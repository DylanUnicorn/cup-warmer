/**
 * @file button.c
 * @brief 按键管理模块实现
 */

#include "button.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BUTTON";

// 按键回调函数
static button_callback_t g_button_callback = NULL;

// 按键状态记录(用于消抖)
static bool g_button_last_state[3] = {false, false, false};

/**
 * @brief 按键扫描任务
 */
static void button_scan_task(void *arg) {
    const int gpio_pins[3] = {BUTTON_1_GPIO, BUTTON_2_GPIO, BUTTON_3_GPIO};
    const button_event_t events[3] = {
        BUTTON_EVENT_1_PRESSED,
        BUTTON_EVENT_2_PRESSED, 
        BUTTON_EVENT_3_PRESSED
    };
    
    while (1) {
        for (int i = 0; i < 3; i++) {
            bool current_state = gpio_get_level(gpio_pins[i]);
            
            // 检测上升沿(低到高，按键按下)
            if (current_state && !g_button_last_state[i]) {
                ESP_LOGI(TAG, "Button %d pressed (GPIO %d)", i + 1, gpio_pins[i]);
                
                // 触发回调
                if (g_button_callback) {
                    g_button_callback(events[i]);
                }
            }
            
            g_button_last_state[i] = current_state;
        }
        
        vTaskDelay(pdMS_TO_TICKS(50)); // 50ms扫描周期，防抖
    }
}

esp_err_t button_init(void) {
    ESP_LOGI(TAG, "Initializing button module");
    
    // 配置GPIO
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUTTON_1_GPIO) | 
                        (1ULL << BUTTON_2_GPIO) | 
                        (1ULL << BUTTON_3_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE, // 默认下拉，按下时为高电平
        .intr_type = GPIO_INTR_DISABLE
    };
    
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure GPIO");
        return ret;
    }
    
    // 创建按键扫描任务
    BaseType_t task_ret = xTaskCreate(
        button_scan_task,
        "button_scan",
        2048,
        NULL,
        5,
        NULL
    );
    
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create button scan task");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Button module initialized successfully");
    return ESP_OK;
}

esp_err_t button_register_callback(button_callback_t callback) {
    if (callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    g_button_callback = callback;
    ESP_LOGI(TAG, "Button callback registered");
    return ESP_OK;
}

bool button_is_pressed(int button_num) {
    if (button_num < 1 || button_num > 3) {
        return false;
    }
    
    const int gpio_pins[3] = {BUTTON_1_GPIO, BUTTON_2_GPIO, BUTTON_3_GPIO};
    return gpio_get_level(gpio_pins[button_num - 1]);
}
