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
#include <unistd.h>


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
    int delay_tool;         /* 语音延迟工具开关(家长面板), 默认 0 */
    int sched_enable;       /* 使用时段限制开关 */
    int sched_days;         /* 允许星期位图 bit0=周日..bit6=周六 */
    char sched_spans[UL_MAX_SPANS][12]; /* "HHMM-HHMM", 空=不限(多时段 2026-08-31) */
    uint64_t break_pending_ms;  /* 会话中断延迟窗到期时刻(MONO ms), 0=无 */
    uint64_t persist_last_ms;
    long persisted_min;     /* 上次落盘时的 spent 整分(整分对齐节流) */
} ul_ctx_t;

/* 前向声明: 加载迁移在函数定义前使用 */
static void ul_parse_spans(const char *csv);

/* 会话中断延迟: 播提示后给提示音留的播放时间 */
#define UL_BREAK_DELAY_MS 2500

static ul_ctx_t g_ul;
/* 耗尽提示音: 优先自定义 mp3(用户制作, 原生 music_player 播放),
 * 无文件回退原生 tts 占位音 */
#define UL_PROMPT_MP3 "/var/upgrade/use_limit_exhausted.mp3"

void use_limit_play_prompt(void)
{
    if (access(UL_PROMPT_MP3, R_OK) == 0 &&
        platform_media_play_file(UL_PROMPT_MP3) == 0)
        return;
    /* 自定义提示音缺失: 必须告警。静默回退会播成原生占位音(USE_LIMIT_PROMPT_ID
     * 落在网络/绑定类语音区间), 现象是"达限后播'我已经连上网啦'", 只能靠听感
     * 发现(2026-10-03 线上问题: 部署流程未随 sair 推送该 mp3) */
    PLOG_W("UL", "触限提示音缺失(%s), 回退原生占位音 id=%d",
           UL_PROMPT_MP3, g_ul.prompt_id);
    platform_tts_play(g_ul.prompt_id);
}

