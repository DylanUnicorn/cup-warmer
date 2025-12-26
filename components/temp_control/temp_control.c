/**
 * @file temp_control.c
 * @brief 温控模块实现 - NTC温度读取 + PID控温 + 加热器PWM控制
 */

#include "temp_control.h"
#include "pid.h"
#include "sdkconfig.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
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

// NTC传感器ADC配置
// 【注意】请在 menuconfig 中配置 NTC_ADC_PIN，或直接修改此处
#ifndef CONFIG_NTC_ADC_PIN
#define CONFIG_NTC_ADC_PIN 0
#endif
#define NTC_ADC_CHANNEL ADC_CHANNEL_0 // GPIO0 对应 ADC1_CH0

// 加热器PWM配置
// 【注意】请在 menuconfig 中配置 HEATER_PWM_PIN，或直接修改此处
#ifndef CONFIG_HEATER_PWM_PIN
#define CONFIG_HEATER_PWM_PIN 4
#endif
#define HEATER_GPIO CONFIG_HEATER_PWM_PIN
#define HEATER_LEDC_TIMER LEDC_TIMER_0
#define HEATER_LEDC_CHANNEL LEDC_CHANNEL_0
#define HEATER_PWM_FREQ 1000              // 1kHz PWM频率
#define HEATER_PWM_BITS LEDC_TIMER_10_BIT // 10位分辨率 (0-1023)

// ============================================================================
// NTC热敏电阻参数 (根据实际NTC调整)
// ============================================================================
#define NTC_BETA 3950.0f      // B值
#define NTC_R25 10000.0f      // 25°C时的电阻值 (10k)
#define NTC_SERIES_R 10000.0f // 分压电阻值 (10k)
#define NTC_VREF_MV 3300.0f   // 参考电压 (mV)

// 温度限制
#ifndef CONFIG_TEMP_MIN
#define CONFIG_TEMP_MIN 30
#endif
#ifndef CONFIG_TEMP_MAX
#define CONFIG_TEMP_MAX 90
#endif
#ifndef CONFIG_TEMP_HARD_LIMIT
#define CONFIG_TEMP_HARD_LIMIT 95
#endif

// PID参数 (从Kconfig读取，除以100得到实际值)
#ifndef CONFIG_PID_KP
#define CONFIG_PID_KP 200
#endif
#ifndef CONFIG_PID_KI
#define CONFIG_PID_KI 10
#endif
#ifndef CONFIG_PID_KD
#define CONFIG_PID_KD 50
#endif

// ============================================================================
// 静态变量
// ============================================================================
static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t s_adc_cali_handle = NULL;
static bool s_cali_enabled = false;

static pid_controller_t s_pid;

static bool s_power_on = false;
static int s_target_temp = 55;       // 默认目标温度
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
// PWM 初始化
// ============================================================================
static esp_err_t pwm_init(void) {
  // 配置LEDC定时器
  ledc_timer_config_t timer_cfg = {
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .timer_num = HEATER_LEDC_TIMER,
      .duty_resolution = HEATER_PWM_BITS,
      .freq_hz = HEATER_PWM_FREQ,
      .clk_cfg = LEDC_AUTO_CLK,
  };
  ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

  // 配置LEDC通道
  ledc_channel_config_t channel_cfg = {
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .channel = HEATER_LEDC_CHANNEL,
      .timer_sel = HEATER_LEDC_TIMER,
      .intr_type = LEDC_INTR_DISABLE,
      .gpio_num = HEATER_GPIO,
      .duty = 0,
      .hpoint = 0,
  };
  ESP_ERROR_CHECK(ledc_channel_config(&channel_cfg));

  ESP_LOGI(TAG, "Heater PWM initialized on GPIO%d", HEATER_GPIO);
  return ESP_OK;
}

// ============================================================================
// 设置加热器PWM占空比
// ============================================================================
static void set_heater_duty(float duty_percent) {
  if (duty_percent < 0)
    duty_percent = 0;
  if (duty_percent > 100)
    duty_percent = 100;

  // 10位分辨率: 0-1023
  uint32_t duty = (uint32_t)(duty_percent * 10.23f);
  ledc_set_duty(LEDC_LOW_SPEED_MODE, HEATER_LEDC_CHANNEL, duty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, HEATER_LEDC_CHANNEL);
}

