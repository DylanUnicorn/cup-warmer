/**
 * @file pid.h
 * @brief PID控制算法
 */

#ifndef PID_H
#define PID_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief PID控制器结构体
 */
typedef struct {
  float kp; // 比例系数
  float ki; // 积分系数
  float kd; // 微分系数

  float setpoint; // 目标值

  float integral;   // 积分累计
  float prev_error; // 上次误差

  float output_min; // 输出下限
  float output_max; // 输出上限

  float integral_max; // 积分限幅
} pid_controller_t;

/**
 * @brief 初始化PID控制器
 *
 * @param pid PID控制器指针
 * @param kp 比例系数
 * @param ki 积分系数
 * @param kd 微分系数
 */
void pid_init(pid_controller_t *pid, float kp, float ki, float kd);

/**
 * @brief 设置PID目标值
 *
 * @param pid PID控制器指针
 * @param setpoint 目标值
 */
void pid_set_setpoint(pid_controller_t *pid, float setpoint);

/**
 * @brief 设置输出范围
 *
 * @param pid PID控制器指针
 * @param min 最小输出
 * @param max 最大输出
 */
void pid_set_output_limits(pid_controller_t *pid, float min, float max);

/**
 * @brief 计算PID输出
 *
 * @param pid PID控制器指针
 * @param current 当前值
 * @return float PID输出
 */
float pid_compute(pid_controller_t *pid, float current);

/**
 * @brief 重置PID控制器状态
 *
 * @param pid PID控制器指针
 */
void pid_reset(pid_controller_t *pid);

#ifdef __cplusplus
}
#endif

#endif // PID_H
