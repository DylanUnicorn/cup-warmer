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
#include "button.h"
#include "voice_module.h"

static const char *TAG = "MAIN";

// 全局状态
app_state_t g_app_state = {0};

// 配网超时标志
static bool g_smartconfig_timeout = false;

// ============================================================================
// UI状态管理
// ============================================================================

// 屏幕历史栈（用于返回功能）
#define SCREEN_STACK_SIZE 5
static ui_screen_t s_screen_stack[SCREEN_STACK_SIZE];
static int s_stack_top = -1;

// 定时加热设置
static int s_timer_minutes = 30; // 默认30分钟
static const int s_timer_options[] = {15, 30, 60, 120}; // 可选时长
static const int s_timer_options_count = sizeof(s_timer_options) / sizeof(s_timer_options[0]);
static int s_timer_option_index = 1; // 默认选中30分钟
static bool s_timer_editing = false;

// 预约加热设置
static int s_schedule_hour = 8;
static int s_schedule_minute = 0;
static int s_schedule_edit_field = 0; // 0=小时, 1=分钟
static bool s_schedule_editing = false;

// 喝水提醒设置
static int s_reminder_interval = 60; // 默认60分钟
static const int s_reminder_options[] = {30, 60, 90, 120};
static const int s_reminder_options_count = sizeof(s_reminder_options) / sizeof(s_reminder_options[0]);
static int s_reminder_option_index = 1; // 默认选中60分钟
static bool s_reminder_enabled = false;
static bool s_reminder_editing = false;

// 主界面目标面板温度预设 (用户设定的是面板温度，水温 ≈ 面板 - 30°C)
static const int s_temp_presets[] = {60, 70, 80, 90, 100, 110, 120, 130, 141};
static const int s_temp_presets_count = sizeof(s_temp_presets) / sizeof(s_temp_presets[0]);
static int s_temp_preset_index = 2; // 默认选中80°C

// 屏幕栈操作
static void push_screen(ui_screen_t screen) {
  if (s_stack_top < SCREEN_STACK_SIZE - 1) {
    s_stack_top++;
    s_screen_stack[s_stack_top] = screen;
  }
}

static ui_screen_t pop_screen(void) {
  if (s_stack_top >= 0) {
    return s_screen_stack[s_stack_top--];
  }
  return UI_SCREEN_MAIN; // 默认返回主界面
}

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

