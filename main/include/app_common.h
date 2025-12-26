/**
 * @file app_common.h
 * @brief 全局共享数据结构和定义
 */

#ifndef APP_COMMON_H
#define APP_COMMON_H

#include <stdbool.h>
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 系统状态结构体
 * 用于各模块间共享状态信息
 */
typedef struct {
  // 温度状态
  float current_temp; // 当前温度 (°C)
  int target_temp;    // 目标温度 (°C)
  bool is_heating;    // 是否正在加热

  // 电源状态
  bool power_on; // 电源开关状态

  // 时间状态
  int hour;    // 小时 (0-23)
  int minute;  // 分钟 (0-59)
  int second;  // 秒 (0-59)
  int weekday; // 星期 (1-7, 1=周一)

  // 定时器状态
  int timer_duration;    // 设定的加热时长 (分钟)
  int timer_remaining;   // 剩余时间 (分钟)
  char schedule_time[6]; // 预约时间 "HH:MM\0"
  bool schedule_active;  // 预约是否激活

  // WiFi 状态
  bool wifi_connected; // WiFi 是否已连接

} app_state_t;

/**
 * @brief 全局状态变量 (定义在 main.c)
 */
extern app_state_t g_app_state;

/**
 * @brief FreeRTOS 任务优先级定义
 */
#define PRIORITY_TEMP_CONTROL 6 // 最高：温控任务
#define PRIORITY_HTTP_SERVER 5  // HTTP 服务
#define PRIORITY_UI_UPDATE 4    // UI 刷新
#define PRIORITY_SCHEDULER 3    // 定时器
#define PRIORITY_WIFI_MANAGER 2 // WiFi 管理

/**
 * @brief 任务栈大小定义
 */
#define STACK_SIZE_TEMP_CONTROL 4096
#define STACK_SIZE_HTTP_SERVER 8192
#define STACK_SIZE_UI_UPDATE 4096
#define STACK_SIZE_SCHEDULER 2048
#define STACK_SIZE_WIFI_MANAGER 4096

#ifdef __cplusplus
}
#endif

#endif // APP_COMMON_H
