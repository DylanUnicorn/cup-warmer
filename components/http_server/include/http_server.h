/**
 * @file http_server.h
 * @brief HTTP REST API 服务器
 */

#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动HTTP服务器
 *
 * 在端口80上启动REST API服务器，提供以下接口：
 * - GET  /status     - 获取设备状态
 * - POST /control    - 发送控制指令
 * - POST /sync_time  - 同步时间
 *
 * @return esp_err_t ESP_OK 成功
 */
esp_err_t http_server_start(void);

/**
 * @brief 停止HTTP服务器
 *
 * @return esp_err_t ESP_OK 成功
 */
esp_err_t http_server_stop(void);

#ifdef __cplusplus
}
#endif

#endif // HTTP_SERVER_H
