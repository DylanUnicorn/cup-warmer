/**
 * @file http_server.c
 * @brief HTTP REST API 服务器实现
 *
 * 提供以下接口：
 * - GET  /status     - 获取设备状态
 * - POST /control    - 发送控制指令
 * - POST /sync_time  - 同步时间
 */

#include "http_server.h"
#include "scheduler.h"
#include "soft_rtc.h"
#include "temp_control.h"


#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include <string.h>


static const char *TAG = "HttpServer";

static httpd_handle_t s_server = NULL;

#define SCRATCH_BUFSIZE 1024
static char s_scratch[SCRATCH_BUFSIZE];

/**
 * @brief 读取POST请求体
 */
static int read_post_body(httpd_req_t *req, char *buf, int buf_size) {
  int total_len = req->content_len;
  int cur_len = 0;
  int received = 0;

  if (total_len >= buf_size) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "Content too long");
    return -1;
  }

  while (cur_len < total_len) {
    received = httpd_req_recv(req, buf + cur_len, total_len - cur_len);
    if (received <= 0) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                          "Failed to receive data");
      return -1;
    }
    cur_len += received;
  }
  buf[total_len] = '\0';
  return total_len;
}

/**
 * @brief GET /status 处理函数
 *
 * 返回JSON格式的设备状态：
 * {
 *   "current_temp": 45.5,
 *   "target_temp": 55,
 *   "is_heating": 1,
 *   "esp_time": "08:00",
 *   "weekday": 5,
 *   "timer_remaining": 59,
 *   "schedule_time": "08:30"
 * }
 */
static esp_err_t status_get_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "application/json");

  // 获取当前状态
  float current_temp = temp_control_get_current_temp();
  int target_temp = temp_control_get_target_temp();
  bool is_heating = temp_control_is_heating();

  rtc_time_t rtc_time;
  soft_rtc_get_time(&rtc_time);

  int timer_remaining = scheduler_get_timer_remaining();
  char schedule_time[8];
  scheduler_get_schedule_time(schedule_time, sizeof(schedule_time));

  // 构建JSON响应
  cJSON *root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "current_temp", current_temp);
  cJSON_AddNumberToObject(root, "target_temp", target_temp);
  cJSON_AddNumberToObject(root, "is_heating", is_heating ? 1 : 0);

  char time_str[6];
  snprintf(time_str, sizeof(time_str), "%02d:%02d", rtc_time.hour,
           rtc_time.minute);
  cJSON_AddStringToObject(root, "esp_time", time_str);
  cJSON_AddNumberToObject(root, "weekday", rtc_time.weekday);

  cJSON_AddNumberToObject(root, "timer_remaining", timer_remaining);
  cJSON_AddStringToObject(root, "schedule_time", schedule_time);

  const char *json_str = cJSON_Print(root);
  httpd_resp_sendstr(req, json_str);

  free((void *)json_str);
  cJSON_Delete(root);

  ESP_LOGI(TAG, "GET /status - responded");
  return ESP_OK;
}

/**
 * @brief POST /control 处理函数
 *
 * 接收JSON格式的控制指令：
 * {
 *   "power": 1,
 *   "set_temp": 60,
 *   "timer_duration": 60,
 *   "schedule_time": "08:30"
 * }
 */
static esp_err_t control_post_handler(httpd_req_t *req) {
  if (read_post_body(req, s_scratch, SCRATCH_BUFSIZE) < 0) {
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "POST /control: %s", s_scratch);

  cJSON *root = cJSON_Parse(s_scratch);
  if (root == NULL) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  // 解析 power
  cJSON *power_item = cJSON_GetObjectItem(root, "power");
  if (power_item && cJSON_IsNumber(power_item)) {
    bool power_on = (power_item->valueint != 0);
    temp_control_set_power(power_on);
    ESP_LOGI(TAG, "Power: %s", power_on ? "ON" : "OFF");
  }

  // 解析 set_temp
  cJSON *temp_item = cJSON_GetObjectItem(root, "set_temp");
  if (temp_item && cJSON_IsNumber(temp_item)) {
    int set_temp = temp_item->valueint;
    temp_control_set_target_temp(set_temp);
    ESP_LOGI(TAG, "Target temp: %d", set_temp);
  }

  // 解析 timer_duration
  cJSON *timer_item = cJSON_GetObjectItem(root, "timer_duration");
  if (timer_item && cJSON_IsNumber(timer_item)) {
    int duration = timer_item->valueint;
    scheduler_set_timer_duration(duration);
    ESP_LOGI(TAG, "Timer duration: %d minutes", duration);
  }

  // 解析 schedule_time
  cJSON *schedule_item = cJSON_GetObjectItem(root, "schedule_time");
  if (schedule_item && cJSON_IsString(schedule_item)) {
    scheduler_set_schedule_time(schedule_item->valuestring);
    ESP_LOGI(TAG, "Schedule time: %s", schedule_item->valuestring);
  }

  cJSON_Delete(root);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"result\":\"ok\"}");

  return ESP_OK;
}

