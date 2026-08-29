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
#define UL_KEY_DELAY_UNTIL "USE_LIMIT_DELAY_UNTIL" /* 临时延迟到期epoch秒, 当日有效 */

void use_limit_init(void);

/** TTS 播放打点：Speaking 进入/退出各调一次，间隔毫秒数由调用方给出 */
void use_limit_on_speaking(uint64_t elapsed_ms);

/** 唤醒事件入口处调用；返回 1=已锁定（内部已播提醒音，调用方必须拦截本次唤醒） */
int use_limit_should_block_wakeup(void);

/** 当前已累计使用秒数(MCP查询用) */
long use_limit_spent_sec(void);

/** 当前是否处于达限锁定态（动态判定: 达限且不在延迟窗口内） */
int use_limit_is_locked(void);

/** 会话中触限后的善后查询：返回 1 表示需要断开清理（一次性消费标志） */
int use_limit_take_session_break_flag(void);

/* ---- 运行时设置(面板/语音, 见 2026-08-30 补全) ---- */
/** 开/关限时(写配置+生效) */
void use_limit_set_enable(int on);
/** 设上限分钟数(0-1440, 写配置+生效) */
void use_limit_set_minutes(int minutes);
/** 临时延迟: 达限后豁免锁定 minutes 分钟, 返回到期 epoch 秒 */
long use_limit_request_delay(int minutes);
/** 当前延迟到期 epoch 秒(0=无延迟) */
long use_limit_delay_until(void);
/** 上限分钟数(0=未设) */
int use_limit_get_minutes(void);
/** 限时开关状态 */
int use_limit_get_enable(void);

#ifdef __cplusplus
}
#endif

#endif /* USE_LIMIT_H */
