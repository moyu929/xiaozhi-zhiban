/**
 * @file use_limit.c
 * @brief 每日使用时长限制实现（见 use_limit.h）
 */
#include "use_limit.h"
#include "platform_power.h"
#include "plog.h"

#include "applib_api.h"      /* get_config/set_config_int/sync_config 包装 */

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

static uint64_t ul_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + ts.tv_nsec / 1000000u;
}


/* ---- 配置存取（真库仅导出 get/set/sync_config, C1 §5/dynsym 定论） ---- */
static int cfg_get_int(const char *key, int def)
{
    char b[32];
    if (get_config(key, b, sizeof(b)) > 0 && b[0])
        return atoi(b);
    return def;
}
static void cfg_set_int(const char *key, int v)
{
    char b[32];
    snprintf(b, sizeof(b), "%d", v);
    set_config(key, b, strlen(b));
}
typedef struct {
    int enable;
    long limit_sec;
    int prompt_id;
    long spent_sec;
    char day[9];            /* YYYYMMDD */
    int locked;
    long delay_until;       /* 临时延迟到期 epoch 秒, 0=无 */
    int wake_block_prompted;
    int session_break_pending;
    uint64_t persist_last_ms;
} ul_ctx_t;

static ul_ctx_t g_ul;

static void today_str(char out[9])
{
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    snprintf(out, 9, "%04d%02d%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
}

static void load_day_spent(void)
{
    get_config(UL_KEY_DAY, g_ul.day, sizeof(g_ul.day));
    char cur[9];
    today_str(cur);
    if (strcmp(cur, g_ul.day) != 0)
    {
        /* 新的一天（或首次）：清零并落盘 */
        strncpy(g_ul.day, cur, sizeof(g_ul.day));
        g_ul.spent_sec = 0;
        set_config(UL_KEY_DAY, g_ul.day, strlen(g_ul.day));
        cfg_set_int(UL_KEY_SPENT, 0);
        cfg_set_int(UL_KEY_DELAY_UNTIL, 0); /* 昨日的延迟不跨天 */
        g_ul.delay_until = 0;
        sync_config();
    }
    else
    {
        g_ul.spent_sec = cfg_get_int(UL_KEY_SPENT, 0);
        if (g_ul.spent_sec < 0)
            g_ul.spent_sec = 0;
    }
}

void use_limit_init(void)
{
    memset(&g_ul, 0, sizeof(g_ul));
    g_ul.enable = cfg_get_int(UL_KEY_ENABLE, 0);
    long minutes = cfg_get_int(UL_KEY_MINUTES, 60);
    g_ul.limit_sec = minutes > 0 ? minutes * 60 : 0;
    g_ul.prompt_id = cfg_get_int(UL_KEY_PROMPT_ID, 14);
    g_ul.delay_until = cfg_get_int(UL_KEY_DELAY_UNTIL, 0);

    load_day_spent();
    g_ul.locked = (g_ul.enable && g_ul.limit_sec > 0 && g_ul.spent_sec >= g_ul.limit_sec);

    PLOG_I("UL", "init: enable=%d minutes=%ld spent=%ld delay_until=%ld%s",
           g_ul.enable, minutes, g_ul.spent_sec, g_ul.delay_until,
           g_ul.locked ? " [LOCKED]" : "");
}

static void persist_if_needed(uint64_t now_ms)
{
    /* 打点节流：30s 或由 speaking 结束强制触发 */
    if (now_ms - g_ul.persist_last_ms < 30000)
        return;
    g_ul.persist_last_ms = now_ms;
    cfg_set_int(UL_KEY_SPENT, (int)g_ul.spent_sec);
    sync_config();
}

void use_limit_on_speaking(uint64_t elapsed_ms)
{
    /* 注意: 不以 locked 位短路——延迟窗口内 spent 继续计数(真实用量),
     * 锁定与否由 use_limit_is_locked() 动态判定 */
    if (!g_ul.enable || elapsed_ms == 0)
    {
        persist_if_needed(ul_now_ms());
        return;
    }

    uint64_t now_ms = ul_now_ms();

    /* 可能已跨天（长时间播放中） */
    char cur[9];
    today_str(cur);
    if (strcmp(cur, g_ul.day) != 0)
    {
        PLOG_I("UL", "跨日重置 (%s -> %s)", g_ul.day, cur);
        strncpy(g_ul.day, cur, sizeof(g_ul.day));
        g_ul.spent_sec = 0;
        set_config(UL_KEY_DAY, g_ul.day, strlen(g_ul.day));
        g_ul.locked = 0;
        g_ul.wake_block_prompted = 0;
        g_ul.delay_until = 0; /* 延迟随当日失效 */
        cfg_set_int(UL_KEY_DELAY_UNTIL, 0);
    }

    g_ul.spent_sec += (long)(elapsed_ms / 1000);
    PLOG_D("UL", "speaking %llums -> spent=%lds/%lds",
           (unsigned long long)elapsed_ms, g_ul.spent_sec, g_ul.limit_sec);

    if (g_ul.limit_sec > 0 && g_ul.spent_sec >= g_ul.limit_sec)
    {
        g_ul.locked = 1;
        g_ul.session_break_pending = 1;
        PLOG_I("UL", "今日使用时长已耗尽(%lds), 进入锁定", g_ul.spent_sec);
    }
    persist_if_needed(now_ms);
}

long use_limit_spent_sec(void)
{
    return g_ul.spent_sec;
}

int use_limit_is_locked(void)
{
    if (!g_ul.enable || g_ul.limit_sec <= 0)
        return 0;
    if (g_ul.spent_sec < g_ul.limit_sec)
        return 0;
    /* 达限但处于临时延迟窗口: 不锁(动态判定, 到期自动回锁) */
    if (g_ul.delay_until > (long)time(NULL))
        return 0;
    return 1;
}

/* ---- 运行时设置(面板/语音, 2026-08-30 补全) ---- */

void use_limit_set_enable(int on)
{
    g_ul.enable = on ? 1 : 0;
    cfg_set_int(UL_KEY_ENABLE, g_ul.enable);
    sync_config();
    if (!g_ul.enable)
        g_ul.wake_block_prompted = 0; /* 关闭后允许重新提示 */
    PLOG_I("UL", "set_enable=%d (minutes=%ld spent=%ld)",
           g_ul.enable, g_ul.limit_sec / 60, g_ul.spent_sec);
}

void use_limit_set_minutes(int minutes)
{
    if (minutes < 0)
        minutes = 0;
    if (minutes > 24 * 60)
        minutes = 24 * 60;
    g_ul.limit_sec = (long)minutes * 60;
    cfg_set_int(UL_KEY_MINUTES, minutes);
    sync_config();
    if (g_ul.spent_sec < g_ul.limit_sec)
        g_ul.wake_block_prompted = 0; /* 上限提高后未超限, 允许重新提示 */
    PLOG_I("UL", "set_minutes=%d (spent=%lds)", minutes, g_ul.spent_sec);
}

long use_limit_request_delay(int minutes)
{
    if (minutes <= 0)
        return g_ul.delay_until;
    if (minutes > 12 * 60)
        minutes = 12 * 60;
    g_ul.delay_until = (long)time(NULL) + (long)minutes * 60;
    cfg_set_int(UL_KEY_DELAY_UNTIL, (int)g_ul.delay_until);
    sync_config();
    g_ul.wake_block_prompted = 0; /* 延迟到期回锁时重新提示一次 */
    PLOG_I("UL", "临时延迟 %d 分钟, 到期 epoch=%ld", minutes, g_ul.delay_until);
    return g_ul.delay_until;
}

long use_limit_delay_until(void)
{
    return g_ul.delay_until;
}

int use_limit_get_minutes(void)
{
    return (int)(g_ul.limit_sec / 60);
}

int use_limit_get_enable(void)
{
    return g_ul.enable;
}

int use_limit_take_session_break_flag(void)
{
    int v = g_ul.session_break_pending;
    g_ul.session_break_pending = 0;
    return v;
}

int use_limit_should_block_wakeup(void)
{
    if (!use_limit_is_locked())
        return 0;

    if (!g_ul.wake_block_prompted)
    {
        g_ul.wake_block_prompted = 1;
        PLOG_I("UL", "唤醒请求被限时拦截, 播提醒音(id=%d)", g_ul.prompt_id);
        platform_tts_play(g_ul.prompt_id);
    }
    return 1;
}
