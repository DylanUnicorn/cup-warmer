/**
 * @file scheduler.c
 * @brief 定时与预约模块实现
 */

#include "scheduler.h"
#include "sdkconfig.h"
#include "soft_rtc.h"
#include "temp_control.h"


#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>


static const char *TAG = "Scheduler";

// 默认最大加热时间 (从Kconfig读取)
#ifndef CONFIG_MAX_HEATING_TIME_MINUTES
#define CONFIG_MAX_HEATING_TIME_MINUTES 240 // 4小时
#endif

// 预热时间估算 (从Kconfig读取)
#ifndef CONFIG_PREHEAT_TIME_MINUTES
#define CONFIG_PREHEAT_TIME_MINUTES 5
#endif

// ============================================================================
// 静态变量
// ============================================================================
static int s_timer_duration = 60; // 设定的加热时长 (分钟)
static int s_timer_remaining = 0; // 剩余时间 (分钟)
static int s_timer_seconds = 0;   // 剩余秒数 (内部计时用)
static bool s_timer_running = false;

static char s_schedule_time[8] = ""; // 预约时间 "HH:MM"
static bool s_schedule_active = false;

static scheduler_state_t s_state = SCHED_STATE_IDLE;
static SemaphoreHandle_t s_mutex = NULL;

static void (*s_timeout_callback)(void) = NULL;

// 1秒定时器
static esp_timer_handle_t s_timer = NULL;

// ============================================================================
// 解析预约时间
// ============================================================================
static bool parse_schedule_time(const char *time_str, int *hour, int *minute) {
  if (time_str == NULL || strlen(time_str) < 5) {
    return false;
  }

  if (sscanf(time_str, "%d:%d", hour, minute) == 2) {
    if (*hour >= 0 && *hour <= 23 && *minute >= 0 && *minute <= 59) {
      return true;
    }
  }
  return false;
}

// ============================================================================
// 1秒定时器回调
// ============================================================================
static void scheduler_timer_callback(void *arg) {
  if (xSemaphoreTake(s_mutex, 0) != pdTRUE) {
    return;
  }

  // 倒计时逻辑
  if (s_timer_running && temp_control_get_power()) {
    s_timer_seconds--;
    if (s_timer_seconds <= 0) {
      s_timer_running = false;
      s_timer_remaining = 0;
      s_state = SCHED_STATE_TIMEOUT;

      ESP_LOGI(TAG, "Timer expired, turning off heater");
      temp_control_set_power(false);

      if (s_timeout_callback) {
        s_timeout_callback();
      }
    } else {
      s_timer_remaining = (s_timer_seconds + 59) / 60; // 向上取整
    }
  }

  // 预约检查逻辑
  if (s_schedule_active && !temp_control_get_power()) {
    int sched_hour, sched_minute;
    if (parse_schedule_time(s_schedule_time, &sched_hour, &sched_minute)) {
      rtc_time_t now;
      soft_rtc_get_time(&now);

      // 计算当前时间的分钟数 和 预约时间的分钟数
      int now_minutes = now.hour * 60 + now.minute;
      int sched_minutes = sched_hour * 60 + sched_minute;

      // 考虑预热时间
      int start_minutes = sched_minutes - CONFIG_PREHEAT_TIME_MINUTES;
      if (start_minutes < 0) {
        start_minutes += 24 * 60; // 跨天处理
      }

      // 检查是否应该启动
      if (now_minutes == start_minutes) {
        ESP_LOGI(
            TAG,
            "Schedule triggered, starting heater (preheat %d min before %s)",
            CONFIG_PREHEAT_TIME_MINUTES, s_schedule_time);

        temp_control_set_power(true);
        s_schedule_active = false; // 触发后取消预约

        // 启动倒计时
        s_timer_running = true;
        s_timer_seconds = s_timer_duration * 60;
        s_timer_remaining = s_timer_duration;
        s_state = SCHED_STATE_TIMER_RUNNING;
      }
    }
  }

  xSemaphoreGive(s_mutex);
}

// ============================================================================
// 公开接口实现
// ============================================================================

