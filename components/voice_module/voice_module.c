/**
 * @file voice_module.c
 * @brief CH7800 语音模块驱动实现 - IO 触发模式
 *
 * CH7800 IO 引脚低电平触发播放音频。
 * 空闲时 GPIO 保持高电平，触发时拉低约 100ms。
 */

#include "voice_module.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "Voice";

// CH7800 IO1 连接的 GPIO 引脚
#define VOICE_IO1_GPIO 1

// 触发脉冲宽度 (ms)
#define TRIGGER_PULSE_MS 100

// 触发后最小间隔 (ms)，防止重复触发
#define MIN_TRIGGER_INTERVAL_MS 500

static int64_t s_last_trigger_time = 0;

esp_err_t voice_module_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << VOICE_IO1_GPIO),
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

    // 空闲状态：高电平
    gpio_set_level(VOICE_IO1_GPIO, 1);

    ESP_LOGI(TAG, "CH7800 voice module initialized (IO1 = GPIO%d)", VOICE_IO1_GPIO);
    return ESP_OK;
}

void voice_play(int track_num) {
    // 当前仅支持 IO1 (track 1)
    if (track_num != 1) {
        ESP_LOGW(TAG, "Only track 1 supported in IO mode (requested: %d)", track_num);
        return;
    }

    // 防抖：避免短时间内重复触发
    int64_t now = esp_timer_get_time();
    if ((now - s_last_trigger_time) < (MIN_TRIGGER_INTERVAL_MS * 1000)) {
        ESP_LOGD(TAG, "Voice trigger too fast, ignored");
        return;
    }
    s_last_trigger_time = now;

    ESP_LOGI(TAG, "Playing track %d (IO1 LOW pulse %dms)", track_num, TRIGGER_PULSE_MS);

    // 拉低触发
    gpio_set_level(VOICE_IO1_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(TRIGGER_PULSE_MS));
    // 恢复高电平
    gpio_set_level(VOICE_IO1_GPIO, 1);
}
