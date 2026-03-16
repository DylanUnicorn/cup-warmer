/**
 * @file button.h
 * @brief 按键管理模块 - 用于UI界面切换
 */

#ifndef BUTTON_H
#define BUTTON_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 按键GPIO定义
#define BUTTON_1_GPIO 4
#define BUTTON_2_GPIO 5
#define BUTTON_3_GPIO 9

// 按键事件类型
typedef enum {
    BUTTON_EVENT_NONE = 0,
    BUTTON_EVENT_1_PRESSED,
    BUTTON_EVENT_2_PRESSED,
    BUTTON_EVENT_3_PRESSED
} button_event_t;

// 按键回调函数类型
typedef void (*button_callback_t)(button_event_t event);

/**
 * @brief 初始化按键模块
 * 
 * @return esp_err_t ESP_OK 成功
 */
esp_err_t button_init(void);

/**
 * @brief 注册按键回调函数
 * 
 * @param callback 按键事件回调函数
 * @return esp_err_t ESP_OK 成功
 */
esp_err_t button_register_callback(button_callback_t callback);

/**
 * @brief 获取按键状态
 * 
 * @param button_num 按键编号 (1-3)
 * @return bool true=按下, false=未按下
 */
bool button_is_pressed(int button_num);

#ifdef __cplusplus
}
#endif

#endif // BUTTON_H
