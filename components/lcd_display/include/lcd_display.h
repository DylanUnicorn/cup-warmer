/**
 * @file lcd_display.h
 * @brief LCD显示模块 - ST7735S驱动 (LovyanGFX)
 */

#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include "esp_err.h"
#include <stdbool.h>


#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 显示UI界面类型
 */
typedef enum {
  UI_SCREEN_MAIN,      // 主界面 (温度+时间)
  UI_SCREEN_MENU,      // 菜单界面
  UI_SCREEN_CONFIG,    // 配网提示界面
  UI_SCREEN_TIMER,     // 定时加热界面
  UI_SCREEN_SCHEDULE,  // 预约加热界面
  UI_SCREEN_REMINDER,  // 喝水提醒界面
} ui_screen_t;

/**
 * @brief 初始化LCD显示
 *
 * @return esp_err_t ESP_OK 成功
 */
esp_err_t lcd_display_init(void);

/**
 * @brief 设置屏幕亮度
 *
 * @param brightness 亮度 (0-255)
 */
void lcd_display_set_brightness(uint8_t brightness);

/**
 * @brief 更新主界面
 *
 * 显示当前温度、目标温度、时间、星期、WiFi状态
 *
 * @param current_temp 当前温度
 * @param target_temp 目标温度
 * @param is_heating 是否加热中
 * @param wifi_connected WiFi是否已连接
 */
void lcd_display_update_main(float current_temp, int target_temp,
                             bool is_heating, bool wifi_connected);

/**
 * @brief 显示菜单界面
 *
 * @param center_index 中心项索引
 */
void lcd_display_show_menu(int center_index);

/**
 * @brief 显示配网提示界面
 */
void lcd_display_show_config_screen(void);

/**
 * @brief 显示启动画面
 */
void lcd_display_show_splash(void);

/**
 * @brief 获取当前显示的界面
 *
 * @return ui_screen_t 当前界面
 */
ui_screen_t lcd_display_get_current_screen(void);

/**
 * @brief 设置当前显示的界面
 *
 * @param screen 目标界面
 */
void lcd_display_set_screen(ui_screen_t screen);

/**
 * @brief 菜单导航向下
 */
void lcd_display_menu_next(void);

/**
 * @brief 获取当前菜单索引
 *
 * @return int 菜单项索引
 */
int lcd_display_get_menu_index(void);

/**
 * @brief 显示定时加热界面
 *
 * @param minutes 定时时长（分钟）
 * @param is_editing 是否处于编辑状态
 */
void lcd_display_show_timer(int minutes, bool is_editing);

/**
 * @brief 显示预约加热界面
 *
 * @param hour 小时
 * @param minute 分钟
 * @param is_editing 是否处于编辑状态
 * @param edit_field 编辑字段 (0=hour, 1=minute)
 */
void lcd_display_show_schedule(int hour, int minute, bool is_editing, int edit_field);

/**
 * @brief 显示喝水提醒界面
 *
 * @param interval_minutes 提醒间隔（分钟）
 * @param enabled 是否启用
 * @param is_editing 是否处于编辑状态
 */
void lcd_display_show_reminder(int interval_minutes, bool enabled, bool is_editing);

#ifdef __cplusplus
}
#endif

#endif // LCD_DISPLAY_H
