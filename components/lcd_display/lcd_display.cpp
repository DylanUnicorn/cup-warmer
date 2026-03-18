/**
 * @file lcd_display.cpp
 * @brief LCD显示模块实现 - ST7735S驱动 (LovyanGFX)
 *
 * 基于 src/src/lcd_driver.cpp 迁移适配
 */

#include "lcd_display.h"
#include "sdkconfig.h"
#include "soft_rtc.h"
#include "temp_control.h"

// Compatibility fixes for LovyanGFX on ESP-IDF 5.x (ESP32-C3)
#if !defined(GPIO_FUNC0_IN_SEL_CFG_REG)
#include "soc/gpio_reg.h"
#include "soc/gpio_struct.h"
#endif

#include <LovyanGFX.hpp>
#include <cstdio>
#include <cstring>

// ============================================================================
// GPIO引脚配置 (从Kconfig读取)
// ============================================================================
#ifndef CONFIG_LCD_PIN_SCLK
#define CONFIG_LCD_PIN_SCLK 2
#endif
#ifndef CONFIG_LCD_PIN_MOSI
#define CONFIG_LCD_PIN_MOSI 3
#endif
#ifndef CONFIG_LCD_PIN_DC
#define CONFIG_LCD_PIN_DC 7
#endif
#ifndef CONFIG_LCD_PIN_CS
#define CONFIG_LCD_PIN_CS 8
#endif
#ifndef CONFIG_LCD_PIN_RST
#define CONFIG_LCD_PIN_RST 6
#endif
#ifndef CONFIG_LCD_PIN_BL
#define CONFIG_LCD_PIN_BL 10
#endif

// ============================================================================
// 字体定义
// ============================================================================
#define BIG_FONT &fonts::FreeSansBold24pt7b
#define SMALL_FONT &fonts::FreeSansBold9pt7b

// ============================================================================
// LovyanGFX 驱动类定义
// ============================================================================
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7735S _panel_instance;
  lgfx::Bus_SPI _bus_instance;
  lgfx::Light_PWM _light_instance;

public:
  LGFX(void) {
    // SPI总线配置
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 20000000; // 20MHz

      cfg.pin_sclk = CONFIG_LCD_PIN_SCLK;
      cfg.pin_mosi = CONFIG_LCD_PIN_MOSI;
      cfg.pin_miso = -1;
      cfg.pin_dc = CONFIG_LCD_PIN_DC;

      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }

    // 面板配置
    {
      auto cfg = _panel_instance.config();

      cfg.pin_cs = CONFIG_LCD_PIN_CS;
      cfg.pin_rst = CONFIG_LCD_PIN_RST;
      cfg.pin_busy = -1;

      cfg.panel_width = 128;
      cfg.panel_height = 160;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.invert = false;
      cfg.rgb_order = false;

      _panel_instance.config(cfg);
    }

    // 背光配置 (低电平有效)
    {
      auto cfg = _light_instance.config();

      cfg.pin_bl = CONFIG_LCD_PIN_BL;
      cfg.invert = true; // 低电平亮
      cfg.freq = 12000;
      cfg.pwm_channel = 7;

      _light_instance.config(cfg);
      _panel_instance.setLight(&_light_instance);
    }

    setPanel(&_panel_instance);
  }
};

// ============================================================================
// 静态实例
// ============================================================================
static LGFX lcd;
static LGFX_Sprite sprite(&lcd);
static ui_screen_t s_current_screen = UI_SCREEN_MAIN;

// ============================================================================
// 菜单项定义
// ============================================================================
struct MenuItem {
  const char *title;
  const char *details;
};

static MenuItem menuItems[] = {
    {"定时加热", "设置加热时长"},
    {"预约加热", "设置加热时间"},
    {"喝水提醒", "间隔提醒"},
    {"WiFi配网", "SmartConfig"},
};
static const int menuCount = sizeof(menuItems) / sizeof(menuItems[0]);
static int s_menu_index = 0; // 当前菜单选中索引

