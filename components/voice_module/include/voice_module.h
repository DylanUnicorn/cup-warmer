/**
 * @file voice_module.h
 * @brief CH7800 语音模块驱动 - IO 触发模式
 *
 * CH7800 模块通过 IO 引脚低电平触发播放对应音频：
 * IO1 → 001.mp3, IO2 → 002.mp3, IO3 → 003.mp3, IO4 → 004.mp3
 */

#ifndef VOICE_MODULE_H
#define VOICE_MODULE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化语音模块 (IO 触发模式)
 *
 * 配置 GPIO1 为输出，默认高电平（空闲）
 *
 * @return esp_err_t ESP_OK 成功
 */
esp_err_t voice_module_init(void);

/**
 * @brief 触发播放第 N 个音频
 *
 * 拉低对应 IO 引脚约 100ms 触发 CH7800 播放
 * 当前仅支持 IO1 (播放 001.mp3)
 *
 * @param track_num 音频编号 (1-4)
 */
void voice_play(int track_num);

#ifdef __cplusplus
}
#endif

#endif // VOICE_MODULE_H
