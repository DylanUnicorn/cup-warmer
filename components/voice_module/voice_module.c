/**
 * @file voice_module.c
 * @brief CH7800 语音模块驱动实现 - 4路 IO 触发模式
 *
 * CH7800 IO 引脚低电平触发播放音频：
 * IO1 -> 001.mp3, IO2 -> 002.mp3, IO3 -> 003.mp3, IO4 -> 004.mp3
 * 空闲时 GPIO 保持高电平，触发时拉低约 100ms。
 */

#include "voice_module.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "Voice";

#define VOICE_IO1_GPIO 1
#define VOICE_IO2_GPIO 18
#define VOICE_IO3_GPIO 19

#define VOICE_IO_COUNT 3

static const int voice_gpios[VOICE_IO_COUNT] = {
    VOICE_IO1_GPIO, VOICE_IO2_GPIO, VOICE_IO3_GPIO
};

// 触发脉冲宽度 (ms)
#define TRIGGER_PULSE_MS 100

// 触发后最小间隔 (ms)，防止重复触发
#define MIN_TRIGGER_INTERVAL_MS 500

static int64_t s_last_trigger_time = 0;

esp_err_t voice_module_init(void) {
    uint64_t pin_mask = 0;
    for (int i = 0; i < VOICE_IO_COUNT; i++) {
        pin_mask |= (1ULL << voice_gpios[i]);
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config failed for voice module");
        return ret;
    }

    // 空闲状态：全部高电平
    for (int i = 0; i < VOICE_IO_COUNT; i++) {
        gpio_set_level(voice_gpios[i], 1);
    }

    ESP_LOGI(TAG, "CH7800 voice module initialized (IO1=GPIO%d IO2=GPIO%d IO3=GPIO%d)",
             VOICE_IO1_GPIO, VOICE_IO2_GPIO, VOICE_IO3_GPIO);
    return ESP_OK;
}

void voice_play(int track_num) {
    if (track_num < 1 || track_num > VOICE_IO_COUNT) {
        ESP_LOGW(TAG, "Invalid track number: %d (valid: 1-%d)", track_num, VOICE_IO_COUNT);
        return;
    }

    // 防抖：避免短时间内重复触发
    int64_t now = esp_timer_get_time();
    if ((now - s_last_trigger_time) < (MIN_TRIGGER_INTERVAL_MS * 1000)) {
        ESP_LOGD(TAG, "Voice trigger too fast, ignored");
        return;
    }
    s_last_trigger_time = now;

    int gpio = voice_gpios[track_num - 1];
    ESP_LOGI(TAG, "Playing track %d (GPIO%d LOW pulse %dms)", track_num, gpio, TRIGGER_PULSE_MS);

    // 拉低触发
    gpio_set_level(gpio, 0);
    vTaskDelay(pdMS_TO_TICKS(TRIGGER_PULSE_MS));
    // 恢复高电平
    gpio_set_level(gpio, 1);
}
