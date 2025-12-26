/**
 * @file pid.c
 * @brief PID控制算法实现
 */

#include "pid.h"

void pid_init(pid_controller_t *pid, float kp, float ki, float kd) {
  pid->kp = kp;
  pid->ki = ki;
  pid->kd = kd;

  pid->setpoint = 0.0f;
  pid->integral = 0.0f;
  pid->prev_error = 0.0f;

  pid->output_min = 0.0f;
  pid->output_max = 100.0f;
  pid->integral_max = 50.0f; // 默认积分限幅
}

void pid_set_setpoint(pid_controller_t *pid, float setpoint) {
  pid->setpoint = setpoint;
}

void pid_set_output_limits(pid_controller_t *pid, float min, float max) {
  pid->output_min = min;
  pid->output_max = max;
}

float pid_compute(pid_controller_t *pid, float current) {
  // 计算误差
  float error = pid->setpoint - current;

  // 比例项
  float p_term = pid->kp * error;

  // 积分项 (带限幅防止积分饱和)
  pid->integral += error;
  if (pid->integral > pid->integral_max) {
    pid->integral = pid->integral_max;
  } else if (pid->integral < -pid->integral_max) {
    pid->integral = -pid->integral_max;
  }
  float i_term = pid->ki * pid->integral;

  // 微分项
  float d_term = pid->kd * (error - pid->prev_error);
  pid->prev_error = error;

  // 计算输出
  float output = p_term + i_term + d_term;

  // 输出限幅
  if (output > pid->output_max) {
    output = pid->output_max;
  } else if (output < pid->output_min) {
    output = pid->output_min;
  }

  return output;
}

void pid_reset(pid_controller_t *pid) {
  pid->integral = 0.0f;
  pid->prev_error = 0.0f;
}