static void today_str(char out[9])
{
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    snprintf(out, 9, "%04d%02d%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
}

/* 跨日重置(内存+落盘一次写齐): DAY/SPENT/DELAY_UNTIL 必须同步落盘——
 * 只改 DAY 不写 SPENT, 重启后 load_day_spent 会用旧 SPENT 把新的一天
 * 直接锁死(2026-10-09 事故形态之一)。仅主循环线程调用 */
static void ul_crossday_reset(const char *cur)
{
    PLOG_I("UL", "跨日重置 (%s -> %s)", g_ul.day, cur);
    strncpy(g_ul.day, cur, sizeof(g_ul.day));
    g_ul.spent_sec = 0;
    g_ul.locked = 0;
    g_ul.delay_until = 0; /* 延迟随当日失效 */
    set_config(UL_KEY_DAY, g_ul.day, strlen(g_ul.day));
    cfg_set_int(UL_KEY_SPENT, 0);
    cfg_set_int(UL_KEY_DELAY_UNTIL, 0);
    sync_config();
}

static void load_day_spent(void)
{
    get_config(UL_KEY_DAY, g_ul.day, sizeof(g_ul.day));
    char cur[9];
    today_str(cur);
    if (strcmp(cur, g_ul.day) != 0)
    {
        /* 新的一天（或首次）：清零并落盘 */
        ul_crossday_reset(cur);
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
    int dirty = 0;
    memset(&g_ul, 0, sizeof(g_ul));
    g_ul.enable = cfg_get_int(UL_KEY_ENABLE, 0);
    long minutes = cfg_get_int(UL_KEY_MINUTES, 60);
    g_ul.limit_sec = minutes > 0 ? minutes * 60 : 0;
    g_ul.prompt_id = cfg_get_int(UL_KEY_PROMPT_ID, 14);
    g_ul.delay_until = cfg_get_int(UL_KEY_DELAY_UNTIL, 0);
    g_ul.delay_tool = cfg_get_int(UL_KEY_DELAY_TOOL, 0);          /* 默认关闭 */
    g_ul.sched_enable = cfg_get_int(UL_KEY_SCHED_ENABLE, 0);
    g_ul.sched_days = cfg_get_int(UL_KEY_SCHED_DAYS, 127);        /* 默认每天 */
    /* 多时段(2026-08-31): CSV 键优先; 未写过时迁移旧 SPAN1/SPAN2.
     * 坑(实测): 此处若用块作用域局部缓冲 { char csv[64]; ... }, ASR 线程
     * 启动后随机 SIGSEGV(fault=栈上字符串前4字节被当指针, PC 落 libc rw 页)——
     * 块作用域变量在此编译环境(uClibc+本函数体量)下栈帧布局异常.
     * 静态缓冲彻底绕开(加载路径仅启动时执行一次, 无并发) */
    static char s_csv[64];
    if (get_config(UL_KEY_SCHED_SPANS, s_csv, sizeof(s_csv)) <= 0)
    {
        get_config(UL_KEY_SCHED_SPAN1, g_ul.sched_spans[0], sizeof(g_ul.sched_spans[0]));
        get_config(UL_KEY_SCHED_SPAN2, g_ul.sched_spans[1], sizeof(g_ul.sched_spans[1]));
    }
    else
    {
        ul_parse_spans(s_csv);
    }

    /* config.bin 脏数据自愈(2026-08-30): 历史解析器 bug 曾把 epoch 秒串进
     * 各配置键并落盘(sched_enable=788064597 之类), 跨重启复活导致时段锁
     * 误开/唤醒全拦. 值域校验不过即重置默认并落盘覆盖脏值 */
    if (g_ul.enable != 0 && g_ul.enable != 1)
    {
        /* 非01脏值按"非0即开"语义归一(2026-10-09 实测 enable=10):
         * 重置为0会违背用户已开启的意图, 归一为1保留开启状态 */
        PLOG_W("UL", "ENABLE 脏值(%d), 归一为1并落盘", g_ul.enable);
        g_ul.enable = 1;
        cfg_set_int(UL_KEY_ENABLE, 1);
        dirty = 1;
    }
    if (minutes < 0 || minutes > 24 * 60)
    {
        PLOG_W("UL", "MINUTES 脏值(%ld), 重置60并落盘", minutes);
        minutes = 60;
        g_ul.limit_sec = 60 * 60;
        cfg_set_int(UL_KEY_MINUTES, 60);
        dirty = 1;
    }
    if (g_ul.sched_enable != 0 && g_ul.sched_enable != 1)
    {
        PLOG_W("UL", "SCHED_ENABLE 脏值(%d), 重置为0并落盘", g_ul.sched_enable);
        g_ul.sched_enable = 0;
        cfg_set_int(UL_KEY_SCHED_ENABLE, 0);
        dirty = 1;
    }
    if (g_ul.sched_days < 0 || g_ul.sched_days > 127)
    {
        PLOG_W("UL", "SCHED_DAYS 脏值(%d), 重置127并落盘", g_ul.sched_days);
        g_ul.sched_days = 127;
        cfg_set_int(UL_KEY_SCHED_DAYS, 127);
        dirty = 1;
    }
    if (g_ul.delay_tool != 0 && g_ul.delay_tool != 1)
    {
        PLOG_W("UL", "DELAY_TOOL 脏值(%d), 重置0并落盘", g_ul.delay_tool);
        g_ul.delay_tool = 0;
        cfg_set_int(UL_KEY_DELAY_TOOL, 0);
        dirty = 1;
    }
    if (g_ul.delay_until < 0 ||
        g_ul.delay_until > (long)time(NULL) + 12 * 60 * 60)
    {
        /* 未来超过12小时的延迟必为脏值(合法上限12h) */
        PLOG_W("UL", "DELAY_UNTIL 脏值(%ld), 重置0并落盘", g_ul.delay_until);
        g_ul.delay_until = 0;
        cfg_set_int(UL_KEY_DELAY_UNTIL, 0);
        dirty = 1;
    }
    if (dirty)
        sync_config();

    load_day_spent();
    g_ul.locked = (g_ul.enable && g_ul.limit_sec > 0 && g_ul.spent_sec >= g_ul.limit_sec);

    PLOG_I("UL", "init: enable=%d minutes=%ld spent=%ld delay_until=%ld delay_tool=%d "
           "sched=%d days=0x%x span1='%s' span2='%s'%s",
           g_ul.enable, minutes, g_ul.spent_sec, g_ul.delay_until, g_ul.delay_tool,
           g_ul.sched_enable, g_ul.sched_days, use_limit_get_spans_csv(),
           g_ul.locked ? " [LOCKED]" : "");
}

static void persist_if_needed(uint64_t now_ms)
{
    /* 打点节流：30s 且 spent 已跨整分才落盘(2026-08-31: 每次落盘=
     * cfg_set_int+sync_config 的 NAND 写, 会话期30s一次持续磨损闪存;
     * 分钟级限额语义下 60s 粒度无感知差异, 跨重启误差≤60s) */
    if (now_ms - g_ul.persist_last_ms < 30000)
        return;
    if ((long)(g_ul.spent_sec / 60) == (long)(g_ul.persisted_min))
        return;
    g_ul.persist_last_ms = now_ms;
    g_ul.persisted_min = (long)(g_ul.spent_sec / 60);
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
        ul_crossday_reset(cur);

    g_ul.spent_sec += (long)(elapsed_ms / 1000);
    PLOG_D("UL", "speaking %llums -> spent=%lds/%lds",
           (unsigned long long)elapsed_ms, g_ul.spent_sec, g_ul.limit_sec);

    if (g_ul.limit_sec > 0 && g_ul.spent_sec >= g_ul.limit_sec)
    {
        g_ul.locked = 1;
        PLOG_I("UL", "今日使用时长已耗尽(%lds), 进入锁定(断开由主循环 poll 处理)", g_ul.spent_sec);
    }
    persist_if_needed(now_ms);
}

/* 周期跨日检查(主循环 2s 慢检查调用)。原实现跨日重置只挂在
 * use_limit_on_speaking(即 Speaking 退出)上: 达限锁定后唤醒全被拦截,
 * 永远进不了 Speaking——锁阻止了唯一能解锁自己的代码路径, 空闲态跨日
 * 也不解锁, 只能靠重启救(2026-10-09 事故: 21:25 达限, 次日 0:55 起
 * 唤醒全拦, 1:22 重启才解)。挂到主循环后空闲态也能自然跨日解锁 */
void use_limit_tick(void)
{
    if (!g_ul.enable)
        return;
    char cur[9];
    today_str(cur);
    if (strcmp(cur, g_ul.day) == 0)
        return;
    ul_crossday_reset(cur);
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

/* ---- 使用时段限制(作息锁, 与达限锁独立, 延迟不豁免) ---- */

static int span_hit(const char *span, int hhmm)
{
    /* "HHMM-HHMM" 判定 hhmm 是否在窗口内; 支持跨午夜(如 2100-0700) */
    if (!span[0])
        return 0;
    const char *dash = strchr(span, '-');
    if (!dash)
        return 0;
    int a = atoi(span);
    int b = atoi(dash + 1);
    if (a == b)
        return 0;
    if (a < b)
        return hhmm >= a && hhmm < b;
    return hhmm >= a || hhmm < b; /* 跨午夜 */
}

int use_limit_out_of_span(void)
{
    if (!g_ul.sched_enable)
        return 0;
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    if (tmv.tm_year + 1900 < 2020)
        return 0; /* RTC 未对时(默认2018), 时段判定不可靠, 不误锁 */
    if (!(g_ul.sched_days & (1 << tmv.tm_wday)))
        return 1; /* 今天不在允许星期 */
    int hhmm = tmv.tm_hour * 100 + tmv.tm_min;
    int any = 0;
    for (int i = 0; i < UL_MAX_SPANS; i++)
    {
        if (!g_ul.sched_spans[i][0])
            continue;
        any = 1;
        if (span_hit(g_ul.sched_spans[i], hhmm))
            return 0;
    }
    if (!any)
        return 0; /* 未设时段=全天允许 */
    return 1;
}

/* 总锁定 = 达限锁 || 时段锁 (唤醒拦截与会话中断用它) */
static int ul_any_locked(void)
{
    return use_limit_is_locked() || use_limit_out_of_span();
}

/* ---- 运行时设置(面板/语音, 2026-08-30 补全) ---- */

void use_limit_set_enable(int on)
{
    g_ul.enable = on ? 1 : 0;
    cfg_set_int(UL_KEY_ENABLE, g_ul.enable);
    sync_config();
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

int use_limit_delay_tool_enabled(void)
{
    return g_ul.delay_tool;
}

void use_limit_set_delay_tool(int on)
{
    g_ul.delay_tool = on ? 1 : 0;
    cfg_set_int(UL_KEY_DELAY_TOOL, g_ul.delay_tool);
    sync_config();
    PLOG_I("UL", "语音延迟工具=%d", g_ul.delay_tool);
}

/* CSV 拆到 spans 数组: "1600-2000,2100-2200" */
static void ul_parse_spans(const char *csv)
{
    memset(g_ul.sched_spans, 0, sizeof(g_ul.sched_spans));
    if (!csv)
        return;
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%s", csv);
    char *save = NULL;
    char *tok = strtok_r(tmp, ",", &save);
    int n = 0;
    while (tok && n < UL_MAX_SPANS)
    {
        while (*tok == ' ')
            tok++;
        if (tok[0])
            snprintf(g_ul.sched_spans[n], sizeof(g_ul.sched_spans[n]), "%s", tok);
        n++;
        tok = strtok_r(NULL, ",", &save);
    }
}

/* 全部时段落盘(CSV 单键) */
static void ul_save_spans(void)
{
    char csv[64] = "";
    int off = 0;
    for (int i = 0; i < UL_MAX_SPANS && g_ul.sched_spans[i][0]; i++)
    {
        int n = snprintf(csv + off, sizeof(csv) - off, "%s%s",
                         off ? "," : "", g_ul.sched_spans[i]);
        if (n < 0)
            break;
        off += n;
    }
    set_config(UL_KEY_SCHED_SPANS, csv, strlen(csv));
}

void use_limit_set_schedule(int enable, int days, const char *span1, const char *span2)
{
    char csv[32] = "";
    snprintf(csv, sizeof(csv), "%s%s%s",
             span1 ? span1 : "", (span2 && span2[0]) ? "," : "", span2 ? span2 : "");
    use_limit_set_spans(enable, days, csv);
}

void use_limit_set_spans(int enable, int days, const char *spans_csv)
{
    g_ul.sched_enable = enable ? 1 : 0;
    if (days < 0 || days > 127)
        days = 127;
    g_ul.sched_days = days;
    ul_parse_spans(spans_csv);
    cfg_set_int(UL_KEY_SCHED_ENABLE, g_ul.sched_enable);
    cfg_set_int(UL_KEY_SCHED_DAYS, g_ul.sched_days);
    ul_save_spans();
    sync_config();
    PLOG_I("UL", "时段设置: enable=%d days=0x%x spans='%s'",
           g_ul.sched_enable, g_ul.sched_days, use_limit_get_spans_csv());
}

int use_limit_get_sched_enable(void)
{
    return g_ul.sched_enable;
}

int use_limit_get_days(void)
{
    return g_ul.sched_days;
}

const char *use_limit_get_span(int idx)
{
    if (idx < 0 || idx >= UL_MAX_SPANS)
        return "";
    return g_ul.sched_spans[idx];
}

int use_limit_get_span_count(void)
{
    int n = 0;
    for (int i = 0; i < UL_MAX_SPANS; i++)
        if (g_ul.sched_spans[i][0])
            n++;
    return n;
}

const char *use_limit_get_spans_csv(void)
{
    static char csv[64];
    int off = 0;
    csv[0] = '\0';
    for (int i = 0; i < UL_MAX_SPANS && g_ul.sched_spans[i][0]; i++)
    {
        int n = snprintf(csv + off, sizeof(csv) - off, "%s%s",
                         off ? "," : "", g_ul.sched_spans[i]);
        if (n < 0)
            break;
        off += n;
    }
    return csv;
}

int use_limit_should_block_wakeup(void)
{
    if (!ul_any_locked())
        return 0;

    /* 每次唤醒都播提示(2026-08-30 用户决策): 只播一次会让孩子以为设备坏了.
     * 唤醒引擎在提示音播放中会连续误触发(实测 200ms/次提示风暴).
     * 2026-08-31 死循环事故: 提示音走原生 music_player 通道, ASR 无 AEC,
     * 提示音人声自己触发唤醒, 播完 1.4s 后重播, 每 5s 一轮无限循环.
     * 修: 播放期(platform_media_is_playing)静默拦截并持续推节流窗,
     * 播完后冷却窗(8s, 覆盖尾音延迟判定)内同样静默不重播 */
#define UL_PROMPT_COOLDOWN_MS 8000
    static uint64_t last_prompt_ms = 0;
    uint64_t now = ul_now_ms();
    if (platform_media_is_playing())
    {
        last_prompt_ms = now;
        return 1;
    }
    if (last_prompt_ms == 0 || now - last_prompt_ms >= UL_PROMPT_COOLDOWN_MS)
    {
        last_prompt_ms = now;
        PLOG_I("UL", "唤醒被限时拦截(达限锁=%d 时段锁=%d), 播提示(id=%d)",
               use_limit_is_locked(), use_limit_out_of_span(), g_ul.prompt_id);
        use_limit_play_prompt();
    }
    return 1;
}

/* ---- 会话中断轮询(达限/时段锁统一入口) ---- */

int use_limit_session_break_poll(void)
{
    if (!ul_any_locked())
    {
        g_ul.break_pending_ms = 0;
        return 0;
    }

    if (g_ul.break_pending_ms == 0)
    {
        /* 首次进入锁定: 播提示并开延迟窗(等提示播完再断) */
        use_limit_play_prompt();
        g_ul.break_pending_ms = ul_now_ms() + UL_BREAK_DELAY_MS;
        PLOG_I("UL", "会话中锁定生效, 播提示后%ums断开(达限锁=%d 时段锁=%d)",
               UL_BREAK_DELAY_MS, use_limit_is_locked(), use_limit_out_of_span());
        return 0;
    }
    return ul_now_ms() >= g_ul.break_pending_ms;
}

void use_limit_break_reset(void)
{
    g_ul.break_pending_ms = 0;
}
