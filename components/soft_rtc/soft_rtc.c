/**
 * @file soft_rtc.c
 * @brief 软件RTC模块实现 - 使用esp_timer维护系统时间
 */

#include "soft_rtc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>


static const char *TAG = "SoftRTC";

// 当前时间
static rtc_time_t s_current_time = {
    .year = 2025,
    .month = 1,
    .day = 1,
    .hour = 0,
    .minute = 0,
    .second = 0,
    .weekday = 3 // 2025-01-01 是周三
};

// 互斥锁保护时间访问
static SemaphoreHandle_t s_time_mutex = NULL;

// 定时器句柄
static esp_timer_handle_t s_rtc_timer = NULL;

// 星期字符串表
static const char *s_weekday_en[] = {"",    "Mon", "Tue", "Wed",
                                     "Thu", "Fri", "Sat", "Sun"};
static const char *s_weekday_cn[] = {"",     "周一", "周二", "周三",
                                     "周四", "周五", "周六", "周日"};

/**
 * @brief 判断是否为闰年
 */
static bool is_leap_year(int year) {
  return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

/**
 * @brief 获取某月的天数
 */
static int days_in_month(int year, int month) {
  static const int days[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2 && is_leap_year(year)) {
    return 29;
  }
  return days[month];
}

/**
 * @brief 定时器回调 - 每秒更新时间
 */
static void rtc_timer_callback(void *arg) {
  if (xSemaphoreTake(s_time_mutex, 0) != pdTRUE) {
    return; // 无法获取锁，跳过本次
  }

  // 秒进位
  s_current_time.second++;
  if (s_current_time.second >= 60) {
    s_current_time.second = 0;
    s_current_time.minute++;

    // 分进位
    if (s_current_time.minute >= 60) {
      s_current_time.minute = 0;
      s_current_time.hour++;

      // 时进位
      if (s_current_time.hour >= 24) {
        s_current_time.hour = 0;
        s_current_time.day++;

        // 星期递增
        s_current_time.weekday++;
        if (s_current_time.weekday > 7) {
          s_current_time.weekday = 1;
        }

        // 日进位
        if (s_current_time.day >
            days_in_month(s_current_time.year, s_current_time.month)) {
          s_current_time.day = 1;
          s_current_time.month++;

          // 月进位
          if (s_current_time.month > 12) {
            s_current_time.month = 1;
            s_current_time.year++;
          }
        }
      }
    }
  }

  xSemaphoreGive(s_time_mutex);
}

esp_err_t soft_rtc_init(void) {
  // 创建互斥锁
  s_time_mutex = xSemaphoreCreateMutex();
  if (s_time_mutex == NULL) {
    ESP_LOGE(TAG, "Failed to create mutex");
    return ESP_FAIL;
  }

  // 创建1秒周期定时器
  esp_timer_create_args_t timer_args = {.callback = rtc_timer_callback,
                                        .arg = NULL,
                                        .dispatch_method = ESP_TIMER_TASK,
                                        .name = "soft_rtc"};

  esp_err_t err = esp_timer_create(&timer_args, &s_rtc_timer);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create timer: %s", esp_err_to_name(err));
    return err;
  }

  // 启动定时器 (1秒 = 1000000微秒)
  err = esp_timer_start_periodic(s_rtc_timer, 1000000);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start timer: %s", esp_err_to_name(err));
    return err;
  }

  ESP_LOGI(TAG, "Soft RTC initialized");
  return ESP_OK;
}

esp_err_t soft_rtc_set_time(const rtc_time_t *time) {
  if (time == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  if (xSemaphoreTake(s_time_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  memcpy(&s_current_time, time, sizeof(rtc_time_t));

  // 验证并修正星期范围
  if (s_current_time.weekday < 1 || s_current_time.weekday > 7) {
    s_current_time.weekday = 1;
  }

  xSemaphoreGive(s_time_mutex);

  ESP_LOGI(TAG, "Time set: %04d-%02d-%02d %02d:%02d:%02d (weekday=%d)",
           time->year, time->month, time->day, time->hour, time->minute,
           time->second, time->weekday);

  return ESP_OK;
}

esp_err_t soft_rtc_get_time(rtc_time_t *time) {
  if (time == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  if (xSemaphoreTake(s_time_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  memcpy(time, &s_current_time, sizeof(rtc_time_t));

  xSemaphoreGive(s_time_mutex);
  return ESP_OK;
}

void soft_rtc_get_time_string(char *buf, int buf_size, int format) {
  if (buf == NULL || buf_size < 6) {
    return;
  }

  rtc_time_t t;
  soft_rtc_get_time(&t);

  switch (format) {
  case 0: // HH:MM
    snprintf(buf, buf_size, "%02d:%02d", t.hour, t.minute);
    break;
  case 1: // HH:MM:SS
    snprintf(buf, buf_size, "%02d:%02d:%02d", t.hour, t.minute, t.second);
    break;
  case 2: // YYYY-MM-DD HH:MM:SS
    snprintf(buf, buf_size, "%04d-%02d-%02d %02d:%02d:%02d", t.year, t.month,
             t.day, t.hour, t.minute, t.second);
    break;
  default:
    snprintf(buf, buf_size, "%02d:%02d", t.hour, t.minute);
    break;
  }
}

const char *soft_rtc_get_weekday_string(int weekday, bool chinese) {
  if (weekday < 1 || weekday > 7) {
    weekday = 1;
  }
  return chinese ? s_weekday_cn[weekday] : s_weekday_en[weekday];
}
