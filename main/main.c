/**
 * @file main.c
 * @brief ESP32-C3 智能加热杯垫 - 主入口
 *
 * FreeRTOS任务架构：
 * - 温控任务 (最高优先级)
 * - HTTP服务器 (高优先级)
 * - UI更新任务 (中优先级)
 * - 调度器 (使用esp_timer)
 * - WiFi管理 (在app_main中初始化)
 */

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <string.h>


// 模块头文件
#include "app_common.h"
#include "http_server.h"
#include "lcd_display.h"
#include "scheduler.h"
#include "soft_rtc.h"
#include "temp_control.h"
#include "wifi_manager.h"


static const char *TAG = "MAIN";

// 全局状态
app_state_t g_app_state = {0};

// WiFi状态回调
static void wifi_status_callback(bool connected) {
  g_app_state.wifi_connected = connected;
  ESP_LOGI(TAG, "WiFi status: %s", connected ? "Connected" : "Disconnected");

  if (connected) {
    // WiFi连接成功后启动mDNS和HTTP服务器
    wifi_manager_start_mdns();
    http_server_start();
  }
}

// UI更新任务
static void ui_update_task(void *arg) {
  TickType_t last_wake_time = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(200); // 200ms刷新周期

  while (1) {
    // 根据当前状态更新UI
    float current_temp = temp_control_get_current_temp();
    int target_temp = temp_control_get_target_temp();
    bool is_heating = temp_control_is_heating();
    bool wifi_ok = wifi_manager_is_connected();

    // 更新全局状态
    g_app_state.current_temp = current_temp;
    g_app_state.target_temp = target_temp;
    g_app_state.is_heating = is_heating;

    // 如果正在配网，显示配网界面
    if (!wifi_ok && lcd_display_get_current_screen() != UI_SCREEN_CONFIG) {
      lcd_display_show_config_screen();
    } else if (wifi_ok &&
               lcd_display_get_current_screen() == UI_SCREEN_CONFIG) {
      // 配网成功，切换到主界面
      lcd_display_set_screen(UI_SCREEN_MAIN);
    }

    // 更新主界面
    if (lcd_display_get_current_screen() == UI_SCREEN_MAIN) {
      lcd_display_update_main(current_temp, target_temp, is_heating, wifi_ok);
    }

    vTaskDelayUntil(&last_wake_time, period);
  }
}

// 定时器超时回调
static void timer_timeout_handler(void) {
  ESP_LOGI(TAG, "Timer expired - heater auto-stopped");
  // 可以在这里添加蜂鸣器或其他提示
}

void app_main(void) {
  ESP_LOGI(TAG, "=================================");
  ESP_LOGI(TAG, "  Smart Cup Warmer Starting...   ");
  ESP_LOGI(TAG, "=================================");

  // 1. 初始化NVS
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);
  ESP_LOGI(TAG, "[1/7] NVS initialized");

  // 2. 初始化LCD显示
  ret = lcd_display_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "LCD init failed!");
  } else {
    lcd_display_show_splash();
    ESP_LOGI(TAG, "[2/7] LCD initialized");
  }
  vTaskDelay(pdMS_TO_TICKS(1500)); // 显示启动画面1.5秒

  // 3. 初始化软件RTC
  ret = soft_rtc_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Soft RTC init failed!");
  } else {
    ESP_LOGI(TAG, "[3/7] Soft RTC initialized");
  }

  // 4. 初始化温控模块
  ret = temp_control_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Temp control init failed!");
  } else {
    ESP_LOGI(TAG, "[4/7] Temp control initialized");
  }

  // 5. 初始化调度器
  ret = scheduler_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Scheduler init failed!");
  } else {
    scheduler_set_timeout_callback(timer_timeout_handler);
    ESP_LOGI(TAG, "[5/7] Scheduler initialized");
  }

  // 6. 显示配网界面
  lcd_display_show_config_screen();

  // 7. 初始化WiFi (会自动从NVS恢复或启动SmartConfig)
  ESP_LOGI(TAG, "[6/7] Starting WiFi...");
  ret = wifi_manager_init(wifi_status_callback);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "WiFi init failed!");
  }

  // 等待WiFi连接 (最多30秒)
  ESP_LOGI(TAG, "Waiting for WiFi connection...");
  for (int i = 0; i < 60; i++) {
    if (wifi_manager_is_connected()) {
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  if (wifi_manager_is_connected()) {
    char ip_str[16];
    wifi_manager_get_ip_string(ip_str);
    ESP_LOGI(TAG, "[7/7] WiFi connected! IP: %s", ip_str);
    ESP_LOGI(TAG, "Access via: http://heated-cup.local or http://%s", ip_str);
  } else {
    ESP_LOGW(TAG, "WiFi not connected, SmartConfig still running...");
  }

  // 8. 启动温控任务
  temp_control_start_task();

  // 9. 创建UI更新任务
  xTaskCreate(ui_update_task, "ui_update", STACK_SIZE_UI_UPDATE, NULL,
              PRIORITY_UI_UPDATE, NULL);

  ESP_LOGI(TAG, "=================================");
  ESP_LOGI(TAG, "    System Ready!                ");
  ESP_LOGI(TAG, "=================================");

  // 主循环 (可用于后续扩展)
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(5000));

    // 可以在这里添加看门狗喂狗、状态打印等
    ESP_LOGD(TAG, "Temp: %.1f°C -> %d°C, Heating: %s",
             temp_control_get_current_temp(), temp_control_get_target_temp(),
             temp_control_is_heating() ? "Yes" : "No");
  }
}