/**
 * @brief POST /sync_time 处理函数
 *
 * 接收JSON格式的时间同步请求：
 * {
 *   "time": "2025-12-26 08:00:00",
 *   "weekday": 5
 * }
 */
static esp_err_t sync_time_post_handler(httpd_req_t *req) {
  if (read_post_body(req, s_scratch, SCRATCH_BUFSIZE) < 0) {
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "POST /sync_time: %s", s_scratch);

  cJSON *root = cJSON_Parse(s_scratch);
  if (root == NULL) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  // 解析 time 字符串
  cJSON *time_item = cJSON_GetObjectItem(root, "time");
  cJSON *weekday_item = cJSON_GetObjectItem(root, "weekday");

  if (!time_item || !cJSON_IsString(time_item)) {
    cJSON_Delete(root);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'time' field");
    return ESP_FAIL;
  }

  int weekday = 1; // 默认周一
  if (weekday_item && cJSON_IsNumber(weekday_item)) {
    weekday = weekday_item->valueint;
    if (weekday < 1 || weekday > 7)
      weekday = 1;
  }

  // 解析时间字符串 "YYYY-MM-DD HH:MM:SS"
  const char *time_str = time_item->valuestring;
  int year, month, day, hour, minute, second;

  if (sscanf(time_str, "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute,
             &second) == 6) {

    rtc_time_t rtc_time = {.year = year,
                           .month = month,
                           .day = day,
                           .hour = hour,
                           .minute = minute,
                           .second = second,
                           .weekday = weekday};

    soft_rtc_set_time(&rtc_time);
    ESP_LOGI(TAG, "Time synced: %s, weekday=%d", time_str, weekday);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"result\":\"ok\"}");
  } else {
    ESP_LOGE(TAG, "Failed to parse time: %s", time_str);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid time format");
    cJSON_Delete(root);
    return ESP_FAIL;
  }

  cJSON_Delete(root);
  return ESP_OK;
}

esp_err_t http_server_start(void) {
  if (s_server != NULL) {
    ESP_LOGW(TAG, "Server already running");
    return ESP_OK;
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.uri_match_fn = httpd_uri_match_wildcard;
  config.lru_purge_enable = true;

  ESP_LOGI(TAG, "Starting HTTP server on port %d", config.server_port);

  esp_err_t err = httpd_start(&s_server, &config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start server: %s", esp_err_to_name(err));
    return err;
  }

  // 注册 URI 处理器
  // GET /status
  httpd_uri_t status_uri = {.uri = "/status",
                            .method = HTTP_GET,
                            .handler = status_get_handler,
                            .user_ctx = NULL};
  httpd_register_uri_handler(s_server, &status_uri);

  // POST /control
  httpd_uri_t control_uri = {.uri = "/control",
                             .method = HTTP_POST,
                             .handler = control_post_handler,
                             .user_ctx = NULL};
  httpd_register_uri_handler(s_server, &control_uri);

  // POST /sync_time
  httpd_uri_t sync_time_uri = {.uri = "/sync_time",
                               .method = HTTP_POST,
                               .handler = sync_time_post_handler,
                               .user_ctx = NULL};
  httpd_register_uri_handler(s_server, &sync_time_uri);

  ESP_LOGI(TAG, "HTTP server started successfully");
  return ESP_OK;
}

esp_err_t http_server_stop(void) {
  if (s_server == NULL) {
    return ESP_OK;
  }

  esp_err_t err = httpd_stop(s_server);
  if (err == ESP_OK) {
    s_server = NULL;
    ESP_LOGI(TAG, "HTTP server stopped");
  }
  return err;
}
