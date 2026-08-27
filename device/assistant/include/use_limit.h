/**
 * @file use_limit.h
 * @brief 每日使用时长限制（《08 方案》§四）
 *
 * 规则：
 *  - 默认关闭（USE_LIMIT_ENABLE=0）；
 *  - 计数口径 = 助手 TTS 实际播放时长（kStateSpeaking 真实区间累计，打断少计）；
 *  - 达限锁定至当日零点：唤醒被拦截（不连服务器，仅播提醒音）；会话中触限则
 *    由调用方负责断开与清理（本模块只报状态并提示一次）；
 *  - 持久化走 libapconfig（set_config_int+sync_config），当日跨重启有效；
 *    零点滚动按本地时间 YYYYMMDD 重置。
 *
 * 占位提醒音 id 默认 14（msg_server Package.dat “下次再见”类），
 * 用户录音替换时仅改配置键 USE_LIMIT_PROMPT_ID。
 */

#ifndef USE_LIMIT_H
#define USE_LIMIT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 配置键（config.bin/libapconfig 域） */
#define UL_KEY_ENABLE     "USE_LIMIT_ENABLE"
#define UL_KEY_MINUTES    "USE_LIMIT_MINUTES"
#define UL_KEY_PROMPT_ID  "USE_LIMIT_PROMPT_ID"   /* 占位默认 14 */
#define UL_KEY_SPENT      "USE_LIMIT_SPENT_SEC"
#define UL_KEY_DAY        "USE_LIMIT_DAY"

void use_limit_init(void);

/** TTS 播放打点：Speaking 进入/退出各调一次，间隔毫秒数由调用方给出 */
void use_limit_on_speaking(uint64_t elapsed_ms);

/** 唤醒事件入口处调用；返回 1=已锁定（内部已播提醒音，调用方必须拦截本次唤醒） */
int use_limit_should_block_wakeup(void);

/** 当前已累计使用秒数(MCP查询用) */
long use_limit_spent_sec(void);

/** 当前是否处于达限锁定态（用于会话中判断） */
int use_limit_is_locked(void);

/** 会话中触限后的善后查询：返回 1 表示需要断开清理（一次性消费标志） */
int use_limit_take_session_break_flag(void);

#ifdef __cplusplus
}
#endif

#endif /* USE_LIMIT_H */
