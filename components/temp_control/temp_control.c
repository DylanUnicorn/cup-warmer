/**
 * @file temp_control.c
 * @brief 温控模块实现 - NTC温度读取 + 简单开关控制
 */

#include "temp_control.h"
#include "sdkconfig.h"

#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <math.h>


static const char *TAG = "TempControl";

// ============================================================================
// 【TODO: 请根据实际硬件修改以下引脚配置】
// ============================================================================

// NTC传感器ADC配置 - NTCGS163JF103FT8
#define NTC_ADC_GPIO 0            // GPIO0 用于NTC温度采集
#define NTC_ADC_CHANNEL ADC_CHANNEL_0 // GPIO0 对应 ADC1_CH0

// 加热器控制配置
#define HEATER_GPIO 12  // GPIO12 控制加热膜

// ============================================================================
// NTC热敏电阻参数 - NTCGS163JF103FT8
// ============================================================================
#define NTC_BETA 3435.0f      // B值 (规格书参数)
#define NTC_R0 10000.0f       // 25°C时的标称电阻值 (10kΩ)
#define NTC_T0_KELVIN 298.15f // 25°C = 298.15K
#define NTC_R1 10000.0f       // 上拉电阻值 (10kΩ)
#define NTC_VREF_MV 3300.0f   // 参考电压 (3.3V)
#define ADC_MAX_VALUE 4095.0f // 12bit ADC (0-4095)

// 温度限制
#ifndef CONFIG_TEMP_MIN
#define CONFIG_TEMP_MIN 30
#endif
#ifndef CONFIG_TEMP_MAX
#define CONFIG_TEMP_MAX 141
#endif
#ifndef CONFIG_TEMP_HARD_LIMIT
#define CONFIG_TEMP_HARD_LIMIT 150
#endif

// 恒温控制参数 - 滞环带宽 (±HYSTERESIS °C)
#define TEMP_HYSTERESIS 2.5f

// 面板温度到水温的估算偏移量
#define WATER_TEMP_OFFSET 30.0f

// ============================================================================
// 静态变量
// ============================================================================
static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t s_adc_cali_handle = NULL;
static bool s_cali_enabled = false;

static bool s_power_on = true;  // 默认开启温控
static int s_target_temp = 80;       // 默认目标面板温度
static float s_current_temp = 25.0f; // 当前温度
static bool s_is_heating = false;
static temp_state_t s_state = TEMP_STATE_IDLE;
static bool s_sensor_ok = true;

static SemaphoreHandle_t s_mutex = NULL;

// ============================================================================
// ADC 初始化
// ============================================================================
static esp_err_t adc_init(void) {
  // 创建ADC单元
  adc_oneshot_unit_init_cfg_t init_cfg = {
      .unit_id = ADC_UNIT_1,
  };
  ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &s_adc_handle));

  // 配置ADC通道
  adc_oneshot_chan_cfg_t chan_cfg = {
      .bitwidth = ADC_BITWIDTH_12,
      .atten = ADC_ATTEN_DB_12, // 0-3.3V范围
  };
  ESP_ERROR_CHECK(
      adc_oneshot_config_channel(s_adc_handle, NTC_ADC_CHANNEL, &chan_cfg));

  // 尝试ADC校准
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
  adc_cali_curve_fitting_config_t cali_cfg = {
      .unit_id = ADC_UNIT_1,
      .atten = ADC_ATTEN_DB_12,
      .bitwidth = ADC_BITWIDTH_12,
  };
  if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_adc_cali_handle) ==
      ESP_OK) {
    s_cali_enabled = true;
    ESP_LOGI(TAG, "ADC calibration enabled (curve fitting)");
  }
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
  adc_cali_line_fitting_config_t cali_cfg = {
      .unit_id = ADC_UNIT_1,
      .atten = ADC_ATTEN_DB_12,
      .bitwidth = ADC_BITWIDTH_12,
  };
  if (adc_cali_create_scheme_line_fitting(&cali_cfg, &s_adc_cali_handle) ==
      ESP_OK) {
    s_cali_enabled = true;
    ESP_LOGI(TAG, "ADC calibration enabled (line fitting)");
  }
#endif

  return ESP_OK;
}

// ============================================================================
// 加热器GPIO初始化（直接开关控制，非PWM）
// ============================================================================
static esp_err_t heater_gpio_init(void) {
  gpio_config_t io_conf = {
    .pin_bit_mask = (1ULL << HEATER_GPIO),
    .mode = GPIO_MODE_OUTPUT,
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE
  };
  
  esp_err_t ret = gpio_config(&io_conf);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Heater GPIO config failed");
    return ret;
  }
  
  gpio_set_level(HEATER_GPIO, 0); // 初始状态关闭
  ESP_LOGI(TAG, "Heater GPIO initialized on GPIO%d (initial: OFF)", HEATER_GPIO);
  return ESP_OK;
}

