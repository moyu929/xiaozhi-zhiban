/**
 * @file display_ctrl.h
 * @brief 会话域背光/息屏管理器（《08 方案》§八 需求③）
 *
 * 设计口径：
 *  - 与原生 libsystime 三倒计时**解耦**：原生息屏链我们从未接入且不想牵连空闲态；
 *    这里做一个 assistant 自有的、只在会话期间生效的背光管理器。
 *  - 配置 SCREEN_OFF_IDLE_SEC：0=不息屏（默认）；>0 = 无音频输出 N 秒后熄屏。
 *  - 熄屏用 bl_power（电源门），不用 brightness=0（部分面板低值仍微亮）；
 *    音频链路与 AEC 参考（audio_service 硬件 loopback）完全不受影响。
 *  - AI 内部状态切换（聆听↔播报）保持黑屏；触摸/按键恢复亮屏且**就地吞掉**
 *    该次输入（只点亮，不当成点击）；会话结束回 Idle 自动恢复。
 */

#ifndef DISPLAY_CTRL_H
#define DISPLAY_CTRL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 启动管理线程（幂等） */
void display_ctrl_start(void);

/** 重新加载配置键 SCREEN_OFF_IDLE_SEC（可运行时变更） */
void display_ctrl_reload_config(void);

/** 记录“有音频输出/用户活动”事件：刷新会话内 idle 计时（频繁调用廉价） */
void display_ctrl_note_activity(void);

/** 会话生命周期：进入对话会话=1，回到 Idle=0（Idle 时若黑屏则恢复亮屏） */
void display_ctrl_set_session(int active);

/** 输入事件入口挂钩（触摸/按键）。返回 1=本次输入已被息屏恢复消费（调用方应忽略后续处理） */
int display_ctrl_notify_input(void);

/** MCP 主动息屏: 立即灭屏, 交流不受影响; 会话结束不自动亮回,
 * 触摸输入唤醒(display_ctrl_notify_input). 返回0成功 */
int display_ctrl_manual_off(void);

/** 当前是否息屏态 */
int display_ctrl_is_off(void);

/** MCP 主动亮屏: 息屏态恢复并清手动标志; 已亮返回1 */
int display_ctrl_manual_on(void);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_CTRL_H */