// ============================================================================
// 读取NTC温度
// ============================================================================
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

  // 转换为电压
  if (s_cali_enabled) {
    adc_cali_raw_to_voltage(s_adc_cali_handle, adc_raw, &voltage_mv);
  } else {
    // 简单线性估算
    voltage_mv = (adc_raw * 3300) / 4095;
  }

  // 检查电压范围 (传感器异常检测)
  if (voltage_mv < 100 || voltage_mv > 3200) {
    ESP_LOGW(TAG, "NTC voltage out of range: %d mV", voltage_mv);
    s_sensor_ok = false;
    return -999.0f;
  }
  s_sensor_ok = true;

  // 计算NTC电阻值 (分压公式: V_ntc = Vref * R_ntc / (R_series + R_ntc))
  // R_ntc = R_series * V_ntc / (Vref - V_ntc)
  float v_ntc = (float)voltage_mv;
  float r_ntc = NTC_SERIES_R * v_ntc / (NTC_VREF_MV - v_ntc);

  // 使用B值公式计算温度
  // 1/T = 1/T25 + (1/B) * ln(R/R25)
  // T = 1 / (1/T25 + (1/B) * ln(R/R25)) - 273.15
  float t25_kelvin = 25.0f + 273.15f;
  float temp_kelvin =
      1.0f / (1.0f / t25_kelvin + (1.0f / NTC_BETA) * logf(r_ntc / NTC_R25));
  float temp_celsius = temp_kelvin - 273.15f;

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
      set_heater_duty(0);
      xSemaphoreGive(s_mutex);
      vTaskDelayUntil(&last_wake_time, period);
      continue;
    }

    // NTC异常保护
    if (!s_sensor_ok) {
      ESP_LOGE(TAG, "SAFETY: NTC sensor error, stopping heater!");
      s_is_heating = false;
      s_state = TEMP_STATE_ERROR;
      set_heater_duty(0);
      xSemaphoreGive(s_mutex);
      vTaskDelayUntil(&last_wake_time, period);
      continue;
    }

    // 正常温控逻辑
    if (s_power_on) {
      // 设置PID目标
      pid_set_setpoint(&s_pid, (float)s_target_temp);

      // 计算PID输出 (0-100%)
      float output = pid_compute(&s_pid, s_current_temp);

      // 设置加热器PWM
      set_heater_duty(output);

      s_is_heating = (output > 5.0f); // 输出>5%认为在加热
      s_state = s_is_heating ? TEMP_STATE_HEATING : TEMP_STATE_KEEPING;

      ESP_LOGD(TAG, "Temp: %.1f -> %d, PID output: %.1f%%", s_current_temp,
               s_target_temp, output);
    } else {
      // 电源关闭
      set_heater_duty(0);
      s_is_heating = false;
      s_state = TEMP_STATE_IDLE;
      pid_reset(&s_pid);
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
    return err;
  }

  // 初始化PWM
  err = pwm_init();
  if (err != ESP_OK) {
    return err;
  }

  // 初始化PID控制器 (参数从Kconfig读取，除以100)
  float kp = (float)CONFIG_PID_KP / 100.0f;
  float ki = (float)CONFIG_PID_KI / 100.0f;
  float kd = (float)CONFIG_PID_KD / 100.0f;

  pid_init(&s_pid, kp, ki, kd);
  pid_set_output_limits(&s_pid, 0, 100); // 输出0-100%

  ESP_LOGI(TAG, "Temp control initialized. PID: Kp=%.2f, Ki=%.2f, Kd=%.2f", kp,
           ki, kd);
  ESP_LOGI(TAG, "NTC ADC on GPIO%d (TODO: verify pin)", CONFIG_NTC_ADC_PIN);
  ESP_LOGI(TAG, "Heater PWM on GPIO%d (TODO: verify pin)",
           CONFIG_HEATER_PWM_PIN);

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
    set_heater_duty(0);
    pid_reset(&s_pid);
  }
  xSemaphoreGive(s_mutex);
  ESP_LOGI(TAG, "Power %s", on ? "ON" : "OFF");
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
