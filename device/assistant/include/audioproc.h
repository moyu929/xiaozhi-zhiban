/**
 * @file audioproc.h
 * @brief 设备端音频前处理（本地 AEC 兜底层）
 *
 * 背景：《08-重构方案》Q2/Q3 —— Realtime 上行原本直接抽取 data[i*3+1] 裸信号
 * 交给云端做 AEC（受 RTT 制约），AutoStop 模式外放时 duilite 引擎内 AEC 生效性
 * 未证实（A1-01 待实机项），导致播放期间难唤醒。
 *
 * 本模块提供一条不依赖第三方库的定点 NLMS 回声消除管线（Q15 定点，
 * Cortex-A5 soft-float ABI 上零浮点开销）作为软件兜底：
 *   in : mic 单声道 + 参考通道(ch2 = audio_service 硬件回采, V2 定论)
 *   out: 消回声后的干净 mono —— 供 Opus 上行，并可镜像喂给唤醒引擎（可选）。
 *
 * speexdsp/WebRTC AECM 替换为本文件唯一接入点（接口保持稳定），届时仅需
 * 替换 audioproc_process_sample 的实现。
 */

#ifndef AUDIOPROC_H
#define AUDIOPROC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化/复位 AEC 状态
 * @param sample_rate 目前固定 16000（保留参数以备将来变率）
 */
void audioproc_init(int sample_rate);

/**
 * @brief 清空自适应状态（在播放轨道重建/切换音源时调用，防旧滤波器发散）
 */
void audioproc_reset(void);

/**
 * @brief 逐样本回声消除
 * @param mic 干净混合麦输入（建议 (mic0>>1)+(mic1>>1)，避免削波失真）
 * @param ref 参考通道样本（采集流 ch2，V2 定论 micIdx=[0,1], refIdx=[2]）
 * @return 消回声后的输出样本
 */
int16_t audioproc_process_sample(int16_t mic, int16_t ref);

#ifdef __cplusplus
}
#endif

#endif /* AUDIOPROC_H */