// ============================================================================
// 设置加热器状态（开/关）
// ============================================================================
static void set_heater_state(bool on) {
  gpio_set_level(HEATER_GPIO, on ? 1 : 0);
  ESP_LOGD(TAG, "Heater: %s", on ? "ON" : "OFF");
}

// ============================================================================
// 读取NTC温度 - 使用Steinhart-Hart公式（B值法）
// 返回温度值，并通过全局变量输出电阻值
// ============================================================================
static float s_last_r_ntc = 0.0f; // 全局变量存储最后一次的电阻值

static float read_ntc_temperature(void) {
  int adc_raw = 0;
  int voltage_mv = 0;

  // 读取ADC原始值
  esp_err_t err = adc_oneshot_read(s_adc_handle, NTC_ADC_CHANNEL, &adc_raw);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "ADC read error: %s", esp_err_to_name(err));
    s_sensor_ok = false;
    return -999.0f;
  }

  // 转换为电压（mV）
  if (s_cali_enabled) {
    adc_cali_raw_to_voltage(s_adc_cali_handle, adc_raw, &voltage_mv);
  } else {
    // 简单线性估算: V_out = (ADC_Value / 4095) * 3300
    voltage_mv = (int)((float)adc_raw * NTC_VREF_MV / ADC_MAX_VALUE);
  }

  // 检查电压范围 (传感器异常检测)
  if (voltage_mv < 100 || voltage_mv > 3200) {
    ESP_LOGW(TAG, "NTC voltage out of range: %d mV (ADC=%d)", voltage_mv, adc_raw);
    s_sensor_ok = false;
    return -999.0f;
  }
  s_sensor_ok = true;

  // 步骤1: 从电压计算NTC电阻值
  // 分压公式: V_out = 3.3V * R_ntc / (R1 + R_ntc)
  // 反推: R_ntc = (V_out * R1) / (3.3V - V_out)
  float v_out = (float)voltage_mv;
  float r_ntc = (v_out * NTC_R1) / (NTC_VREF_MV - v_out);
  
  // 保存电阻值供外部访问
  s_last_r_ntc = r_ntc;

  // 步骤2: 使用Steinhart-Hart公式（B值法）计算温度
  // T = 1 / (1/T0 + (1/B) * ln(R_ntc/R0))
  // 其中: T0=298.15K, B=3435K, R0=10kΩ
  float temp_kelvin = 1.0f / (
    (1.0f / NTC_T0_KELVIN) + 
    (1.0f / NTC_BETA) * logf(r_ntc / NTC_R0)
  );
  float temp_celsius = temp_kelvin - 273.15f;

  // 详细日志（便于调试温度计算）
  ESP_LOGI(TAG, "========================================");
  ESP_LOGI(TAG, "[NTC Debug] ADC Raw Value: %d", adc_raw);
  ESP_LOGI(TAG, "[NTC Debug] Voltage (V_out): %.2f mV", v_out);
  ESP_LOGI(TAG, "[NTC Debug] Resistance (R_ntc): %.0f Ω (%.2f kΩ)", r_ntc, r_ntc/1000.0f);
  ESP_LOGI(TAG, "[NTC Debug] Temperature: %.2f °C", temp_celsius);
  ESP_LOGI(TAG, "========================================");

  return temp_celsius;
}

