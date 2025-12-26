/**
 * @file temp_control.h
 * @brief 温控模块 - NTC温度读取 + PID控温 + 加热器PWM控制
 */

#ifndef TEMP_CONTROL_H
#define TEMP_CONTROL_H

#include "esp_err.h"
#include <stdbool.h>


#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 温控状态枚举
 */
typedef enum {
  TEMP_STATE_IDLE,    // 待机
  TEMP_STATE_HEATING, // 加热中
  TEMP_STATE_KEEPING, // 保温中
  TEMP_STATE_ERROR    // 传感器异常
} temp_state_t;

/**
 * @brief 初始化温控模块
 *
 * 初始化ADC、PWM和PID控制器
 *
 * @return esp_err_t ESP_OK 成功
 */
esp_err_t temp_control_init(void);

/**
 * @brief 启动温控任务
 *
 * 创建FreeRTOS任务进行周期性温控
 */
void temp_control_start_task(void);

/**
 * @brief 设置电源开关
 *
 * @param on true=开启加热, false=关闭加热
 */
void temp_control_set_power(bool on);

/**
 * @brief 获取电源状态
 *
 * @return true 加热开启
 * @return false 加热关闭
 */
bool temp_control_get_power(void);

/**
 * @brief 设置目标温度
 *
 * @param temp 目标温度 (30-90°C)
 */
void temp_control_set_target_temp(int temp);

/**
 * @brief 获取目标温度
 *
 * @return int 目标温度
 */
int temp_control_get_target_temp(void);

/**
 * @brief 获取当前温度
 *
 * @return float 当前温度 (°C)
 */
float temp_control_get_current_temp(void);

/**
 * @brief 检查是否正在加热
 *
 * @return true 正在加热
 * @return false 未加热
 */
bool temp_control_is_heating(void);

/**
 * @brief 获取温控状态
 *
 * @return temp_state_t 当前状态
 */
temp_state_t temp_control_get_state(void);

/**
 * @brief 检查NTC传感器是否正常
 *
 * @return true 正常
 * @return false 异常
 */
bool temp_control_is_sensor_ok(void);

#ifdef __cplusplus
}
#endif

#endif // TEMP_CONTROL_H