// ============================================================================
// 辅助绘图函数
// ============================================================================

/**
 * @brief 绘制圆角进度条
 */
static void drawProgressBar(int x, int y, int w, int h, int val, int max_val,
                            uint16_t color) {
  // 背景框
  sprite.fillRoundRect(x, y, w, h, h / 2, 0x2124);

  // 前景
  int bar_w = (int)((float)val / max_val * w);
  if (bar_w > w)
    bar_w = w;
  if (bar_w < 2)
    bar_w = 0;

  if (bar_w > 0) {
    sprite.fillRoundRect(x, y, bar_w, h, h / 2, color);
  }
}

/**
 * @brief 绘制菜单卡片
 */
static void drawMenuCard(int y_center, int width, int height, int data_index,
                         bool isFocused) {
  int x_pos = (128 - width) / 2;
  int y_pos = y_center - height / 2;

  // 颜色主题
  uint16_t bgColor = isFocused ? 0xFFFF : 0x2124;
  uint16_t titleColor = isFocused ? 0x0000 : 0xBDF7;
  uint16_t detailColor = isFocused ? 0x4208 : 0x73AE;
  uint16_t iconBgColor = isFocused ? 0xDDD0 : 0x39C7;

  // 圆角背景
  sprite.fillRoundRect(x_pos, y_pos, width, height, 12, bgColor);

  // 图标圆
  int icon_x = x_pos + 20;
  int icon_y = y_center;
  sprite.fillCircle(icon_x, icon_y, isFocused ? 14 : 11, iconBgColor);

  // 图标文字 (取第一个字)
  sprite.setTextColor(titleColor);
  sprite.setFont(&fonts::efontCN_16);
  sprite.setTextDatum(textdatum_t::middle_center);

  char icon_char[4] = {0};
  memcpy(icon_char, menuItems[data_index].title, 3);
  sprite.drawString(icon_char, icon_x, icon_y + 1);

  // 标题和详情
  int text_x = x_pos + 40;
  sprite.setTextDatum(textdatum_t::top_left);

  sprite.setTextColor(titleColor);
  sprite.setFont(&fonts::efontCN_16);
  sprite.drawString(menuItems[data_index].title, text_x,
                    y_pos + (isFocused ? 6 : 4));

  sprite.setTextColor(detailColor);
  sprite.setFont(&fonts::efontCN_14);
  sprite.drawString(menuItems[data_index].details, text_x + 3,
                    y_pos + (isFocused ? 24 : 20));
}

// ============================================================================
// C接口实现
// ============================================================================

