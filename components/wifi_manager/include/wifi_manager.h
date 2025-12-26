/**
 * @file wifi_manager.h
 * @brief WiFi管理模块 - SmartConfig配网 + mDNS + 自动重连
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief WiFi 事件回调类型
 */
typedef void (*wifi_event_callback_t)(bool connected);

/**
 * @brief 初始化WiFi管理器
 *
 * 尝试从NVS读取已保存的WiFi凭证进行连接，
 * 如果没有保存的凭证或连接失败，则启动SmartConfig
 *
 * @param callback WiFi状态变化回调函数（可为NULL）
 * @return esp_err_t ESP_OK 成功
 */
esp_err_t wifi_manager_init(wifi_event_callback_t callback);

/**
 * @brief 强制启动SmartConfig配网
 *
 * 即使已有保存的凭证，也会重新进入配网模式
 */
void wifi_manager_start_smartconfig(void);

/**
 * @brief 获取WiFi连接状态
 *
 * @return true 已连接
 * @return false 未连接
 */
bool wifi_manager_is_connected(void);

/**
 * @brief 启动mDNS服务
 *
 * 注册设备为 heated-cup.local
 *
 * @return esp_err_t ESP_OK 成功
 */
esp_err_t wifi_manager_start_mdns(void);

/**
 * @brief 获取设备IP地址字符串
 *
 * @param ip_str 输出缓冲区（至少16字节）
 */
void wifi_manager_get_ip_string(char *ip_str);

/**
 * @brief 清除NVS中保存的WiFi凭证
 */
esp_err_t wifi_manager_clear_credentials(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_MANAGER_H
