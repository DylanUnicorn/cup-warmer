/**
 * @file wifi_manager.c
 * @brief WiFi管理模块实现 - SmartConfig配网 + mDNS + 自动重连
 */

#include "wifi_manager.h"
#include "sdkconfig.h"

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_smartconfig.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/apps/netbiosns.h"
#include "mdns.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "WiFiManager";

// NVS 存储的 key
#define NVS_NAMESPACE "wifi_creds"
#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASSWORD "password"

// mDNS 配置
#ifndef CONFIG_CUP_WARMER_MDNS_HOSTNAME
#define CONFIG_CUP_WARMER_MDNS_HOSTNAME "heated-cup"
#endif

// 事件组标志位
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define ESPTOUCH_DONE_BIT BIT2

// 静态变量
static EventGroupHandle_t s_wifi_event_group = NULL;
static wifi_event_callback_t s_user_callback = NULL;
static bool s_is_connected = false;
static esp_netif_t *s_sta_netif = NULL;
static bool s_smartconfig_running = false;

// SmartConfig 任务
static void smartconfig_task(void *parm);

/**
 * @brief 保存WiFi凭证到NVS
 */
static esp_err_t save_wifi_credentials(const char *ssid, const char *password) {
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
    return err;
  }

  err = nvs_set_str(nvs_handle, NVS_KEY_SSID, ssid);
  if (err == ESP_OK) {
    err = nvs_set_str(nvs_handle, NVS_KEY_PASSWORD, password);
  }
  if (err == ESP_OK) {
    err = nvs_commit(nvs_handle);
  }

  nvs_close(nvs_handle);

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "WiFi credentials saved to NVS");
  }
  return err;
}

/**
 * @brief 从NVS读取WiFi凭证
 */
static esp_err_t load_wifi_credentials(char *ssid, size_t ssid_len,
                                       char *password, size_t pass_len) {
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
  if (err != ESP_OK) {
    return err;
  }

  err = nvs_get_str(nvs_handle, NVS_KEY_SSID, ssid, &ssid_len);
  if (err == ESP_OK) {
    err = nvs_get_str(nvs_handle, NVS_KEY_PASSWORD, password, &pass_len);
  }

  nvs_close(nvs_handle);
  return err;
}

/**
 * @brief WiFi事件处理函数
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT) {
    switch (event_id) {
    case WIFI_EVENT_STA_START:
      ESP_LOGI(TAG, "WiFi STA started");
      // 如果没有保存的凭证，启动SmartConfig
      break;

    case WIFI_EVENT_STA_DISCONNECTED:
      ESP_LOGI(TAG, "WiFi disconnected, reconnecting...");
      s_is_connected = false;
      xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

      if (s_user_callback) {
        s_user_callback(false);
      }

      // 尝试重连
      if (!s_smartconfig_running) {
        esp_wifi_connect();
      }
      break;

    default:
      break;
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));

    s_is_connected = true;
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

    if (s_user_callback) {
      s_user_callback(true);
    }
  } else if (event_base == SC_EVENT) {
    switch (event_id) {
    case SC_EVENT_SCAN_DONE:
      ESP_LOGI(TAG, "SmartConfig: Scan done");
      break;

    case SC_EVENT_FOUND_CHANNEL:
      ESP_LOGI(TAG, "SmartConfig: Found channel");
      break;

    case SC_EVENT_GOT_SSID_PSWD:
      ESP_LOGI(TAG, "SmartConfig: Got SSID and password");

      smartconfig_event_got_ssid_pswd_t *evt =
          (smartconfig_event_got_ssid_pswd_t *)event_data;
      wifi_config_t wifi_config;
      uint8_t ssid[33] = {0};
      uint8_t password[65] = {0};

      bzero(&wifi_config, sizeof(wifi_config_t));
      memcpy(wifi_config.sta.ssid, evt->ssid, sizeof(wifi_config.sta.ssid));
      memcpy(wifi_config.sta.password, evt->password,
             sizeof(wifi_config.sta.password));

      memcpy(ssid, evt->ssid, sizeof(evt->ssid));
      memcpy(password, evt->password, sizeof(evt->password));
      ESP_LOGI(TAG, "SSID: %s", ssid);

      // 保存到 NVS
      save_wifi_credentials((char *)ssid, (char *)password);

      ESP_ERROR_CHECK(esp_wifi_disconnect());
      ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
      esp_wifi_connect();
      break;

    case SC_EVENT_SEND_ACK_DONE:
      ESP_LOGI(TAG, "SmartConfig: ACK sent, config done");
      xEventGroupSetBits(s_wifi_event_group, ESPTOUCH_DONE_BIT);
      break;

    default:
      break;
    }
  }
}

/**
 * @brief SmartConfig 任务
 */