esp_err_t scheduler_init(void) {
  s_mutex = xSemaphoreCreateMutex();
  if (s_mutex == NULL) {
    return ESP_FAIL;
  }

  // 创建1秒周期定时器
  esp_timer_create_args_t timer_args = {.callback = scheduler_timer_callback,
                                        .arg = NULL,
                                        .dispatch_method = ESP_TIMER_TASK,
                                        .name = "scheduler"};

  esp_err_t err = esp_timer_create(&timer_args, &s_timer);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create timer: %s", esp_err_to_name(err));
    return err;
  }

  // 启动定时器
  err = esp_timer_start_periodic(s_timer, 1000000); // 1秒
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start timer: %s", esp_err_to_name(err));
    return err;
  }

  ESP_LOGI(TAG,
           "Scheduler initialized. Max heating time: %d min, Preheat: %d min",
           CONFIG_MAX_HEATING_TIME_MINUTES, CONFIG_PREHEAT_TIME_MINUTES);

  return ESP_OK;
}

void scheduler_start_task(void) {
  // 调度器使用定时器而非独立任务
  ESP_LOGI(TAG, "Scheduler running via esp_timer");
}

void scheduler_set_timer_duration(int minutes) {
  // 限制最大时间
  if (minutes < 1)
    minutes = 1;
  if (minutes > CONFIG_MAX_HEATING_TIME_MINUTES) {
    minutes = CONFIG_MAX_HEATING_TIME_MINUTES;
  }

  xSemaphoreTake(s_mutex, portMAX_DELAY);
  s_timer_duration = minutes;

  // 如果正在加热，重置倒计时
  if (temp_control_get_power()) {
    s_timer_seconds = minutes * 60;
    s_timer_remaining = minutes;
    s_timer_running = true;
    s_state = SCHED_STATE_TIMER_RUNNING;
  }

  xSemaphoreGive(s_mutex);
  ESP_LOGI(TAG, "Timer duration set to %d minutes", minutes);
}

int scheduler_get_timer_remaining(void) { return s_timer_remaining; }

void scheduler_set_schedule_time(const char *time_str) {
  if (time_str == NULL) {
    return;
  }

  int hour, minute;
  if (!parse_schedule_time(time_str, &hour, &minute)) {
    ESP_LOGW(TAG, "Invalid schedule time format: %s", time_str);
    return;
  }

  xSemaphoreTake(s_mutex, portMAX_DELAY);
  snprintf(s_schedule_time, sizeof(s_schedule_time), "%02d:%02d", hour, minute);
  s_schedule_active = true;
  s_state = SCHED_STATE_SCHEDULED;
  xSemaphoreGive(s_mutex);

  ESP_LOGI(TAG, "Schedule set: %s (will preheat %d min before)",
           s_schedule_time, CONFIG_PREHEAT_TIME_MINUTES);
}

void scheduler_get_schedule_time(char *buf, int buf_size) {
  if (buf == NULL || buf_size < 6) {
    return;
  }

  xSemaphoreTake(s_mutex, portMAX_DELAY);
  strncpy(buf, s_schedule_time, buf_size - 1);
  buf[buf_size - 1] = '\0';
  xSemaphoreGive(s_mutex);
}

void scheduler_cancel_schedule(void) {
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  s_schedule_active = false;
  s_schedule_time[0] = '\0';
  if (s_state == SCHED_STATE_SCHEDULED) {
    s_state = SCHED_STATE_IDLE;
  }
  xSemaphoreGive(s_mutex);
  ESP_LOGI(TAG, "Schedule cancelled");
}

void scheduler_start_timer(void) {
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  s_timer_running = true;
  s_timer_seconds = s_timer_duration * 60;
  s_timer_remaining = s_timer_duration;
  s_state = SCHED_STATE_TIMER_RUNNING;
  xSemaphoreGive(s_mutex);
  ESP_LOGI(TAG, "Timer started: %d minutes", s_timer_duration);
}

void scheduler_stop_timer(void) {
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  s_timer_running = false;
  s_timer_seconds = 0;
  s_timer_remaining = 0;
  if (s_state == SCHED_STATE_TIMER_RUNNING || s_state == SCHED_STATE_TIMEOUT) {
    s_state = SCHED_STATE_IDLE;
  }
  xSemaphoreGive(s_mutex);
  ESP_LOGI(TAG, "Timer stopped");
}

scheduler_state_t scheduler_get_state(void) { return s_state; }

void scheduler_set_timeout_callback(void (*callback)(void)) {
  s_timeout_callback = callback;
}