// ============================================================================
// 温控任务
// ============================================================================
static void temp_control_task(void *arg) {
  TickType_t last_wake_time = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(500); // 500ms控制周期

  while (1) {
    // 读取当前温度
    float temp = read_ntc_temperature();

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_sensor_ok) {
      s_current_temp = temp;
    }

    // 安全保护: 95°C强制断电
    if (s_current_temp >= CONFIG_TEMP_HARD_LIMIT) {
      ESP_LOGW(TAG, "SAFETY: Temperature %.1f >= %d, emergency shutoff!",
               s_current_temp, CONFIG_TEMP_HARD_LIMIT);
      s_power_on = false;
      s_is_heating = false;
      s_state = TEMP_STATE_IDLE;
      set_heater_state(false);
      xSemaphoreGive(s_mutex);
      vTaskDelayUntil(&last_wake_time, period);
      continue;
    }

    // NTC异常保护
    if (!s_sensor_ok) {
      ESP_LOGE(TAG, "SAFETY: NTC sensor error, stopping heater!");
      s_is_heating = false;
      s_state = TEMP_STATE_ERROR;
      set_heater_state(false);
      xSemaphoreGive(s_mutex);
      vTaskDelayUntil(&last_wake_time, period);
      continue;
    }

    // 温控逻辑：基于用户设定的面板目标温度，滞环控制
    if (s_power_on) {
      float target_low  = (float)s_target_temp - TEMP_HYSTERESIS;
      float target_high = (float)s_target_temp + TEMP_HYSTERESIS;

      if (s_current_temp < target_low) {
        // 温度低于目标下限，开启加热
        set_heater_state(true);
        s_is_heating = true;
        s_state = TEMP_STATE_HEATING;
        ESP_LOGI(TAG, "[Heating] Panel: %.1f°C < %.1f°C (target=%d) | Water: ~%.0f°C | R_ntc: %.0fΩ", 
                 s_current_temp, target_low, s_target_temp,
                 s_current_temp - WATER_TEMP_OFFSET, s_last_r_ntc);
      } else if (s_current_temp >= target_high) {
        // 温度达到目标上限，关闭加热
        set_heater_state(false);
        s_is_heating = false;
        s_state = TEMP_STATE_KEEPING;
        ESP_LOGI(TAG, "[Keeping] Panel: %.1f°C >= %.1f°C (target=%d) | Water: ~%.0f°C | R_ntc: %.0fΩ", 
                 s_current_temp, target_high, s_target_temp,
                 s_current_temp - WATER_TEMP_OFFSET, s_last_r_ntc);
      } else {
        // 在死区范围内，保持当前状态
        ESP_LOGD(TAG, "[Stable] Panel: %.1f°C (%.1f-%.1f) target=%d | Heater: %s | R_ntc: %.0fΩ", 
                 s_current_temp, target_low, target_high, s_target_temp,
                 s_is_heating ? "ON" : "OFF", s_last_r_ntc);
      }
    } else {
      // 电源关闭
      set_heater_state(false);
      s_is_heating = false;
      s_state = TEMP_STATE_IDLE;
    }

    xSemaphoreGive(s_mutex);

    vTaskDelayUntil(&last_wake_time, period);
  }
}

// ============================================================================
// 公开接口实现
// ============================================================================

esp_err_t temp_control_init(void) {
  s_mutex = xSemaphoreCreateMutex();
  if (s_mutex == NULL) {
    return ESP_FAIL;
  }

  // 初始化ADC
  esp_err_t err = adc_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "ADC init failed!");
    return err;
  }

  // 初始化加热器GPIO
  err = heater_gpio_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Heater GPIO init failed!");
    return err;
  }

  ESP_LOGI(TAG, "==========================================");
  ESP_LOGI(TAG, "  Temperature Control Initialized");
  ESP_LOGI(TAG, "  NTC Sensor: GPIO%d (ADC1_CH%d)", NTC_ADC_GPIO, NTC_ADC_CHANNEL);
  ESP_LOGI(TAG, "  Heater Control: GPIO%d (ENABLED)", HEATER_GPIO);
  ESP_LOGI(TAG, "  NTC Model: NTCGS163JF103FT8");
  ESP_LOGI(TAG, "  Parameters: R0=10kΩ, B=3435K, R1=10kΩ");
  ESP_LOGI(TAG, "  ⚙️ Target Panel Temp: %d°C (±%.1f°C hysteresis)", s_target_temp, TEMP_HYSTERESIS);
  ESP_LOGI(TAG, "  🔥 Auto-start heating enabled, max=%d°C", CONFIG_TEMP_MAX);
  ESP_LOGI(TAG, "==========================================");

  return ESP_OK;
}

void temp_control_start_task(void) {
  xTaskCreate(temp_control_task, "temp_ctrl", 4096, NULL, 6, NULL);
  ESP_LOGI(TAG, "Temp control task started");
}

void temp_control_set_power(bool on) {
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  s_power_on = on;
  if (!on) {
    set_heater_state(false);
  }
  xSemaphoreGive(s_mutex);
  ESP_LOGI(TAG, "Temperature Control Power: %s", on ? "ON" : "OFF");
}

bool temp_control_get_power(void) { return s_power_on; }

void temp_control_set_target_temp(int temp) {
  // 限制范围
  if (temp < CONFIG_TEMP_MIN)
    temp = CONFIG_TEMP_MIN;
  if (temp > CONFIG_TEMP_MAX)
    temp = CONFIG_TEMP_MAX;

  xSemaphoreTake(s_mutex, portMAX_DELAY);
  s_target_temp = temp;
  xSemaphoreGive(s_mutex);
  ESP_LOGI(TAG, "Target temp set to %d", temp);
}

int temp_control_get_target_temp(void) { return s_target_temp; }

float temp_control_get_current_temp(void) { return s_current_temp; }

bool temp_control_is_heating(void) { return s_is_heating; }

temp_state_t temp_control_get_state(void) { return s_state; }

bool temp_control_is_sensor_ok(void) { return s_sensor_ok; }

float temp_control_get_water_temp_estimate(void) {
  float panel_temp = s_current_temp;
  float water_temp = panel_temp - WATER_TEMP_OFFSET;
  if (water_temp < 0.0f) water_temp = 0.0f;
  if (water_temp > 100.0f) water_temp = 100.0f;
  return water_temp;
}
