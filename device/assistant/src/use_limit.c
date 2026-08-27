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

    load_day_spent();
    g_ul.locked = (g_ul.enable && g_ul.limit_sec > 0 && g_ul.spent_sec >= g_ul.limit_sec);

    PLOG_I("UL", "init: enable=%d minutes=%ld spent=%ld%s",
           g_ul.enable, minutes, g_ul.spent_sec, g_ul.locked ? " [LOCKED]" : "");
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
    if (!g_ul.enable || g_ul.locked || elapsed_ms == 0)
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
    return g_ul.enable ? g_ul.locked : 0;
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