extern "C" {

esp_err_t lcd_display_init(void) {
  lcd.init();
  lcd.setBrightness(200);

  // 创建全屏Sprite (128x160, RGB565 ≈ 40KB)
  if (!sprite.createSprite(128, 160)) {
    return ESP_ERR_NO_MEM;
  }

  return ESP_OK;
}

void lcd_display_set_brightness(uint8_t brightness) {
  lcd.setBrightness(brightness);
}

void lcd_display_update_main(float current_temp, int target_temp,
                             bool is_heating, bool wifi_connected) {
  s_current_screen = UI_SCREEN_MAIN;

  // 获取时间
  rtc_time_t rtc_time;
  soft_rtc_get_time(&rtc_time);

  // 获取预估水温
  float water_temp = temp_control_get_water_temp_estimate();

  // 清屏
  sprite.fillScreen(0x0000);

  // ==========================================
  // 顶部状态栏
  // ==========================================
  sprite.setTextColor(0xFFFF);
  sprite.setFont(&fonts::Font0);
  sprite.setCursor(2, 2);

  char date_str[20];
  snprintf(date_str, sizeof(date_str), "%02d-%02d %s %02d:%02d", rtc_time.month,
           rtc_time.day, soft_rtc_get_weekday_string(rtc_time.weekday, false),
           rtc_time.hour, rtc_time.minute);
  sprite.print(date_str);

  // WiFi图标
  sprite.fillCircle(120, 5, 2, wifi_connected ? 0x07E0 : 0xF800);

  // ==========================================
  // 面板温度显示（大字体）
  // ==========================================
  // 标签
  sprite.setFont(&fonts::Font0);
  sprite.setTextDatum(textdatum_t::top_left);
  sprite.setTextColor(0x07FF);
  sprite.drawString("Panel", 5, 16);

  char temp_str[10];
  snprintf(temp_str, sizeof(temp_str), "%.1f", current_temp);

  sprite.setFont(BIG_FONT);
  sprite.setTextDatum(textdatum_t::middle_center);

  // 发光阴影效果
  sprite.setTextColor(0x212F);
  sprite.drawString(temp_str, 64 + 2, 46 + 2);

  // 主文字
  sprite.setTextColor(0xFFFF);
  sprite.drawString(temp_str, 64, 46);

  // 单位 °C
  sprite.setFont(&fonts::Font2);
  sprite.drawString("C", 115, 36);
  sprite.drawCircle(108, 31, 2, 0xFFFF);

  // ==========================================
  // 面板温度进度条 + 目标值
  // ==========================================
  sprite.setFont(&fonts::Font0);
  sprite.setTextDatum(textdatum_t::top_left);
  sprite.setTextColor(0xBDF7);
  sprite.drawString("Set:", 10, 70);
  sprite.setCursor(100, 70);
  sprite.print(target_temp);

  // 进度条 (青色) - 基于室温(25°C)到目标温度的进度
  int progress = 0;
  float baseline = 25.0f;
  if (target_temp > baseline) {
    progress = (int)(((current_temp - baseline) / (target_temp - baseline)) * 100);
  }
  if (progress > 100)
    progress = 100;
  if (progress < 0)
    progress = 0;
  drawProgressBar(10, 80, 108, 6, progress, 100, 0x07FF);

  // ==========================================
  // 预估水温显示
  // ==========================================
  sprite.setFont(&fonts::efontCN_14);
  sprite.setTextDatum(textdatum_t::top_left);
  sprite.setTextColor(0x07E0);
  sprite.setCursor(5, 92);

  char water_str[30];
  snprintf(water_str, sizeof(water_str), "水温: ~%.0f°C", water_temp);
  sprite.print(water_str);

  // 预估水温目标 (不超过100°C)
  sprite.setTextColor(0x73AE);
  sprite.setFont(&fonts::Font0);
  sprite.setCursor(80, 95);
  int water_target = target_temp - 30;
  if (water_target > 100) water_target = 100;
  if (water_target < 0) water_target = 0;
  char water_target_str[20];
  snprintf(water_target_str, sizeof(water_target_str), "(~%d)", water_target);
  sprite.print(water_target_str);

  // ==========================================
  // 加热状态
  // ==========================================
  sprite.setFont(&fonts::efontCN_14);
  sprite.setTextColor(0xFFFF);
  sprite.setTextDatum(textdatum_t::top_left);
  sprite.setCursor(5, 115);

  bool power_on = temp_control_get_power();
  if (!power_on) {
    sprite.setTextColor(0x73AE);
    sprite.print("已关闭");
  } else if (is_heating) {
    sprite.setTextColor(0xFBE0);
    sprite.print("加热中...");
  } else if (current_temp >= target_temp - 2) {
    sprite.setTextColor(0x07E0);
    sprite.print("保温中");
  } else {
    sprite.setTextColor(0xBDF7);
    sprite.print("待机");
  }

  // ==========================================
  // 底部操作提示
  // ==========================================
  sprite.setFont(&fonts::Font0);
  sprite.setTextColor(0x4208);
  sprite.setTextDatum(textdatum_t::bottom_left);
  sprite.drawString("NAV:Temp", 2, 158);
  sprite.setTextDatum(textdatum_t::bottom_center);
  sprite.drawString("OK:Menu", 64, 158);
  sprite.setTextDatum(textdatum_t::bottom_right);
  sprite.drawString("BACK:On/Off", 126, 158);

  // 推送到屏幕
  sprite.pushSprite(0, 0);
}

void lcd_display_show_menu(int center_index) {
  s_current_screen = UI_SCREEN_MENU;

  sprite.fillScreen(0x0000);

  // 标题
  sprite.setTextColor(0x73AE);
  sprite.setFont(&fonts::Font0);
  sprite.setTextDatum(textdatum_t::top_center);
  sprite.drawString("- PRESETS -", 64, 5);

  // 计算循环索引
  int prev_index = (center_index - 1 + menuCount) % menuCount;
  int next_index = (center_index + 1) % menuCount;

  // 绘制三张卡片
  drawMenuCard(35, 114, 30, prev_index, false);  // 上
  drawMenuCard(125, 114, 30, next_index, false); // 下
  drawMenuCard(80, 124, 54, center_index, true); // 中 (焦点)

  sprite.pushSprite(0, 0);
}

void lcd_display_show_config_screen(void) {
  s_current_screen = UI_SCREEN_CONFIG;

  sprite.fillScreen(0x0000);

  sprite.setFont(&fonts::efontCN_16);
  sprite.setTextColor(0xFFFF);
  sprite.setTextDatum(textdatum_t::middle_center);

  sprite.drawString("请使用手机", 64, 50);
  sprite.drawString("进行SmartConfig", 64, 70);
  sprite.drawString("配网", 64, 90);

  sprite.setFont(&fonts::Font0);
  sprite.setTextColor(0x07FF);
  sprite.drawString("Waiting for WiFi...", 64, 130);

  sprite.pushSprite(0, 0);
}

void lcd_display_show_splash(void) {
  sprite.fillScreen(0x0000);

  sprite.setFont(&fonts::efontCN_16);
  sprite.setTextColor(0xFFFF);
  sprite.setTextDatum(textdatum_t::middle_center);

  sprite.drawString("智能加热杯垫", 64, 70);

  sprite.setFont(&fonts::Font2);
  sprite.setTextColor(0x07FF);
  sprite.drawString("Cup Warmer v1.0", 64, 100);

  sprite.pushSprite(0, 0);
}

ui_screen_t lcd_display_get_current_screen(void) { return s_current_screen; }

void lcd_display_set_screen(ui_screen_t screen) { s_current_screen = screen; }

// ============================================================================
// 菜单导航函数
// ============================================================================

void lcd_display_menu_next(void) {
  s_menu_index = (s_menu_index + 1) % menuCount;
}

void lcd_display_menu_prev(void) {
  s_menu_index = (s_menu_index - 1 + menuCount) % menuCount;
}

int lcd_display_get_menu_index(void) {
  return s_menu_index;
}

// ============================================================================
// 新增UI界面显示函数
// ============================================================================

/**
 * @brief 显示定时加热界面
 * @param minutes 定时时长(分钟)
 * @param is_editing 是否处于编辑状态
 */
void lcd_display_show_timer(int minutes, bool is_editing) {
  s_current_screen = UI_SCREEN_TIMER;
  
  sprite.fillScreen(0x0000);
  
  // 标题
  sprite.setTextColor(0x07FF);
  sprite.setFont(&fonts::Font0);
  sprite.setTextDatum(textdatum_t::top_center);
  sprite.drawString("- TIMER MODE -", 64, 5);
  
  // 主标题
  sprite.setFont(&fonts::efontCN_16);
  sprite.setTextColor(0xFFFF);
  sprite.drawString("定时加热", 64, 30);
  
  // 时长显示（中号字体）
  char time_str[20];
  snprintf(time_str, sizeof(time_str), "%d min", minutes);
  
  sprite.setFont(SMALL_FONT);
  if (is_editing) {
    // 编辑状态：黄色高亮
    sprite.setTextColor(0xFFE0);
    sprite.drawString(time_str, 64, 80);
    // 下划线
    sprite.fillRect(30, 95, 68, 2, 0xFFE0);
  } else {
    sprite.setTextColor(0xFFFF);
    sprite.drawString(time_str, 64, 80);
  }
  
  // 提示信息
  sprite.setFont(&fonts::efontCN_14);
  sprite.setTextColor(0x73AE);
  sprite.drawString("可选: 15/30/60/120", 64, 120);
  
  // 操作提示
  sprite.setFont(&fonts::Font0);
  sprite.setTextColor(0xBDF7);
  sprite.setTextDatum(textdatum_t::bottom_left);
  sprite.drawString("NAV:Change", 5, 155);
  sprite.setTextDatum(textdatum_t::bottom_right);
  sprite.drawString("CONF:OK", 123, 155);
  
  sprite.pushSprite(0, 0);
}

/**
 * @brief 显示预约加热界面
 * @param hour 小时
 * @param minute 分钟
 * @param is_editing 是否处于编辑状态
 * @param edit_field 0=小时, 1=分钟
 */
void lcd_display_show_schedule(int hour, int minute, bool is_editing, int edit_field) {
  s_current_screen = UI_SCREEN_SCHEDULE;
  
  sprite.fillScreen(0x0000);
  
  // 标题
  sprite.setTextColor(0x07E0);
  sprite.setFont(&fonts::Font0);
  sprite.setTextDatum(textdatum_t::top_center);
  sprite.drawString("- SCHEDULE MODE -", 64, 5);
  
  // 主标题
  sprite.setFont(&fonts::efontCN_16);
  sprite.setTextColor(0xFFFF);
  sprite.drawString("预约加热", 64, 30);
  
  // 时钟图标（表盘）
  sprite.drawCircle(64, 65, 25, 0x4208);
  sprite.drawCircle(64, 65, 26, 0x4208);
  
  // 刻度点（12点、3点、6点、9点）
  sprite.fillCircle(64, 40, 2, 0x4208);  // 12点
  sprite.fillCircle(89, 65, 2, 0x4208);  // 3点
  sprite.fillCircle(64, 90, 2, 0x4208);  // 6点
  sprite.fillCircle(39, 65, 2, 0x4208);  // 9点
  
  // 时针（短粗）
  float hour_angle = (hour % 12) * 30 + minute * 0.5;
  float hour_rad = hour_angle * 3.14159 / 180.0;
  int hour_x = 64 + (int)(12 * sin(hour_rad));
  int hour_y = 65 - (int)(12 * cos(hour_rad));
  sprite.drawLine(64, 65, hour_x, hour_y, 0x07E0);      // 中心线
  sprite.drawLine(64-1, 65, hour_x-1, hour_y, 0x07E0);  // 左侧线（加粗）
  sprite.drawLine(64+1, 65, hour_x+1, hour_y, 0x07E0);  // 右侧线（加粗）
  
  // 分针（长细）
  float min_angle = minute * 6;
  float min_rad = min_angle * 3.14159 / 180.0;
  int min_x = 64 + (int)(20 * sin(min_rad));
  int min_y = 65 - (int)(20 * cos(min_rad));
  sprite.drawLine(64, 65, min_x, min_y, 0xFFFF);
  
  // 中心点
  sprite.fillCircle(64, 65, 2, 0xFFE0);
  
  // 时间显示
  char time_str[10];
  snprintf(time_str, sizeof(time_str), "%02d:%02d", hour, minute);
  
  sprite.setFont(BIG_FONT);
  sprite.setTextDatum(textdatum_t::middle_center);
  
  if (is_editing) {
    // 小时部分
    if (edit_field == 0) {
      sprite.setTextColor(0xFFE0); // 编辑时高亮
    } else {
      sprite.setTextColor(0xBDF7);
    }
    char hour_str[4];
    snprintf(hour_str, sizeof(hour_str), "%02d", hour);
    sprite.drawString(hour_str, 40, 110);
    
    // 冒号
    sprite.setTextColor(0xFFFF);
    sprite.drawString(":", 64, 110);
    
    // 分钟部分
    if (edit_field == 1) {
      sprite.setTextColor(0xFFE0);
    } else {
      sprite.setTextColor(0xBDF7);
    }
    char min_str[4];
    snprintf(min_str, sizeof(min_str), "%02d", minute);
    sprite.drawString(min_str, 88, 110);
  } else {
    sprite.setTextColor(0xFFFF);
    sprite.drawString(time_str, 64, 110);
  }
  
  // 操作提示
  sprite.setFont(&fonts::Font0);
  sprite.setTextColor(0xBDF7);
  sprite.setTextDatum(textdatum_t::bottom_left);
  sprite.drawString("NAV:+/-", 5, 155);
  sprite.setTextDatum(textdatum_t::bottom_right);
  sprite.drawString("CONF:Next", 123, 155);
  
  sprite.pushSprite(0, 0);
}

/**
 * @brief 显示喝水提醒界面
 * @param interval_minutes 提醒间隔(分钟)
 * @param enabled 是否开启
 * @param is_editing 是否处于编辑状态
 */
void lcd_display_show_reminder(int interval_minutes, bool enabled, bool is_editing) {
  s_current_screen = UI_SCREEN_REMINDER;
  
  sprite.fillScreen(0x0000);
  
  // 标题
  sprite.setTextColor(0x07FF);
  sprite.setFont(&fonts::Font0);
  sprite.setTextDatum(textdatum_t::top_center);
  sprite.drawString("- REMINDER MODE -", 64, 5);
  
  // 主标题
  sprite.setFont(&fonts::efontCN_16);
  sprite.setTextColor(0xFFFF);
  sprite.drawString("喝水提醒", 64, 30);
  
  // 水杯图标（简单矩形+波浪）
  sprite.drawRoundRect(52, 55, 24, 30, 4, enabled ? 0x07FF : 0x4208);
  if (enabled) {
    sprite.fillRect(54, 70, 20, 13, 0x07FF);
    // 波浪线
    for (int i = 0; i < 20; i += 4) {
      sprite.drawPixel(54 + i, 69, 0x0000);
      sprite.drawPixel(54 + i + 2, 70, 0x0000);
    }
  }
  
  // 状态显示
  sprite.setFont(BIG_FONT);
  sprite.setTextDatum(textdatum_t::middle_center);
  
  if (is_editing) {
    sprite.setTextColor(0xFFE0);
  } else {
    sprite.setTextColor(enabled ? 0x07E0 : 0xF800);
  }
  
  sprite.drawString(enabled ? "ON" : "OFF", 64, 105);
  
  // 间隔时间
  if (enabled) {
    sprite.setFont(&fonts::efontCN_16);
    sprite.setTextColor(0xBDF7);
    
    char interval_str[30];
    snprintf(interval_str, sizeof(interval_str), "每 %d 分钟提醒", interval_minutes);
    sprite.drawString(interval_str, 64, 130);
  } else {
    sprite.setFont(&fonts::efontCN_14);
    sprite.setTextColor(0x73AE);
    sprite.drawString("提醒已关闭", 64, 130);
  }
  
  // 操作提示
  sprite.setFont(&fonts::Font0);
  sprite.setTextColor(0xBDF7);
  sprite.setTextDatum(textdatum_t::bottom_left);
  sprite.drawString("NAV:Toggle", 5, 155);
  sprite.setTextDatum(textdatum_t::bottom_right);
  sprite.drawString("CONF:Save", 123, 155);
  
  sprite.pushSprite(0, 0);
}

} // extern "C"
