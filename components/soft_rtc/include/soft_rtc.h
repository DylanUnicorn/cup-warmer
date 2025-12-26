/**
 * @file soft_rtc.h
 * @brief 软件RTC模块 - 使用ESP32内部定时器维护系统时间
 */

#ifndef SOFT_RTC_H
#define SOFT_RTC_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief RTC时间结构体
 */
typedef struct {
  int year;    // 年 (如 2025)
  int month;   // 月 (1-12)
  int day;     // 日 (1-31)
  int hour;    // 小时 (0-23)
  int minute;  // 分钟 (0-59)
  int second;  // 秒 (0-59)
  int weekday; // 星期 (1-7, 1=周一, 7=周日)
} rtc_time_t;

/**
 * @brief 初始化软件RTC
 *
 * 启动1秒周期定时器维护时间
 *
 * @return esp_err_t ESP_OK 成功
 */
esp_err_t soft_rtc_init(void);

/**
 * @brief 设置RTC时间
 *
 * @param time 要设置的时间
 * @return esp_err_t ESP_OK 成功
 */
esp_err_t soft_rtc_set_time(const rtc_time_t *time);

/**
 * @brief 获取当前RTC时间
 *
 * @param time 输出时间结构体
 * @return esp_err_t ESP_OK 成功
 */
esp_err_t soft_rtc_get_time(rtc_time_t *time);

/**
 * @brief 获取格式化的时间字符串
 *
 * @param buf 输出缓冲区
 * @param buf_size 缓冲区大小
 * @param format 格式 (0=HH:MM, 1=HH:MM:SS, 2=YYYY-MM-DD HH:MM:SS)
 */
void soft_rtc_get_time_string(char *buf, int buf_size, int format);

/**
 * @brief 获取星期字符串
 *
 * @param weekday 星期数值 (1-7)
 * @param chinese 是否返回中文
 * @return const char* 星期字符串
 */
const char *soft_rtc_get_weekday_string(int weekday, bool chinese);

#ifdef __cplusplus
}
#endif

#endif // SOFT_RTC_H