// 按键事件处理回调
// 按键1 (GPIO4): 导航/选择 (NAV)
// 按键2 (GPIO5): 确认/进入 (CONFIRM)
// 按键3 (GPIO9): 返回/退出 (BACK)
static void button_event_handler(button_event_t event) {
  ui_screen_t current_screen = lcd_display_get_current_screen();
  
  // 每次按键按下都播放第一个音频
  voice_play(1);
  
  switch (current_screen) {
    // ========================================
    // 主界面
    // ========================================
    case UI_SCREEN_MAIN:
      if (event == BUTTON_EVENT_1_PRESSED) {
        // 按键1：循环调整目标温度
        s_temp_preset_index = (s_temp_preset_index + 1) % s_temp_presets_count;
        int new_temp = s_temp_presets[s_temp_preset_index];
        temp_control_set_target_temp(new_temp);
        ESP_LOGI(TAG, "Target temperature changed: %d°C", new_temp);
      } else if (event == BUTTON_EVENT_2_PRESSED) {
        // 按键2：进入菜单
        ESP_LOGI(TAG, "Enter menu from main screen");
        push_screen(current_screen);
        lcd_display_set_screen(UI_SCREEN_MENU);
      } else if (event == BUTTON_EVENT_3_PRESSED) {
        // 按键3：切换加热开关
        bool current_power = temp_control_get_power();
        bool new_power = !current_power;
        temp_control_set_power(new_power);
        ESP_LOGI(TAG, "Heating power toggled: %s", new_power ? "ON" : "OFF");
      }
      break;
      
    // ========================================
    // 菜单界面
    // ========================================
    case UI_SCREEN_MENU:
      if (event == BUTTON_EVENT_1_PRESSED) {
        // 导航键：菜单向下滚动
        ESP_LOGI(TAG, "Menu navigation: next");
        lcd_display_menu_next();
      } else if (event == BUTTON_EVENT_2_PRESSED) {
        // 确认键：进入选中的功能界面
        int menu_index = lcd_display_get_menu_index();
        ESP_LOGI(TAG, "Menu select: index=%d", menu_index);
        
        push_screen(current_screen);
        
        switch (menu_index) {
          case 0: // 定时加热
            s_timer_editing = true;
            lcd_display_set_screen(UI_SCREEN_TIMER);
            break;
          case 1: // 预约加热
            s_schedule_editing = true;
            s_schedule_edit_field = 0; // 从小时开始编辑
            lcd_display_set_screen(UI_SCREEN_SCHEDULE);
            break;
          case 2: // 喝水提醒
            s_reminder_editing = true;
            lcd_display_set_screen(UI_SCREEN_REMINDER);
            break;
        }
      } else if (event == BUTTON_EVENT_3_PRESSED) {
        // 返回键：返回主界面
        ESP_LOGI(TAG, "Back to main from menu");
        lcd_display_set_screen(pop_screen());
      }
      break;
      
    // ========================================
    // 定时加热界面
    // ========================================
    case UI_SCREEN_TIMER:
      if (event == BUTTON_EVENT_1_PRESSED && s_timer_editing) {
        // 导航键：切换时长选项
        s_timer_option_index = (s_timer_option_index + 1) % s_timer_options_count;
        s_timer_minutes = s_timer_options[s_timer_option_index];
        ESP_LOGI(TAG, "Timer option changed: %d min", s_timer_minutes);
      } else if (event == BUTTON_EVENT_2_PRESSED) {
        // 确认键：保存设置
        ESP_LOGI(TAG, "Timer confirmed: %d min", s_timer_minutes);
        s_timer_editing = false;
        // TODO: 启动定时器任务
        // 返回菜单
        lcd_display_set_screen(pop_screen());
      } else if (event == BUTTON_EVENT_3_PRESSED) {
        // 返回键：取消编辑
        ESP_LOGI(TAG, "Timer cancelled");
        s_timer_editing = false;
        lcd_display_set_screen(pop_screen());
      }
      break;
      
    // ========================================
    // 预约加热界面
    // ========================================
    case UI_SCREEN_SCHEDULE:
      if (event == BUTTON_EVENT_1_PRESSED && s_schedule_editing) {
        // 导航键：增加当前字段的值
        if (s_schedule_edit_field == 0) {
          // 小时 (0-23)
          s_schedule_hour = (s_schedule_hour + 1) % 24;
          ESP_LOGI(TAG, "Schedule hour: %02d", s_schedule_hour);
        } else {
          // 分钟 (0, 15, 30, 45)
          s_schedule_minute = (s_schedule_minute + 15) % 60;
          ESP_LOGI(TAG, "Schedule minute: %02d", s_schedule_minute);
        }
      } else if (event == BUTTON_EVENT_2_PRESSED && s_schedule_editing) {
        // 确认键：切换到下一个字段或保存
        if (s_schedule_edit_field == 0) {
          // 从小时切换到分钟
          s_schedule_edit_field = 1;
          ESP_LOGI(TAG, "Schedule: editing minute");
        } else {
          // 保存设置
          ESP_LOGI(TAG, "Schedule confirmed: %02d:%02d", s_schedule_hour, s_schedule_minute);
          s_schedule_editing = false;
          s_schedule_edit_field = 0;
          // TODO: 设置预约加热任务
          // 返回菜单
          lcd_display_set_screen(pop_screen());
        }
      } else if (event == BUTTON_EVENT_3_PRESSED) {
        // 返回键：取消编辑或返回上一字段
        if (s_schedule_edit_field == 1) {
          // 从分钟返回到小时
          s_schedule_edit_field = 0;
          ESP_LOGI(TAG, "Schedule: back to hour");
        } else {
          // 取消编辑，返回菜单
          ESP_LOGI(TAG, "Schedule cancelled");
          s_schedule_editing = false;
          s_schedule_edit_field = 0;
          lcd_display_set_screen(pop_screen());
        }
      }
      break;
      
    // ========================================
    // 喝水提醒界面
    // ========================================
    case UI_SCREEN_REMINDER:
      if (event == BUTTON_EVENT_1_PRESSED && s_reminder_editing) {
        // 导航键：切换开关状态或间隔时间
        s_reminder_enabled = !s_reminder_enabled;
        ESP_LOGI(TAG, "Reminder toggled: %s", s_reminder_enabled ? "ON" : "OFF");
        
        // 如果开启，循环切换间隔
        if (s_reminder_enabled) {
          s_reminder_option_index = (s_reminder_option_index + 1) % s_reminder_options_count;
          s_reminder_interval = s_reminder_options[s_reminder_option_index];
          ESP_LOGI(TAG, "Reminder interval: %d min", s_reminder_interval);
        }
      } else if (event == BUTTON_EVENT_2_PRESSED) {
        // 确认键：保存设置
        ESP_LOGI(TAG, "Reminder confirmed: %s, interval=%d min", 
                 s_reminder_enabled ? "ON" : "OFF", s_reminder_interval);
        s_reminder_editing = false;
        // TODO: 启动/停止提醒任务
        // 返回菜单
        lcd_display_set_screen(pop_screen());
      } else if (event == BUTTON_EVENT_3_PRESSED) {
        // 返回键：取消编辑
        ESP_LOGI(TAG, "Reminder cancelled");
        s_reminder_editing = false;
        lcd_display_set_screen(pop_screen());
      }
      break;
      
    // ========================================
    // 配网界面（不响应按键）
    // ========================================
    case UI_SCREEN_CONFIG:
      // 配网期间暂不响应按键
      break;
      
    default:
      break;
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

    // 如果正在配网且未超时，显示配网界面
    if (!wifi_ok && !g_smartconfig_timeout && 
        lcd_display_get_current_screen() != UI_SCREEN_CONFIG) {
      lcd_display_show_config_screen();
    } else if (wifi_ok &&
               lcd_display_get_current_screen() == UI_SCREEN_CONFIG) {
      // 配网成功，切换到主界面
      lcd_display_set_screen(UI_SCREEN_MAIN);
    } else if (!wifi_ok && g_smartconfig_timeout &&
               lcd_display_get_current_screen() == UI_SCREEN_CONFIG) {
      // 配网超时，切换到主界面（即使WiFi未连接）
      ESP_LOGI(TAG, "SmartConfig timeout, switching to main screen");
      lcd_display_set_screen(UI_SCREEN_MAIN);
    }

    // 更新当前界面
    ui_screen_t current_screen = lcd_display_get_current_screen();
    switch (current_screen) {
      case UI_SCREEN_MAIN:
        lcd_display_update_main(current_temp, target_temp, is_heating, wifi_ok);
        break;
        
      case UI_SCREEN_MENU: {
        int menu_index = lcd_display_get_menu_index();
        lcd_display_show_menu(menu_index);
        break;
      }
      
      case UI_SCREEN_TIMER:
        lcd_display_show_timer(s_timer_minutes, s_timer_editing);
        break;
        
      case UI_SCREEN_SCHEDULE:
        lcd_display_show_schedule(s_schedule_hour, s_schedule_minute, 
                                  s_schedule_editing, s_schedule_edit_field);
        break;
        
      case UI_SCREEN_REMINDER:
        lcd_display_show_reminder(s_reminder_interval, s_reminder_enabled, 
                                 s_reminder_editing);
        break;
        
      case UI_SCREEN_CONFIG:
        // 配网界面已由上面的逻辑处理
        break;
        
      default:
        break;
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
  ESP_LOGI(TAG, "[1/8] NVS initialized");

  // 2. 初始化LCD显示
  ret = lcd_display_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "LCD init failed!");
  } else {
    lcd_display_show_splash();
    ESP_LOGI(TAG, "[2/8] LCD initialized");
  }
  vTaskDelay(pdMS_TO_TICKS(1500)); // 显示启动画面1.5秒

  // 3. 初始化软件RTC
  ret = soft_rtc_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Soft RTC init failed!");
  } else {
    ESP_LOGI(TAG, "[3/8] Soft RTC initialized");
  }

  // 4. 初始化温控模块
  ret = temp_control_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Temp control init failed!");
  } else {
    ESP_LOGI(TAG, "[4/8] Temp control initialized");
  }

  // 5. 初始化调度器
  ret = scheduler_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Scheduler init failed!");
  } else {
    scheduler_set_timeout_callback(timer_timeout_handler);
    ESP_LOGI(TAG, "[5/8] Scheduler initialized");
  }

  // 6. 初始化按键模块
  ret = button_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Button init failed!");
  } else {
    button_register_callback(button_event_handler);
    ESP_LOGI(TAG, "[6/8] Button initialized (GPIO 4, 5, 9)");
  }

  // 6.5 初始化语音模块
  ret = voice_module_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Voice module init failed!");
  } else {
    ESP_LOGI(TAG, "[6.5/8] Voice module initialized (CH7800, GPIO1)");
  }

  // 7. 显示配网界面
  lcd_display_show_config_screen();

  // 8. 初始化WiFi (会自动从NVS恢复或启动SmartConfig)
  ESP_LOGI(TAG, "[7/8] Starting WiFi...");
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
    ESP_LOGI(TAG, "[8/8] WiFi connected! IP: %s", ip_str);
    ESP_LOGI(TAG, "Access via: http://heated-cup.local or http://%s", ip_str);
  } else {
    ESP_LOGW(TAG, "WiFi not connected after 30s, SmartConfig timeout");
    g_smartconfig_timeout = true; // 设置超时标志
  }

  // 9. 启动温控任务
  temp_control_start_task();

  // 10. 创建UI更新任务
  xTaskCreate(ui_update_task, "ui_update", STACK_SIZE_UI_UPDATE, NULL,
              PRIORITY_UI_UPDATE, NULL);

  ESP_LOGI(TAG, "=========================================");
  ESP_LOGI(TAG, "    System Ready!                        ");
  ESP_LOGI(TAG, "  🔥 Temperature Control: DYNAMIC      ");
  ESP_LOGI(TAG, "  🎯 Panel Target: %d°C (Water: ~%d°C) ", s_temp_presets[s_temp_preset_index], s_temp_presets[s_temp_preset_index] - 30);
  ESP_LOGI(TAG, "  Button 1 (GPIO4): Main Screen          ");
  ESP_LOGI(TAG, "  Button 2 (GPIO5): Menu Screen          ");
  ESP_LOGI(TAG, "  Button 3 (GPIO9): Toggle UI            ");
  ESP_LOGI(TAG, "  NTC Sensor: GPIO0 | Heater: GPIO12     ");
  ESP_LOGI(TAG, "=========================================");

  // 主循环 (可用于后续扩展)
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(5000));

    // 可以在这里添加看门狗喂狗、状态打印等
    float current_temp = temp_control_get_current_temp();
    bool is_heating = temp_control_is_heating();
    
    ESP_LOGI(TAG, "========= Status Monitor =========");
    ESP_LOGI(TAG, "Temperature: %.1f°C (Panel Target: %d°C, Water: ~%.0f°C)", 
             current_temp, temp_control_get_target_temp(),
             current_temp - 30.0f > 0 ? current_temp - 30.0f : 0.0f);
    ESP_LOGI(TAG, "Heater: %s (Dynamic Target Mode)", 
             is_heating ? "ON " : "OFF");
    ESP_LOGI(TAG, "==================================");
  }
}
