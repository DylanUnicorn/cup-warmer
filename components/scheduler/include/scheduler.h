/**
 * @file scheduler.h
 * @brief 定时与预约模块 - 倒计时 + 预约加热逻辑
 */

#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "esp_err.h"
#include <stdbool.h>


#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 调度器状态枚举
 */
typedef enum {
  SCHED_STATE_IDLE,          // 空闲
  SCHED_STATE_TIMER_RUNNING, // 倒计时运行中
  SCHED_STATE_SCHEDULED,     // 预约等待中
  SCHED_STATE_TIMEOUT        // 已超时
} scheduler_state_t;

/**
 * @brief 初始化调度器
 *
 * @return esp_err_t ESP_OK 成功
 */
esp_err_t scheduler_init(void);

/**
 * @brief 启动调度器任务
 */
void scheduler_start_task(void);

/**
 * @brief 设置定时加热时长
 *
 * 加热开启后开始倒计时，到时自动关闭加热
 *
 * @param minutes 加热时长 (分钟)
 */
void scheduler_set_timer_duration(int minutes);

/**
 * @brief 获取定时器剩余时间
 *
 * @return int 剩余分钟数
 */
int scheduler_get_timer_remaining(void);

/**
 * @brief 设置预约加热时间
 *
 * 格式 "HH:MM"，到达指定时间时自动开始加热
 * 会提前启动以确保到时温度达标
 *
 * @param time_str 预约时间 "HH:MM"
 */
void scheduler_set_schedule_time(const char *time_str);

/**
 * @brief 获取预约时间
 *
 * @param buf 输出缓冲区
 * @param buf_size 缓冲区大小
 */
void scheduler_get_schedule_time(char *buf, int buf_size);

/**
 * @brief 取消预约
 */
void scheduler_cancel_schedule(void);

/**
 * @brief 启动倒计时
 *
 * 手动启动倒计时（加热开启时自动调用）
 */
void scheduler_start_timer(void);

/**
 * @brief 停止倒计时
 */
void scheduler_stop_timer(void);

/**
 * @brief 获取调度器状态
 *
 * @return scheduler_state_t 当前状态
 */
scheduler_state_t scheduler_get_state(void);

/**
 * @brief 设置超时回调
 *
 * @param callback 超时时调用的函数
 */
void scheduler_set_timeout_callback(void (*callback)(void));

#ifdef __cplusplus
}
#endif

#endif // SCHEDULER_H