static void smartconfig_task(void *parm) {
  s_smartconfig_running = true;

  ESP_LOGI(TAG, "Starting SmartConfig...");
  ESP_ERROR_CHECK(esp_smartconfig_set_type(SC_TYPE_ESPTOUCH));
  smartconfig_start_config_t cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_smartconfig_start(&cfg));

  while (1) {
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group, WIFI_CONNECTED_BIT | ESPTOUCH_DONE_BIT, false,
        false, pdMS_TO_TICKS(500));

    if (bits & ESPTOUCH_DONE_BIT) {
      ESP_LOGI(TAG, "SmartConfig completed");
      esp_smartconfig_stop();
      s_smartconfig_running = false;
      gpio_set_level(CONFIG_STATUS_LED_PIN, 0); // 关闭灯
      vTaskDelete(NULL);
    }

    if (bits & WIFI_CONNECTED_BIT) {
      // 已经连上WiFi，但还没有收到SmartConfig的ACK完成信号
      // 这里只需要打印一次，或者降低频率，给底层协议栈时间发送ACK
      ESP_LOGD(TAG, "WiFi connected, waiting for SmartConfig ACK...");
      // 我们不需要在这里死循环打印，只需要等待 ESPTOUCH_DONE_BIT
      // 稍微延时避免看门狗复位
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    // 既没有连上，也没有完成，说明还在配网中或等待超时
    if (!(bits & (WIFI_CONNECTED_BIT | ESPTOUCH_DONE_BIT))) {
      // 超时退出 (500ms)，进行灯反转
      static int led_state = 0;
      led_state = !led_state;
      gpio_set_level(CONFIG_STATUS_LED_PIN, led_state);
    }
  }
}

esp_err_t wifi_manager_init(wifi_event_callback_t callback) {
  s_user_callback = callback;

  // 创建事件组
  s_wifi_event_group = xEventGroupCreate();

  // 初始化 LED
  gpio_reset_pin(CONFIG_STATUS_LED_PIN);
  gpio_set_direction(CONFIG_STATUS_LED_PIN, GPIO_MODE_OUTPUT);
  gpio_set_level(CONFIG_STATUS_LED_PIN, 0);

  // 初始化网络接口
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  s_sta_netif = esp_netif_create_default_wifi_sta();

  // 初始化WiFi
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  // 注册事件处理
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                             &wifi_event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                             &wifi_event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(SC_EVENT, ESP_EVENT_ANY_ID,
                                             &wifi_event_handler, NULL));

  // 按照用户要求：每次启动都清除旧凭证并进入配网模式
  wifi_manager_clear_credentials();

  ESP_LOGI(TAG, "Resetting WiFi: Always starting SmartConfig on boot");
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_start());
  wifi_manager_start_smartconfig();

  return ESP_OK;
}

void wifi_manager_start_smartconfig(void) {
  if (!s_smartconfig_running) {
    xTaskCreate(smartconfig_task, "smartconfig_task", 4096, NULL, 3, NULL);
  }
}

bool wifi_manager_is_connected(void) { return s_is_connected; }

esp_err_t wifi_manager_start_mdns(void) {
  esp_err_t err = mdns_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
    return err;
  }

  // 设置主机名
  mdns_hostname_set(CONFIG_CUP_WARMER_MDNS_HOSTNAME);
  ESP_LOGI(TAG, "mDNS hostname: %s.local", CONFIG_CUP_WARMER_MDNS_HOSTNAME);

  // 设置实例名
  mdns_instance_name_set("Smart-Cup-Warmer");

  // 添加 HTTP 服务
  // 注意：某些 App 扫描指定的协议名，确保与 App 需求匹配
  mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
  mdns_service_instance_name_set("_http", "_tcp", "Smart-Cup-Warmer");

  // 添加 TXT 记录帮助辨识
  mdns_service_txt_item_set("_http", "_tcp", "type", "cup-warmer");
  mdns_service_txt_item_set("_http", "_tcp", "status", "online");

  // NetBIOS 支持 (Windows 发现)
  netbiosns_init();
  netbiosns_set_name(CONFIG_CUP_WARMER_MDNS_HOSTNAME);

  return ESP_OK;
}

void wifi_manager_get_ip_string(char *ip_str) {
  if (!s_sta_netif || !ip_str) {
    if (ip_str)
      ip_str[0] = '\0';
    return;
  }

  esp_netif_ip_info_t ip_info;
  if (esp_netif_get_ip_info(s_sta_netif, &ip_info) == ESP_OK) {
    sprintf(ip_str, IPSTR, IP2STR(&ip_info.ip));
  } else {
    ip_str[0] = '\0';
  }
}

esp_err_t wifi_manager_clear_credentials(void) {
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK)
    return err;

  err = nvs_erase_all(nvs_handle);
  if (err == ESP_OK) {
    err = nvs_commit(nvs_handle);
  }
  nvs_close(nvs_handle);
  ESP_LOGI(TAG, "WiFi credentials cleared from NVS");
  return err;
}
