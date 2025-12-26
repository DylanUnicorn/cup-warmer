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
};
static const int menuCount = sizeof(menuItems) / sizeof(menuItems[0]);

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
  // 核心温度显示
  // ==========================================
  char temp_str[10];
  snprintf(temp_str, sizeof(temp_str), "%.1f", current_temp);

  sprite.setFont(BIG_FONT);
  sprite.setTextDatum(textdatum_t::middle_center);

  // 发光阴影效果
  sprite.setTextColor(0x212F);
  sprite.drawString(temp_str, 64 + 2, 50 + 2);

  // 主文字
  sprite.setTextColor(0xFFFF);
  sprite.drawString(temp_str, 64, 50);

  // 单位 °C
  sprite.setFont(&fonts::Font2);
  sprite.drawString("C", 115, 40);
  sprite.drawCircle(108, 35, 2, 0xFFFF);

  // ==========================================
  // 温度进度条
  // ==========================================
  sprite.setFont(&fonts::Font0);
  sprite.setTextDatum(textdatum_t::top_left);
  sprite.setTextColor(0xBDF7);
  sprite.drawString("Temp:", 10, 85);
  sprite.setCursor(100, 85);
  sprite.print(target_temp);

  // 进度条 (青色)
  int progress = (int)((current_temp / target_temp) * 100);
  if (progress > 100)
    progress = 100;
  drawProgressBar(10, 95, 108, 8, progress, 100, 0x07FF);

  // ==========================================
  // 加热状态
  // ==========================================
  sprite.setFont(&fonts::efontCN_14);
  sprite.setTextColor(0xFFFF);
  sprite.setCursor(5, 135);

  if (is_heating) {
    sprite.print("加热中...");
  } else if (current_temp >= target_temp - 2) {
    sprite.print("保温中");
  } else {
    sprite.print("待机");
  }

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

} // extern "C"
