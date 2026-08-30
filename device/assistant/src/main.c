/**
 * @file main.c
 * @brief 小智助手主程序入口
 *
 * 本文件是小智AI语音助手的核心主程序，负责：
 * - 系统初始化（信号处理、调度优先级、applib初始化）
 * - 热更新检测与执行
 * - 主事件循环（唤醒处理、按键处理、超时检查、OTA配置）
 * - 各子线程管理（录音线程、消息线程、OTA线程、连接线程）
 * - 状态机驱动与状态变更回调
 * - WebSocket协议回调处理（音频、JSON消息）
 * - IPC消息处理（系统消息、服务消息）
 * - 资源清理与退出
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>
#include <sched.h>
#include <stdarg.h>
#include <time.h>
#include <sys/prctl.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <mqueue.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <linux/input.h>
#include <poll.h>
#include <dirent.h>

#include <math.h>

#include "reverse/applib_api.h"
#include "reverse/audio_recorder_api.h"
#include "xiaozhi_config.h"
#include "state_machine.h"
#include "watchdog.h"
#include "use_limit.h"
#include "display_ctrl.h"
#include "audioproc.h"
#include "config_manager.h"
#include "plog.h"
#include "wakeup_module.h"
#include "audio_dispatcher.h"
#include "touch_key.h"
#include "protocol_handler.h"
#include "audio_player.h"
#include "audio_recorder.h"
#include "mcp_handler.h"
#include "api_server.h"
#include "app_context.h"

/* 解决某些工具链缺少 __nan 链接符号的问题 */
int __isnan(double x) { return x != x; }

/**
 * @brief 堆内存健康检查
 * @param tag 检查标签，用于日志标识
 * @return 0表示堆正常，-1表示堆内存分配失败
 */
static int heap_check(const char *tag)
{
    void *p = malloc(16);
    if (!p)
    {
        PLOG_E("HEAP", "[%s] malloc(16) 失败!", tag);
        return -1;
    }
    free(p);
    return 0;
}

/* 占位函数：dump接口的空实现，供applib框架调用 */
void *dump_open(void *ctx, int type, void *data) { return NULL; }
void dump_close(void *ctx) { (void)ctx; }
int dump_write(void *ctx, const void *data, int size)
{
    (void)ctx;
    (void)data;
    return size;
}

/* 平台SDK外部接口：软看门狗控制 */
extern int set_soft_watchdog_timeout(int timeout_ms);
extern int sys_forbid_soft_watchdog(int forbid);
extern int sys_is_soft_watchdog_forbid(void);

__attribute__((weak)) int set_soft_watchdog_timeout(int timeout_ms) { (void)timeout_ms; return -1; }
__attribute__((weak)) int sys_forbid_soft_watchdog(int forbid) { (void)forbid; return -1; }
__attribute__((weak)) int sys_is_soft_watchdog_forbid(void) { return 0; }

/* 全局运行标志，0=停止，1=运行中 */
static volatile int g_running = 0;
volatile sig_atomic_t g_hot_update_pending = 0;

typedef struct {
    int type;
    char content[SUBTITLE_MAX_LEN];
} sair_content_t;

static sair_content_t g_subtitle;
static pthread_mutex_t g_subtitle_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ---- 字幕流式(打字机) + 自动消失 (g_subtitle_mutex 保护) ----
 * 协议层 sentence_start 为整句下发(无字符级 delta, websocket.md 定论),
 * 故按 TTS 语音节奏(约 4 字/秒)本地逐字滚出, 视觉等效流式.
 * 句子完成后延迟 SUBTITLE_HIDE_MS 自动清屏, 避免残留. */
#define TYPE_MS_PER_CHAR 250
#define SUBTITLE_HIDE_MS 2000
static char g_type_full[SUBTITLE_MAX_LEN]; /* 待打字全文 */
static int g_type_chars;                   /* 全文 UTF-8 字符数 */
static int g_type_shown;                   /* 已显示字符数 */
static uint64_t g_type_start_ms;           /* 打字起点 */
static int g_type_done = 1;                /* 1=无任务/已完成 */
static uint64_t g_subtitle_hide_ms;        /* 计划清屏时刻(0=无计划) */

/** UTF-8 字符串前 max_chars 个字符的字节数(截断不撕裂多字节) */
static int utf8_prefix_bytes(const char *s, int max_chars)
{
    int chars = 0, i = 0;
    while (s[i] && chars < max_chars)
    {
        unsigned char c = (unsigned char)s[i];
        i += (c & 0x80) == 0 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
        chars++;
    }
    return i;
}

/** UTF-8 字符串字符数 */
static int utf8_char_count(const char *s)
{
    int chars = 0, i = 0;
    while (s[i])
    {
        unsigned char c = (unsigned char)s[i];
        i += (c & 0x80) == 0 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
        chars++;
    }
    return chars;
}

/* ---- Q7 回环防护: 字幕/表情/唤醒广播会触发 msg_server 回发 AI_START(0x3EB)
 * (实测 0x23A 广播后 3-5ms 回 0x3EB, 21:31:29 实录: 回环在 Speaking 态被当
 * 用户打断 -> 播放停止 + ignore_tts_audio -> TTS 音频帧全被静默丢弃).
 * 统一经本包装广播并记录时间, AI_START 短窗内到达即判回环忽略. ---- */
static uint64_t g_last_bcast_ms = 0;
/* 0x231(会话结束事件)发出时间: wiki 收到后回调 sair_ai_stop(0x3EC),
 * 该回环在窗内忽略, 防止字幕清屏误杀进行中的 AutoStop 会话 */
static uint64_t g_last_end_evt_ms = 0;
/* 唤醒广播(0x235)专用时间戳: msg_server 收到后契约性回发 AI_STOP(0x3EC),
 * 打断场景该回环杀死刚建立的新会话, AI_STOP 处理器以此时间戳开窗忽略 */
static uint64_t g_last_awake_bcast_ms = 0;
static uint64_t get_time_ms(void); /* 前向声明: 定义在下方, 本包装需先用 */

static int sair_broadcast(void *msg)
{
    int ret = broadcast_msg(msg);
    g_last_bcast_ms = get_time_ms();
    return ret;
}

/**
 * 原生字幕通道（2026-08-27 四次修正, 依 wiki.so _play_flash 内 0x233 分支
 * a6fc 反汇编定论 -- 实测 launcher 仅加载 wiki.so, 唤醒动画即 wiki 场景）：
 *   0x233 + buf[4]=5(事件) + buf[8..]=文本  → strcpy(ctx+56)+显示+gui_screen_update
 *   0x233 + buf[4]=7(事件) + buf[8..]=动画名 → swf 播放(表情通道)
 * 前置: wiki 运行态 [ctx+1140]!=1 且 [ctx+1260]<0。
 * evaluation.so 的 0x23A 协议(文本@+4)仅在 evaluation 场景加载时有效, 当前不适用。
 * 经 send_async_msg("launcher") 定向推送。
 */
static void sair_subtitle_push(const char *text)
{
    char buf[300];
    memset(buf, 0, sizeof(buf));
    *(int *)buf = 0x233;
    *(int *)(buf + 4) = 5; /* 事件 5 = 字幕文本 */
    if (text && text[0])
        strncpy(buf + 8, text, sizeof(buf) - 9);
    send_async_msg("launcher", buf);
    g_last_bcast_ms = get_time_ms(); /* 触发回环, 纳入防护窗 */
}

/** 清除屏幕残留字幕（会话结束/自动隐藏/打断时调用）
 * 两连发: 0x233+空文本清文字(仅清文字, 7678 只判 NULL 指针, 空串仍画容器)
 *        + 0x231 会话结束事件(wiki a888 空字幕路径: 重绘场景回 idle, 容器消失)
 * 注: wiki 收 0x231 会回调 sair_ai_stop(0x3EC), MSG_SAIR_AI_STOP 处理器
 *     依 g_last_end_evt_ms 窗口判回环忽略, 保护进行中的 AutoStop 会话. */
static void sair_subtitle_clear(void)
{
    char buf[72];
    memset(buf, 0, sizeof(buf));
    *(int *)buf = 0x233;
    *(int *)(buf + 4) = 5;
    send_async_msg("launcher", buf);
    memset(buf, 0, sizeof(buf));
    *(int *)buf = 0x231; /* 会话结束事件, payload[0]=0 → idle 化+清容器 */
    send_async_msg("launcher", buf);
    g_last_bcast_ms = get_time_ms();
    g_last_end_evt_ms = g_last_bcast_ms;
}

static void subtitle_set(int type, const char *text)
{
    pthread_mutex_lock(&g_subtitle_mutex);
    g_subtitle.type = type;
    if (text)
    {
        strncpy(g_subtitle.content, text, SUBTITLE_MAX_LEN - 1);
        g_subtitle.content[SUBTITLE_MAX_LEN - 1] = '\0';
    }
    else
    {
        g_subtitle.content[0] = '\0';
    }
    pthread_mutex_unlock(&g_subtitle_mutex);

    /* 需求②: 屏幕底部字幕仅显示回复(TTS)内容, 渲染由原生 wiki 场景完成 */
    if (type == SUBTITLE_TYPE_TTS && text && text[0])
    {
        /* 打字机流式: 立即显示句首 2 字, 余下由主循环 tick 按语音节奏推进 */
        char head[SUBTITLE_MAX_LEN];
        int bytes;
        pthread_mutex_lock(&g_subtitle_mutex);
        strncpy(g_type_full, text, SUBTITLE_MAX_LEN - 1);
        g_type_full[SUBTITLE_MAX_LEN - 1] = '\0';
        g_type_chars = utf8_char_count(g_type_full);
        g_type_shown = (g_type_chars > 2) ? 2 : g_type_chars;
        g_type_start_ms = get_time_ms();
        g_type_done = (g_type_shown >= g_type_chars);
        g_subtitle_hide_ms = 0; /* 新句到达, 取消挂起的清屏计划 */
        bytes = utf8_prefix_bytes(g_type_full, g_type_shown);
        memcpy(head, g_type_full, bytes);
        pthread_mutex_unlock(&g_subtitle_mutex);
        head[bytes] = '\0';
        sair_subtitle_push(head);
    }
    else if (type == SUBTITLE_TYPE_ASR)
    {
        /* 新一轮用户说话: 上一轮 TTS 字幕立即清屏, 打字机作废 */
        pthread_mutex_lock(&g_subtitle_mutex);
        g_type_done = 1;
        g_type_chars = 0;
        g_type_shown = 0;
        g_subtitle_hide_ms = 0;
        pthread_mutex_unlock(&g_subtitle_mutex);
        sair_subtitle_clear();
    }

    PLOG_I("SUB", "字幕更新: type=%d text='%.64s'", type, text ? text : "");
}

static void subtitle_clear(void)
{
    pthread_mutex_lock(&g_subtitle_mutex);
    g_subtitle.type = SUBTITLE_TYPE_NONE;
    g_subtitle.content[0] = '\0';
    g_type_done = 1;
    g_type_chars = 0;
    g_type_shown = 0;
    g_subtitle_hide_ms = 0;
    pthread_mutex_unlock(&g_subtitle_mutex);
    sair_subtitle_clear();
}

/** 打字机立即完成(整句全量上屏), TTS stop/打断兜底时调用 */
static void subtitle_finish_typing(void)
{
    char full[SUBTITLE_MAX_LEN];
    int need_push;
    pthread_mutex_lock(&g_subtitle_mutex);
    need_push = (!g_type_done && g_type_chars > 0);
    if (need_push)
    {
        memcpy(full, g_type_full, sizeof(full));
        g_type_done = 1;
        g_type_shown = g_type_chars;
    }
    pthread_mutex_unlock(&g_subtitle_mutex);
    if (need_push)
        sair_subtitle_push(full);
}

/** 安排字幕延迟消失(毫秒), tts stop 播放完成后调用 */
static void subtitle_schedule_hide(uint64_t delay_ms)
{
    pthread_mutex_lock(&g_subtitle_mutex);
    g_subtitle_hide_ms = get_time_ms() + delay_ms;
    pthread_mutex_unlock(&g_subtitle_mutex);
}

/** 字幕打断: 打字机作废 + 立即清屏(用户抢话/中止时调用) */
static void subtitle_interrupt(void)
{
    pthread_mutex_lock(&g_subtitle_mutex);
    g_type_done = 1;
    g_type_chars = 0;
    g_type_shown = 0;
    g_subtitle_hide_ms = 0;
    pthread_mutex_unlock(&g_subtitle_mutex);
    sair_subtitle_clear();
}

/**
 * @brief 字幕打字机推进 + 自动清屏 (主循环周期调用, 内部 150ms 节流)
 */
static void subtitle_typing_tick(void)
{
    static uint64_t last_tick_ms = 0;
    uint64_t now = get_time_ms();
    if (now - last_tick_ms < 150)
        return;
    last_tick_ms = now;

    /* 打字机推进 */
    pthread_mutex_lock(&g_subtitle_mutex);
    if (!g_type_done && g_type_chars > 0)
    {
        char full[SUBTITLE_MAX_LEN];
        memcpy(full, g_type_full, sizeof(full));
        int total_chars = g_type_chars;
        int shown = g_type_shown;
        pthread_mutex_unlock(&g_subtitle_mutex);

        int target = (int)((now - g_type_start_ms) / TYPE_MS_PER_CHAR);
        if (target < shown)
            target = shown;
        if (target > total_chars)
            target = total_chars;
        if (target > shown)
        {
            int bytes = utf8_prefix_bytes(full, target);
            full[bytes] = '\0';
            pthread_mutex_lock(&g_subtitle_mutex);
            g_type_shown = target;
            if (target >= total_chars)
                g_type_done = 1;
            pthread_mutex_unlock(&g_subtitle_mutex);
            sair_subtitle_push(full);
            return; /* 本 tick 已推送, 清屏下轮再看 */
        }
    }
    else
    {
        pthread_mutex_unlock(&g_subtitle_mutex);
    }

    /* 自动清屏 */
    pthread_mutex_lock(&g_subtitle_mutex);
    uint64_t hide_at = g_subtitle_hide_ms;
    pthread_mutex_unlock(&g_subtitle_mutex);
    if (hide_at && now >= hide_at && g_subtitle.type != SUBTITLE_TYPE_NONE)
    {
        PLOG_D("SUB", "字幕超时自动清屏");
        subtitle_clear();
    }
}

/* ---- 每日推送动画 (bot_push) 开关 (2026-08-30) ----
 * 开机联网后 olmedia_service 向原厂云端请求 pushBot 每日动画,
 * 下载到 /tmp/bot_push.swf, 下载完成事件触发广播 MSG_POWER_ON_PUSH(372),
 * manager 随即 exec /usr/lib/boot_push.so 播放. 原厂已停止内容运营,
 * 云端只返回固定的最后一期, 每次开机重复播放同一动画, 无意义.
 * 抑制方式(零侵入, 可逆): 把 /tmp/bot_push.swf 占位成目录 —
 * 下载器 open(O_CREAT) / rename() 落到目录上均失败, 下载完成事件
 * 不触发 → 不广播 → 不播放. 恢复只需 rmdir 占位, 下次下载成功
 * 即回到原生行为.
 * 配置: /var/upgrade/.boot_push_disable 内容 "1" 启用抑制(默认不抑制). */
#define BOT_PUSH_PATH "/tmp/bot_push.swf"
#define BOT_PUSH_DISABLE_CFG "/var/upgrade/.boot_push_disable"

/* boot_push_tick 内用, main.c 前向引用(定义在下方) */
extern app_context_t g_app;

int boot_push_disable_enabled(void)
{
    int fd = open(BOT_PUSH_DISABLE_CFG, O_RDONLY);
    if (fd < 0)
        return 0;
    char buf[16] = {0};
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    return (n > 0 && buf[0] == '1');
}

/* boot_push.so 的 bind 覆盖状态(/proc/mounts 查询) */
static int boot_push_bind_active(void)
{
    FILE *f = fopen("/proc/mounts", "r");
    if (!f)
        return 0;
    char line[256];
    int found = 0;
    while (fgets(line, sizeof(line), f))
    {
        if (strstr(line, "/usr/lib/boot_push.so"))
        {
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

/* 终极拦截: 用坏文件 bind mount 盖住 /usr/lib/boot_push.so —— launcher 懒加载
 * 时 dlopen 读到非 ELF 垃圾即失败, 每日动画场景代码根本不执行(黑屏无从发生).
 * rootfs 只读无法改名, bind 是唯一无损覆盖手段; 解除=umount 即回原生.
 * 时序: sair 主循环首 tick(~开机12s) bind 就位, 早于 372(~开机16s). */
static void boot_push_bind(int enable)
{
    if (enable)
    {
        if (boot_push_bind_active())
            return;
        int fd = open("/var/upgrade/.bad_so_stub", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0)
        {
            write(fd, "NOTELF", 6);
            close(fd);
        }
        if (system("mount --bind /var/upgrade/.bad_so_stub /usr/lib/boot_push.so 2>/dev/null") == 0)
            PLOG_I("PUSH", "boot_push.so 已被坏文件 bind 覆盖(launcher dlopen 将失败)");
        else
            PLOG_W("PUSH", "boot_push.so bind 失败: %s", strerror(errno));
    }
    else
    {
        if (boot_push_bind_active())
        {
            if (system("umount /usr/lib/boot_push.so 2>/dev/null") == 0)
                PLOG_I("PUSH", "boot_push.so bind 已解除");
        }
    }
}

/* 幂等: enabled=1 → 普通文件删除后 mkdir 占位 + bind 覆盖 so;
 *       enabled=0 → 空占位目录移除 + bind 解除 */
static void boot_push_apply(int enabled)
{
    struct stat st;
    int is_dir = (lstat(BOT_PUSH_PATH, &st) == 0 && S_ISDIR(st.st_mode));

    if (enabled)
    {
        if (!is_dir)
        {
            if (lstat(BOT_PUSH_PATH, &st) == 0)
                unlink(BOT_PUSH_PATH); /* 旧 swf 残留, 清掉省 tmpfs */
            if (mkdir(BOT_PUSH_PATH, 0755) == 0)
                PLOG_I("PUSH", "每日动画已抑制: %s 占位为目录(下载写入将失败)", BOT_PUSH_PATH);
            else
                PLOG_W("PUSH", "每日动画占位失败: errno=%d", errno);
        }
    }
    else
    {
        if (is_dir)
        {
            if (rmdir(BOT_PUSH_PATH) == 0)
                PLOG_I("PUSH", "每日动画已恢复: 占位目录移除, 下次联网重新下载");
        }
    }
    boot_push_bind(enabled);
}

static void boot_push_tick(void)
{
    static uint64_t last_ms = 0;
    uint64_t now = get_time_ms();
    int enabled = boot_push_disable_enabled();

    /* 抑制期 1s 粒度(bind 维护+背光自愈), 非抑制 60s 维护 */
    if (now - last_ms < (enabled ? 1000 : 60000))
        return;
    last_ms = now;

    boot_push_apply(enabled);
    if (!enabled)
        return;

    /* 开机窗口背光误关自愈(直接状态修复, 非按键注入): 开机链中背光被关
     * (bl_power=4)且无人恢复——实测与 372 相关但非严格伴随(21:28 轮无 372
     * 仍被关), 故不锚定广播, 直接监测状态. 启动后 5 分钟窗口内: 背光被关
     * 且非自研 display_ctrl 息屏态则复原; 自研息屏(display_ctrl)与 MCP
     * 手动息屏均置 self_off, 不会被误开.
     * 注意: 原生屏保关背光也是 bl_power=4, 窗口内若屏保先关会被复原
     * (开机 5 分钟内不息屏), 窗口过后屏保正常接管——可接受的边界. */
    {
        static uint64_t last_chk_ms = 0;
        uint64_t up_ms = now - g_app.boot_time_ms;
        if (up_ms <= 300000 && now - last_chk_ms >= 2000)
        {
            last_chk_ms = now;
            int blp = -1;
            int bfd = open("/sys/class/backlight/owl_backlight/bl_power", O_RDONLY);
            if (bfd >= 0)
            {
                char bb[16] = {0};
                if (read(bfd, bb, sizeof(bb) - 1) > 0)
                    blp = atoi(bb);
                close(bfd);
            }
            extern int display_ctrl_is_off(void);
            if (blp == 4 && !display_ctrl_is_off())
            {
                if (system("/etc/backlight.sh open 2>/dev/null") == 0)
                    PLOG_I("PUSH", "开机+%llums: 背光被误关(bl_power=4), 已复原",
                           (unsigned long long)up_ms);
            }
        }
    }

    /* 抑制期残留处理已废弃(2026-08-30): bind mount 拦截后 launcher
     * dlopen boot_push.so 直接失败, 进程根本不会存在, 无需监控/退出
     * 消息/按键注入(注入方案已被用户否决). 保留三层: 占位目录挡下载,
     * bind 挡 dlopen, 背光自愈挡黑屏残留. */
}


/* 全局应用上下文 */
app_context_t g_app;
app_info_t *g_this_app_info = NULL;

static int read_precache_enabled(void)
{
    char buf[256];
    int fd = open("/var/upgrade/xwebd_persist.conf", O_RDONLY);
    if (fd < 0)
        return 0;
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    char *p = strstr(buf, "precache=");
    if (p)
        return atoi(p + 9);
    return 0;
}

static void read_custom_ws_url(char *out, int out_size)
{
    out[0] = '\0';
    char buf[1024];
    int fd = open("/var/upgrade/xwebd_persist.conf", O_RDONLY);
    if (fd < 0)
        return;
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return;
    buf[n] = '\0';
    char *p = strstr(buf, "custom_ws_url=");
    if (p)
    {
        p += 14;
        int i = 0;
        while (*p && *p != '\n' && *p != '\r' && i < out_size - 1)
            out[i++] = *p++;
        out[i] = '\0';
    }
}

/**
 * @brief 获取单调时钟的毫秒时间戳
 * @return 当前毫秒时间戳（基于CLOCK_MONOTONIC）
 */
static uint64_t get_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void proc_srv_msg(void *req_header_ptr, int *resp_result);
static void proc_sys_msg(void *msg_ptr);
static void on_state_changed(xiaozhi_state_t from, xiaozhi_state_t to, void *user_data);
static int pre_applib_init(void);

/**
 * @brief MCP消息发送回调，通过WebSocket协议处理器发送JSON数据
 * @param json JSON数据指针
 * @param len JSON数据长度
 * @param user_data 用户数据，传入app_context_t指针
 * @return 0成功，-1失败
 */
static int mcp_send_handler(const char *json, size_t len, void *user_data)
{
    app_context_t *app = (app_context_t *)user_data;
    if (!app || !app->proto_initialized)
        return -1;
    return protocol_handler_send_json(&app->proto, json, len);
}

/* 保存applib可能覆盖前的SIGUSR1原始处理函数 */
static struct sigaction g_old_sigusr1_sa;

/* SIGUSR1信号接收标志，用于主循环检测 */
static volatile sig_atomic_t g_sigusr1_received = 0;

/**
 * @brief SIGUSR1信号处理函数
 *        设置标志位并链式调用旧的处理函数
 * @param sig 信号编号
 */
static void sigusr1_handler(int sig)
{
    g_sigusr1_received = 1;
    if (g_old_sigusr1_sa.sa_handler &&
        g_old_sigusr1_sa.sa_handler != SIG_IGN &&
        g_old_sigusr1_sa.sa_handler != SIG_DFL)
    {
        g_old_sigusr1_sa.sa_handler(sig);
    }
}

/**
 * @brief 通用信号处理函数
 *        SIGTERM/SIGINT: 设置停止运行标志
 * @param sig 信号编号
 */
static void signal_handler(int sig)
{
    if (sig == SIGTERM || sig == SIGINT)
    {
        g_running = 0;
    }
}

/**
 * @brief 崩溃信号处理函数（SIGSEGV/SIGABRT等）
 *        记录崩溃信息（信号类型、故障地址、PC/LR/SP寄存器）到日志和crash.log
 *        同时写入/proc/self/maps内存映射信息便于调试
 * @param sig 信号编号
 * @param si 信号信息结构体
 * @param ctx 上下文（ucontext_t），包含寄存器快照
 */
static void crash_handler(int sig, siginfo_t *si, void *ctx)
{
    char buf[512];
    unsigned long fault_addr = (unsigned long)si->si_addr;
    unsigned long pc = 0, lr = 0, sp = 0;

    if (ctx)
    {
        ucontext_t *uctx = (ucontext_t *)ctx;
        pc = (unsigned long)uctx->uc_mcontext.arm_pc;
        lr = (unsigned long)uctx->uc_mcontext.arm_lr;
        sp = (unsigned long)uctx->uc_mcontext.arm_sp;
    }

    int len = snprintf(buf, sizeof(buf),
                       "[XIAOZHI] SIG=%d fault=0x%08lx PC=0x%08lx LR=0x%08lx SP=0x%08lx\n",
                       sig, fault_addr, pc, lr, sp);
    if (len > 0)
        write(STDERR_FILENO, buf, len);

    /* 写入主日志文件 */
    int plog_fd = open("/var/log/xiaozhi.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (plog_fd >= 0)
    {
        write(plog_fd, buf, len > 0 ? len : 0);
        close(plog_fd);
    }

    /* 写入崩溃日志文件，附带内存映射信息 */
    int fd2 = open("/var/upgrade/crash.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd2 >= 0)
    {
        write(fd2, buf, len > 0 ? len : 0);
        int mfd = open("/proc/self/maps", O_RDONLY);
        if (mfd >= 0)
        {
            char mbuf[1024];
            int n;
            while ((n = read(mfd, mbuf, sizeof(mbuf))) > 0)
            {
                write(fd2, mbuf, n);
            }
            close(mfd);
        }
        close(fd2);
    }

    _exit(128 + sig);
}

/**
 * @brief 注册所有信号处理器
 *        - 崩溃信号（SIGSEGV/SIGABRT等）：记录崩溃信息
 *        - 终止信号（SIGTERM/SIGINT）：优雅退出
 *        - SIGPIPE：忽略
 *        - SIGUSR1：定时器驱动（不设SA_RESTART以中断poll）
 * @return 0成功
 */
static int setup_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);

    /* 注册崩溃信号处理器 */
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);

    /* 注册终止和热更新信号处理器 */
    sa.sa_handler = signal_handler;
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGPIPE, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);

    /* 解除SIGUSR2的屏蔽，确保热更新信号可达 */
    {
        sigset_t unblock_set;
        sigemptyset(&unblock_set);
        sigaddset(&unblock_set, SIGUSR2);
        sigprocmask(SIG_UNBLOCK, &unblock_set, NULL);
    }

    /* 注册SIGUSR1处理，不设SA_RESTART以中断poll等待 */
    sigaction(SIGUSR1, NULL, &g_old_sigusr1_sa);
    sa.sa_handler = sigusr1_handler;
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, NULL);
    PLOG_I("INIT", "SIGUSR1 处理器已安装 (无SA_RESTART), 旧处理器=%p",
           (void *)g_old_sigusr1_sa.sa_handler);

    return 0;
}

static void re_register_signals(void)
{
    struct sigaction sa_cur;
    sigaction(SIGUSR1, NULL, &sa_cur);
    PLOG_I("INIT", "applib_init 后 SIGUSR1: handler=%p flags=0x%x",
           (void *)sa_cur.sa_handler, sa_cur.sa_flags);

    if (sa_cur.sa_flags & SA_RESTART)
    {
        PLOG_W("INIT", "applib 设置了 SA_RESTART! 重新注册不带 SA_RESTART");
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = sigusr1_handler;
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, &g_old_sigusr1_sa);
    PLOG_I("INIT", "SIGUSR1 已重新注册 (无SA_RESTART), 旧处理器=%p flags=0x%x",
           (void *)g_old_sigusr1_sa.sa_handler, g_old_sigusr1_sa.sa_flags);

    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);

    sa.sa_handler = signal_handler;
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGPIPE, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);

    {
        sigset_t unblock_set;
        sigemptyset(&unblock_set);
        sigaddset(&unblock_set, SIGUSR2);
        sigprocmask(SIG_UNBLOCK, &unblock_set, NULL);
    }

    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);

    PLOG_I("INIT", "applib_init 后重新注册信号处理器");
}

static int set_sched_priority(void)
{
    struct sched_param param;
    param.sched_priority = 8;
    if (sched_setscheduler(0, SCHED_RR, &param) < 0)
    {
        PLOG_W("INIT", "设置调度策略失败: %d", errno);
        return -1;
    }
    PLOG_I("INIT", "已设置 SCHED_RR 优先级 %d", param.sched_priority);
    return 0;
}

/**
 * @brief applib初始化前的预处理
 *        - 检测并恢复上次失败的热更新（sair_new回滚）
 *        - 检查app_running_list共享内存判断是否为热更新启动
 *        - 热更新时清理残留的共享内存和服务socket
 *        - 冷启动时注册应用到app_running_list
 * @return 1=热更新启动，0=正常冷启动
 */
static int pre_applib_init(void)
{
    int is_hot_update = 0;
    int sair_entry_exists = 0;

    /* sair 是 Assistant 的二进制文件名（平台约束不可改名） */
    unlink("/var/upgrade/sair_old");

    /* 检查是否有上次失败热更新残留的sair_new，如有则恢复 */
    struct stat st_new;
    if (stat("/var/upgrade/sair_new", &st_new) == 0)
    {
        unlink("/var/upgrade/sair_old");
        rename("/var/upgrade/sair", "/var/upgrade/sair_old");
        rename("/var/upgrade/sair_new", "/var/upgrade/sair");
        PLOG_I("BOOT", "已恢复上次失败热更新的 sair_new");
    }

    /* 读取app_running_list共享内存，判断当前进程是否已注册（热更新场景） */
    int arl_fd = shm_open("app_running_list", O_RDWR, 0);
    if (arl_fd >= 0)
    {
        void *arl_ptr = mmap(NULL, APP_RUNNING_LIST_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, arl_fd, 0);
        if (arl_ptr != MAP_FAILED)
        {
            char *data = (char *)arl_ptr;
            int my_pid = getpid();

            /* 遍历共享内存中的条目，查找sair（助手二进制文件名） */
            for (int base = 0x20; base + 0x188 <= APP_RUNNING_LIST_SIZE; base += 0x188)
            {
                char *entry = data + base;
                int entry_pid = *(int *)(entry + 0x08);
                char *name_ptr = entry + 0x0c;

                if (name_ptr[0] != '\0' && strstr(name_ptr, "sair") != NULL)
                {
                    sair_entry_exists = 1;
                    if (entry_pid == my_pid)
                    {
                        is_hot_update = 1;
                    }
                }
            }

            if (is_hot_update)
            {
                PLOG_I("BOOT", "检测到热更新 (pid=%d 在 app_running_list 中)", my_pid);
                /* 清理热更新残留的共享内存和socket */
                char shm_name[64];
                snprintf(shm_name, sizeof(shm_name), "/sair_%d_sync_shm", my_pid);
                shm_unlink(shm_name);
                unlink("/tmp/service/sair");

                PLOG_I("BOOT", "热更新: 已清理 sync_shm 和服务 socket");

                /* 清理/dev/shm下属于当前进程的sair共享内存对象 */
                DIR *shm_dir = opendir("/dev/shm");
                if (shm_dir)
                {
                    struct dirent *ent;
                    char pid_prefix[32];
                    snprintf(pid_prefix, sizeof(pid_prefix), "sair_%d_", my_pid);
                    int cleaned = 0;
                    while ((ent = readdir(shm_dir)) != NULL)
                    {
                        if (strstr(ent->d_name, pid_prefix) == ent->d_name)
                        {
                            shm_unlink(ent->d_name);
                            PLOG_I("BOOT", "热更新: 已清理 /dev/shm/%s", ent->d_name);
                            cleaned++;
                        }
                    }
                    closedir(shm_dir);
                    if (cleaned == 0)
                    {
                        PLOG_I("BOOT", "热更新: 无需清理 sair 共享内存对象");
                    }
                }
            }
            else
            {
                PLOG_I("BOOT", "正常冷启动");
            }

            munmap(arl_ptr, APP_RUNNING_LIST_SIZE);
        }
        close(arl_fd);
    }

    /* 冷启动时需要注册应用到app_running_list */
    if (!sair_entry_exists)
    {
        extern int applib_register_app(const char *appname);
        int reg_ret = applib_register_app("sair"); /* sair 是助手二进制文件名 */
        PLOG_I("BOOT", "applib_register_app 返回 %d", reg_ret);
    }
    else
    {
        PLOG_I("BOOT", "sair 已在 app_running_list 中, 跳过注册");
    }

    return is_hot_update;
}

/**
 * @brief 广播助手唤醒消息，通知系统其他模块助手已激活
 * @param wakeup_result 唤醒结果，0=普通唤醒，非0=命令式唤醒
 */
static void broadcast_sair_awake(int wakeup_result)
{
    int msg[4] = {0};
    if (wakeup_result == 0)
    {
        msg[0] = MSG_SAIR_AWAKE; /* MSG_SAIR_AWAKE: 平台SDK定义的IPC消息ID - 助手唤醒通知 */
    }
    else
    {
        msg[0] = MSG_SAIR_AWAKE_CMD; /* MSG_SAIR_AWAKE_CMD: 平台SDK定义的IPC消息ID - 命令式唤醒通知 */
        msg[1] = wakeup_result + 256;
    }
    /* 唤醒广播专用时间戳: AI_STOP 回环窗口判定用(勿复用 g_last_bcast_ms——
     * 字幕 0x23A 推送也刷新它, 对话中字幕高频会把真 smart_player 抢占误杀) */
    g_last_awake_bcast_ms = get_time_ms();
    int ret = sair_broadcast(msg);
    PLOG_I("IPC", "广播 MSG_SAIR_AWAKE msg[0]=0x%x msg[1]=%d ret=%d", msg[0], msg[1], ret);
}

/**
 * @brief 广播助手会话结束消息
 */
static void broadcast_sair_end(void)
{
    int msg[4] = {MSG_SAIR_END, 0, 0, 0}; /* MSG_SAIR_END: 平台SDK定义的IPC消息ID - 助手会话结束 */
    int ret = sair_broadcast(msg);
    PLOG_I("IPC", "广播 MSG_SAIR_END (0x%X) ret=%d", MSG_SAIR_END, ret);
}

/**
 * @brief 唤醒事件回调
 *        检查启动保护期、冷却期、清理后冷却期，通过后设置挂起唤醒标志
 *        并通过self-pipe和SIGUSR1通知主循环
 * @param wakeup_type 唤醒类型
 * @param user_data 用户数据，传入app_context_t指针
 */
static void on_wakeup_event(int wakeup_type, void *user_data)
{
    app_context_t *app = (app_context_t *)user_data;
    uint64_t now = get_time_ms();

    if (!app->wakeup.started)
    {
        PLOG_D("WAKEUP", "唤醒检测未启动, 忽略唤醒");
        return;
    }

    /* 唤醒冷却期内忽略，防止连续误唤醒 */
    if (now - app->last_wakeup_ms < g_app.wakeup_cooldown_ms)
    {
        PLOG_D("WAKEUP", "唤醒冷却中, 忽略 (已过=%llums)", (unsigned long)(now - app->last_wakeup_ms));
        return;
    }

    /* 清理结束后冷却期内忽略，避免刚结束就重新唤醒 */
    if (app->last_cleaning_end_time_ms > 0 && now - app->last_cleaning_end_time_ms < POST_CLEANUP_COOLDOWN_MS)
    {
        PLOG_D("WAKEUP", "清理后冷却中, 忽略 (已过=%llums)",
               (unsigned long long)(now - app->last_cleaning_end_time_ms));
        return;
    }

    PLOG_I("WAKEUP", "检测到唤醒! type=%d", wakeup_type);
    app->pending_wakeup = 1;
    app->pending_wakeup_type = wakeup_type;

    /* 通过self-pipe通知主循环 */
    if (app->self_pipe[1] >= 0)
    {
        char c = 'W';
        write(app->self_pipe[1], &c, 1);
    }
    kill(getpid(), SIGUSR1);
}

/**
 * @brief 按键事件回调
 *        GOODIX_KEY_BACK/其他键: 设置退出标志
 *        GOODIX_KEY_HOME: 设置Home键标志
 * @param key_code 按键编码
 * @param user_data 用户数据，传入app_context_t指针
 */
static void on_key_event(int key_code, void *user_data)
{
    app_context_t *app = (app_context_t *)user_data;
    PLOG_I("KEY", "按键事件: code=%d", key_code);
    if (key_code == GOODIX_KEY_BACK)
    {
        app->pending_key_exit = 1;
    }
    else if (key_code == GOODIX_KEY_HOME)
    {
        app->pending_key_home = 1;
    }
    else
    {
        app->pending_key_exit = 1;
    }
    kill(getpid(), SIGUSR1);
}

/**
 * @brief 录音线程函数
 *        打开音频录音器（16kHz, 2麦克风+1参考信号），持续读取PCM数据
 *        并通过audio_dispatcher分发到唤醒模块和录音发送模块
 * @param arg app_context_t指针
 * @return NULL
 */
static void *recorder_thread_func(void *arg)
{
    app_context_t *app = (app_context_t *)arg;

    prctl(PR_SET_NAME, "audio_rec");

    /* 配置录音参数：PCM单声道格式，3通道（2mic+1ref），16kHz采样率 */
    audio_recorder_config_t rec_config;
    memset(&rec_config, 0, sizeof(rec_config));
    rec_config.format = RECORDER_FORMAT_PCM_MONO;
    rec_config.type = RECORDER_TYPE_CAPTURE_5;
    rec_config.channels = 3;
    rec_config.mic_num = 2;
    rec_config.ref_num = 1;
    rec_config.sample_rate = 16000;

    void *handle = audio_recorder_open(&rec_config);

    if (!handle)
    {
        PLOG_E("REC", "audio_recorder_open 失败");
        app->recorder_running = 0;
        return NULL;
    }

    if (audio_recorder_start(handle) != 0)
    {
        PLOG_E("REC", "audio_recorder_start 失败");
        audio_recorder_close(handle);
        app->recorder_running = 0;
        return NULL;
    }

    app->recorder_handle = handle;
    PLOG_I("REC", "录音器已启动 (16kHz, 2mic+1ref)");

    char buf[6144];

    /* Q7 诊断: 采集层统计(每5秒汇总, 定位后移除) */
    long r_batches = 0, r_bytes = 0, r_zero = 0;
    time_t r_t0 = time(NULL);

    /* 持续读取录音数据并分发 */
    while (app->recorder_running && g_running)
    {
        int n = audio_recorder_read(handle, buf, sizeof(buf));
        if (n <= 0)
        {
            r_zero++;
            usleep(10000);
        }
        else
        {
            r_batches++;
            r_bytes += n;
            audio_dispatcher_dispatch(&app->audio_disp, (const int16_t *)buf, n / 2);
        }
        time_t r_now = time(NULL);
        if (r_now - r_t0 >= 5)
        {
            PLOG_I("REC", "[诊断] read: %ld批/%ldB/零返回%ld次",
                   r_batches, r_bytes, r_zero);
            r_batches = r_bytes = r_zero = 0;
            r_t0 = r_now;
        }
    }

    audio_recorder_stop(handle);
    audio_recorder_close(handle);
    app->recorder_handle = NULL;

    PLOG_I("REC", "录音器已停止");
    return NULL;
}

/**
 * @brief 协议音频数据回调
 *        收到服务端TTS音频数据时调用，写入音频播放器
 *        首次收到音频时从Listening/Connecting状态转换到Speaking状态
 * @param packet 音频数据包
 * @param user_data 用户数据，传入app_context_t指针
 */
static int tts_audio_count = 0;

static void on_proto_audio(audio_packet_t *packet, void *user_data)
{
    app_context_t *app = (app_context_t *)user_data;
    if (!app || !packet)
        return;

    tts_audio_count++;

    if (app->ignore_tts_audio)
    {
        PLOG_D("PROTO", "忽略迟到的TTS音频 (中止后), ts=%u size=%d",
               packet->timestamp, packet->payload_size);
        return;
    }

    xiaozhi_state_t state = state_machine_get_state(&app->sm);

    app->last_tts_audio_ms = get_time_ms();

    if (state == kStateListening || state == kStateConnecting)
    {
        PLOG_I("PROTO", "收到首包音频(#%d), payload=%d字节, 转换到 Speaking 状态",
               tts_audio_count, packet->payload_size);
        app->player.aborted = false;
        audio_player_reset_decoder(&app->player);
        state_machine_transition(&app->sm, kStateSpeaking);
        audio_player_start(&app->player);
    }
    else if (state == kStateSpeaking && !app->player.playing)
    {
        PLOG_I("PROTO", "收到音频(#%d)但播放器未启动, 重新启动", tts_audio_count);
        app->player.aborted = false;
        audio_player_start(&app->player);
    }

    state = state_machine_get_state(&app->sm);
    if (state == kStateSpeaking)
    {
        int ret = audio_player_write_opus(&app->player, packet->payload, packet->payload_size, packet->timestamp);
        if (ret != 0 && tts_audio_count <= 10)
        {
            PLOG_W("PROTO", "audio_player_write_opus 失败: ret=%d (#%d, size=%d)",
                   ret, tts_audio_count, packet->payload_size);
        }
    }
}

/**
 * @brief 协议JSON消息回调
 *        处理服务端发来的各类JSON消息：hello/tts/stt/llm/goodbye/mcp/iot
 * @param json JSON数据指针
 * @param len JSON数据长度
 * @param user_data 用户数据，传入app_context_t指针
 */
static void on_proto_json(const char *json, size_t len, void *user_data)
{
    app_context_t *app = (app_context_t *)user_data;
    if (!app || !json)
        return;

    char type_str[64] = {0};
    proto_find_json_str(json, len, "type", type_str, sizeof(type_str));

    if (strcmp(type_str, "hello") == 0)
    {
        tts_audio_count = 0;
        PLOG_I("PROTO", "收到服务端hello, session=%s sr=%d fd=%d (唤醒后+%llums)",
               app->proto.session_id, app->proto.server_sample_rate,
               app->proto.server_frame_duration,
               (unsigned long long)(get_time_ms() - app->session_start_ms));

        if (app->player_initialized)
        {
            if (app->player.sample_rate != app->proto.server_sample_rate)
            {
                PLOG_I("PROTO", "采样率变化: %d -> %d, 重建解码器",
                       app->player.sample_rate, app->proto.server_sample_rate);
                audio_player_set_sample_rate(&app->player, app->proto.server_sample_rate);
            }
            app->player.ts_queue = &app->proto.ts_queue;
        }
        else
        {
            int player_ret = audio_player_init(&app->player, app->proto.server_sample_rate, 1);
            if (player_ret == 0)
            {
                app->player_initialized = 1;
                app->player.ts_queue = &app->proto.ts_queue;
                audio_player_set_volume(&app->player, 60);
            }
            else
            {
                PLOG_E("PROTO", "audio_player_init 失败: %d", player_ret);
            }
        }

        if (!app->recorder_initialized)
        {
            int rec_ret = audio_recorder_module_init(&app->recorder_mod, &app->proto, &app->audio_disp);
            if (rec_ret == 0)
            {
                app->recorder_initialized = 1;
            }
            else
            {
                PLOG_E("PROTO", "audio_recorder_module_init 失败: %d", rec_ret);
            }
        }
        else
        {
            audio_recorder_module_set_proto(&app->recorder_mod, &app->proto);
        }

        state_machine_transition(&app->sm, kStateListening);
    }
    else if (strcmp(type_str, "tts") == 0)
    {
        char state_str[32] = {0};
        proto_find_json_str(json, len, "state", state_str, sizeof(state_str));

        if (strcmp(state_str, "start") == 0)
        {
            PLOG_I("PROTO", "TTS 开始");
            app->ignore_tts_audio = 0;
            /* 新 TTS 段开始: 取消挂起的自动清屏(连续对话场景) */
            pthread_mutex_lock(&g_subtitle_mutex);
            g_subtitle_hide_ms = 0;
            pthread_mutex_unlock(&g_subtitle_mutex);
            xiaozhi_state_t cur = state_machine_get_state(&app->sm);
            if (cur == kStateCleaning)
            {
                PLOG_D("PROTO", "Cleaning 状态下忽略 TTS start");
            }
            else
            {
                app->player.aborted = false;
                audio_player_reset_decoder(&app->player);
                if (cur != kStateSpeaking)
                {
                    state_machine_transition(&app->sm, kStateSpeaking);
                }
                audio_player_start(&app->player);
            }
        }
        else if (strcmp(state_str, "stop") == 0)
        {
            if (app->ignore_tts_audio)
            {
                PLOG_D("PROTO", "忽略迟到的TTS stop (中止后)");
            }
            else
            {
                PLOG_I("PROTO", "TTS 结束 (正常完成)");
                bool wait_play = (app->listening_mode != LISTENING_MODE_REALTIME);
                audio_player_stop_with_wait(&app->player, wait_play);
                /* 打字机兜底完成(末句音频播完后整句必然显示完), 并安排延迟清屏 */
                subtitle_finish_typing();
                subtitle_schedule_hide(SUBTITLE_HIDE_MS);
                xiaozhi_state_t cur_state = state_machine_get_state(&app->sm);
                if (cur_state != kStateCleaning)
                {
                    state_machine_transition(&app->sm, kStateListening);
                }
            }
        }
        else if (strcmp(state_str, "sentence_start") == 0)
        {
            char tts_text[SUBTITLE_MAX_LEN] = {0};
            proto_find_json_str(json, len, "text", tts_text, sizeof(tts_text));
            PLOG_I("PROTO", "TTS sentence_start: text='%.64s'", tts_text);
            if (tts_text[0])
            {
                subtitle_set(SUBTITLE_TYPE_TTS, tts_text);
            }
        }
    }
    else if (strcmp(type_str, "stt") == 0)
    {
        char state_str[32] = {0};
        proto_find_json_str(json, len, "state", state_str, sizeof(state_str));
        char stt_text[SUBTITLE_MAX_LEN] = {0};
        proto_find_json_str(json, len, "text", stt_text, sizeof(stt_text));
        PLOG_I("PROTO", "STT %s text='%.64s'", state_str, stt_text);
        if (stt_text[0])
        {
            subtitle_set(SUBTITLE_TYPE_ASR, stt_text);
        }
    }
    else if (strcmp(type_str, "llm") == 0)
    {
        char emotion_str[64] = {0};
        proto_find_json_str(json, len, "emotion", emotion_str, sizeof(emotion_str));
        if (emotion_str[0])
        {
            PLOG_I("PROTO", "表情: %s", emotion_str);
            static const struct
            {
                const char *name;
                int id;
            } emotion_map[] = {
                {"neutral", 0},
                {"happy", 1},
                {"laughing", 1},
                {"funny", 1},
                {"sad", 2},
                {"crying", 3},
                {"angry", 4},
                {"surprised", 5},
                {"shocked", 6},
                {"embarrassed", 7},
                {"loving", 8},
                {"kissy", 9},
                {"confident", 10},
                {"winking", 10},
                {"sleepy", 11},
                {"silly", 1},
                {"confused", 12},
                {"relaxed", 13},
                {"cool", 14},
                {"thinking", 0},
                {"delicious", 1},
                {"fearful", 6},
            };
            int emotion_id = -1;
            for (int i = 0; i < (int)(sizeof(emotion_map) / sizeof(emotion_map[0])); i++)
            {
                if (strcmp(emotion_str, emotion_map[i].name) == 0)
                {
                    emotion_id = emotion_map[i].id;
                    break;
                }
            }
            if (emotion_id < 0)
            {
                emotion_id = 0;
                PLOG_D("PROTO", "未知表情 '%s', 使用默认值", emotion_str);
            }
            int msg[4] = {MSG_SAIR_EMOTION, emotion_id, 0, 0};
            int ret = sair_broadcast(msg);
            PLOG_I("IPC", "广播 MSG_SAIR_EMOTION id=%d ret=%d", emotion_id, ret);
        }
    }
    else if (strcmp(type_str, "goodbye") == 0)
    {
        PLOG_I("PROTO", "收到 goodbye");
        state_machine_transition(&app->sm, kStateCleaning);
    }
    else if (strcmp(type_str, "listen") == 0)
    {
        char state_str[32] = {0};
        proto_find_json_str(json, len, "state", state_str, sizeof(state_str));
        PLOG_I("PROTO", "收到服务端listen: state=%s", state_str);

        if (strcmp(state_str, "start") == 0)
        {
            xiaozhi_state_t cur = state_machine_get_state(&app->sm);
            if (cur == kStateSpeaking)
            {
                PLOG_I("PROTO", "Speaking状态收到服务端listen start, 中止TTS播放");
                app->ignore_tts_audio = 1;
                app->player.aborted = true;
                audio_player_stop(&app->player);
                /* 用户抢话: 旧回复字幕立即清屏, 打字机作废 */
                subtitle_interrupt();
                if (protocol_handler_is_connected(&app->proto))
                {
                    protocol_handler_send_abort(&app->proto, "server_listening");
                    if (app->listening_mode != LISTENING_MODE_REALTIME)
                    {
                        protocol_handler_clear_send_queue(&app->proto);
                    }
                }
                state_machine_transition(&app->sm, kStateListening);
            }
        }
    }
    else if (strcmp(type_str, "mcp") == 0 || strcmp(type_str, "iot") == 0)
    {
        if (app->mcp_initialized)
        {
            mcp_handler_process_message(&app->mcp, json, len);
        }
        else
        {
            PLOG_W("PROTO", "收到 %s 但 MCP 未初始化", type_str);
        }
    }
}

/**
 * @brief 协议断开连接回调
 *        在会话中意外断开时转换到Cleaning状态清理资源
 * @param user_data 用户数据，传入app_context_t指针
 */
static void on_proto_disconnected(void *user_data)
{
    app_context_t *app = (app_context_t *)user_data;
    if (!app)
        return;

    PLOG_W("PROTO", "连接断开");

    app->ignore_tts_audio = 0;

    xiaozhi_state_t state = state_machine_get_state(&app->sm);

    /* 在活跃会话状态下断开，结束当前会话 */
    if (state == kStateListening || state == kStateSpeaking)
    {
        PLOG_I("PROTO", "云端在 %s 状态断开, 结束会话",
               state_machine_get_state_name(state));
        state_machine_transition(&app->sm, kStateCleaning);
        return;
    }

    /* 其他非空闲/非初始状态下也进入清理 */
    if (state != kStateIdle && state != kStateActivating && state != kStateStarting && state != kStateCleaning)
    {
        state_machine_transition(&app->sm, kStateCleaning);
    }
}

/**
 * @brief 协议错误回调
 * @param message 错误信息
 * @param user_data 用户数据
 */
static void on_proto_error(const char *message, void *user_data)
{
    PLOG_E("PROTO", "错误: %s", message ? message : "未知");
}

/**
 * @brief 消息接收线程函数
 *        从IPC消息队列中读取系统消息，通过self-pipe通知主循环处理
 * @param arg app_context_t指针
 * @return NULL
 */
static void *msg_thread_func(void *arg)
{
    app_context_t *app = (app_context_t *)arg;
    prctl(PR_SET_NAME, "msg_recv");

    /* 屏蔽SIGUSR2和SIGUSR1，避免在此线程中处理信号 */
    {
        sigset_t block_set;
        sigemptyset(&block_set);
        sigaddset(&block_set, SIGUSR2);
        sigaddset(&block_set, SIGUSR1);
        pthread_sigmask(SIG_BLOCK, &block_set, NULL);
    }

    char buf[MSG_SIZE];

    PLOG_I("MSG", "消息接收线程已启动");

    while (app->msg_thread_running && g_running)
    {
        int ret = get_msg(buf);
        if (ret > 0)
        {
            /* 将消息复制到共享缓冲区，通知主循环处理 */
            pthread_mutex_lock(&app->msg_mutex);
            memcpy(app->msg_buf, buf, MSG_SIZE);
            app->msg_available = 1;
            pthread_mutex_unlock(&app->msg_mutex);

            if (app->self_pipe[1] >= 0)
            {
                char c = 'M';
                write(app->self_pipe[1], &c, 1);
            }
        }
        else if (ret < 0)
        {
            if (!g_running)
                break;
            usleep(10000);
        }
    }

    PLOG_I("MSG", "消息接收线程退出");
    return NULL;
}

/**
 * @brief OTA线程函数
 *        等待WiFi连接后检查设备激活状态，获取WebSocket配置
 *        成功获取配置后通过self-pipe通知主循环
 * @param arg app_context_t指针
 * @return NULL
 */
static void *ota_thread_func(void *arg)
{
    app_context_t *app = (app_context_t *)arg;
    prctl(PR_SET_NAME, "ota_check");

    PLOG_I("OTA", "OTA线程启动, 等待WiFi连接");

    /* 延迟200ms避免与录音线程的audio_service初始化竞争
     * 热更新场景下两个线程几乎同时启动，audio_recorder_open会做大量共享内存操作
     * 过早启动WiFi检查可能导致dlopen("libwifi_client.so")在低内存环境下竞争锁 */
    usleep(200000);

    /* 等待WiFi连接，最多60秒 */
    int wifi_wait = 0;
    while (wifi_wait < 60 && g_running)
    {
        if (config_manager_check_wifi())
            break;
        sleep(1);
        wifi_wait++;
    }

    if (!config_manager_check_wifi())
    {
        PLOG_W("OTA", "WiFi 60秒后仍不可用");
        app->ota_done = 1;
        return NULL;
    }

    PLOG_I("OTA", "WiFi已连接, 检查设备激活状态");

    /* 获取设备MAC地址 */
    if (strlen(app->config.device_mac) == 0 ||
        strcmp(app->config.device_mac, "00:00:00:00:00:00") == 0)
    {
        config_manager_get_mac(app->config.device_mac, sizeof(app->config.device_mac));
        strncpy(app->device_mac, app->config.device_mac, sizeof(app->device_mac) - 1);
    }

    /* 重试3次获取OTA配置 */
    int retries = 0;
    while (retries < 3 && g_running)
    {
        config_manager_check_activation(&app->config);
        if (app->config.has_ws_config)
        {
            app->ota_config_received = 1;
            break;
        }
        retries++;
        sleep(10);
    }

    /* 2026-08-30: MQTT+UDP 传输路线已移除, OTA 下发的 mqtt 段在 config_manager
     * 层直接忽略, 不再自适应切换传输模式(原切换会把设备带入 MCP/字幕/打断
     * 全不可用的半残形态) */

    if (app->ota_config_received)
    {
        PLOG_I("OTA", "已获取配置: ws_url=%s",
               app->config.ws_url);
    }

    app->ota_done = 1;
    PLOG_I("OTA", "OTA线程完成");

    /* 通知主循环OTA已完成 */
    if (app->self_pipe[1] >= 0)
    {
        char c = 'O';
        write(app->self_pipe[1], &c, 1);
    }
    kill(getpid(), SIGUSR1);

    return NULL;
}

/**
 * @brief WebSocket连接线程函数
 *        初始化协议处理器、设置回调、建立WebSocket连接
 *        连接成功后转换到Connecting状态等待hello消息
 * @param arg app_context_t指针
 * @return NULL
 */
static void *connect_thread_func(void *arg)
{
    app_context_t *app = (app_context_t *)arg;
    prctl(PR_SET_NAME, "ws_connect");

    /* 屏蔽信号，避免在此线程中处理 */
    {
        sigset_t block_set;
        sigemptyset(&block_set);
        sigaddset(&block_set, SIGUSR2);
        sigaddset(&block_set, SIGUSR1);
        pthread_sigmask(SIG_BLOCK, &block_set, NULL);
    }

    PLOG_I("CONN", "连接线程启动 (唤醒后+%llums)",
           (unsigned long long)(get_time_ms() - app->session_start_ms));

    /* 配置协议参数 */
    protocol_config_t proto_config;
    memset(&proto_config, 0, sizeof(proto_config));

    /* 2026-08-30: MQTT+UDP 路线已移除, 恒走 WebSocket */
    PLOG_I("CONN", "传输模式: WebSocket (配置: ws=%s)",
           app->config.has_ws_config ? "有" : "无");

    if (app->custom_ws_url[0])
        strncpy(proto_config.url, app->custom_ws_url, sizeof(proto_config.url) - 1);
    else
        strncpy(proto_config.url, app->config.ws_url, sizeof(proto_config.url) - 1);
    if (app->config.ws_token[0])
    {
        snprintf(proto_config.token, sizeof(proto_config.token), "Bearer %.495s", app->config.ws_token);
    }
    strncpy(proto_config.device_id, app->device_mac, sizeof(proto_config.device_id) - 1);
    strncpy(proto_config.client_id, app->client_id, sizeof(proto_config.client_id) - 1);
    proto_config.sample_rate = 16000;
    proto_config.channels = 1;
    proto_config.frame_duration = 60;
    proto_config.ping_interval_ms = app->ws_ping_interval_ms;

    int init_ret = protocol_handler_init(&app->proto, &proto_config);
    if (init_ret != 0)
    {
        PLOG_E("CONN", "protocol_handler_init 失败: %d", init_ret);
        app->proto_initialized = 0;
        app->connecting = 0;
        state_machine_transition(&app->sm, kStateCleaning);
        return NULL;
    }

    PLOG_I("CONN", "protocol_version: %d, channels: %d, listening_mode: %s",
           app->proto.protocol_version, proto_config.channels,
           app->listening_mode == LISTENING_MODE_REALTIME ? "realtime" : "autostop");
    app->proto_initialized = 1;
    app->proto.cloud_aec = app->aec_mode; /* AEC方案传入, hello 按此声明 */

    protocol_handler_set_callbacks(&app->proto, NULL, on_proto_disconnected,
                                   on_proto_audio, on_proto_json, on_proto_error, app);

    PLOG_I("CONN", "正在连接 %s", proto_config.url);

    int connect_ret = protocol_handler_connect(&app->proto);
    /* 检查连接是否在连接过程中被取消 */
    if (!app->connecting)
    {
        PLOG_W("CONN", "连接在建立过程中被取消");
        protocol_handler_disconnect(&app->proto);
        protocol_handler_destroy(&app->proto);
        app->proto_initialized = 0;
        return NULL;
    }
    /* 首次连接失败则重试一次 */
    if (connect_ret != 0)
    {
        PLOG_W("CONN", "首次连接失败, 1秒后重试...");
        for (int wait = 0; wait < 10 && app->connecting && g_running; wait++)
        {
            usleep(100000);
        }
        if (app->connecting && g_running)
        {
            connect_ret = protocol_handler_connect(&app->proto);
        }
    }
    if (connect_ret != 0)
    {
        PLOG_E("CONN", "重试后连接仍失败");
        app->connecting = 0;
        state_machine_transition(&app->sm, kStateCleaning);
        return NULL;
    }

    PLOG_I("CONN", "已连接到 %s, 等待主循环中接收hello (唤醒后+%llums)",
           proto_config.url, (unsigned long long)(get_time_ms() - app->session_start_ms));

    app->connecting = 0;
    state_machine_transition(&app->sm, kStateConnecting);

    PLOG_I("CONN", "连接线程完成");
    return NULL;
}

/**
 * @brief 处理系统IPC消息
 *        MSG_APP_QUIT: 退出应用
 *        MSG_SAIR_ENABLE/MSG_SAIR_DISABLE: 启用/禁用唤醒词检测
 *        （MSG_SAIR_* 系列为平台SDK定义的IPC消息ID）
 * @param msg_ptr 消息指针
 */
static void proc_sys_msg(void *msg_ptr)
{
    int *msg = (int *)msg_ptr;
    if (!msg)
        return;

    int msg_type = msg[0];
    PLOG_I("IPC", "系统消息: type=0x%X", msg_type);

    switch (msg_type)
    {
    case MSG_APP_QUIT:
        PLOG_I("IPC", "收到 MSG_APP_QUIT");
        g_running = 0;
        exit_msg_loop();
        break;
    case MSG_SAIR_ENABLE: /* MSG_SAIR_ENABLE: 平台SDK IPC消息ID - 启用助手 */
        PLOG_I("IPC", "MSG_SAIR_ENABLE");
        wakeup_resume_feed(&g_app.wakeup);
        break;
    case MSG_SAIR_DISABLE:
        PLOG_I("IPC", "MSG_SAIR_DISABLE");
        wakeup_pause_feed(&g_app.wakeup);
        /* R-12: 禁用语义 = 静默 -- 丢弃过期唤醒事件; 进行中的会话由 STOP/QUIT 或自然路径收束 */
        g_app.pending_wakeup = 0;
        g_app.pending_wakeup_type = 0;
        display_ctrl_note_activity();
        break;
    case 0x39:
    {
        uint64_t now = get_time_ms();
        if (now - g_app.last_button_wakeup_ms < 2000)
        {
            PLOG_D("IPC", "唤醒按钮 0x39: 冷却中, 忽略");
        }
        else
        {
            PLOG_I("IPC", "唤醒按钮 0x39: 触发唤醒");
            g_app.last_button_wakeup_ms = now;
            g_app.pending_wakeup = 1;
            g_app.pending_wakeup_type = 1;
            if (g_app.self_pipe[1] >= 0)
            {
                char c = 'W';
                write(g_app.self_pipe[1], &c, 1);
            }
            kill(getpid(), SIGUSR1);
        }
        break;
    }
    case 0x174: /* MSG_POWER_ON_PUSH: WiFi连上后每日动画推送广播(olmedia触发) */
        if (boot_push_disable_enabled())
        {
            PLOG_I("IPC", "0x174 每日推送广播(抑制中): 插件已被bind拦截, 背光自愈兜底中");
        }
        else
        {
            PLOG_I("IPC", "0x174 每日推送广播(未抑制): 放行原生播放");
        }
        break;
    default:
        PLOG_D("IPC", "系统消息: 未处理 type=0x%X", msg_type);
        break;
    }
}

/**
 * @brief 处理服务IPC消息
 *        对MSG_SAIR_*系列消息进行应答确认
 *        （MSG_SAIR_* 系列为平台SDK定义的IPC消息ID）
 * @param req_header_ptr 请求头指针
 * @param resp_result 响应结果输出指针
 */
static void proc_srv_msg(void *req_header_ptr, int *resp_result)
{
    srv_req_header_t *hdr = (srv_req_header_t *)req_header_ptr;
    if (!hdr)
        return;

    int cmd = hdr->cmd;
    PLOG_I("IPC", "服务消息: cmd=0x%X seq=%d payload_size=%d", cmd, hdr->seq, hdr->payload_size);

    if (resp_result)
    {
        *resp_result = 0;
    }

    switch (cmd)
    {
    case MSG_SAIR_OPEN: /* MSG_SAIR_OPEN: 平台SDK IPC消息ID - 打开助手 */
        PLOG_I("IPC", "MSG_SAIR_OPEN: 已确认");
        if (resp_result)
            *resp_result = 1;
        break;
    case MSG_SAIR_CLOSE: /* MSG_SAIR_CLOSE: 平台SDK IPC消息ID - 关闭助手 */
        PLOG_I("IPC", "MSG_SAIR_CLOSE: 已确认");
        if (resp_result)
            *resp_result = 1;
        break;
    case MSG_SAIR_AI_START:
    {
        uint64_t now = get_time_ms();
        /* 入口即消费挂起的外部停止: 原生序列 0x3EC(AI_STOP)→0x3EB(AI_START)
         * 是"重启会话"原子操作, AI_STOP 的挂起停止若不被 AI_START 清除,
         * 新会话 Listening 建立后即被主循环收敛块误杀(唤醒秒退, 22:32:39 实录).
         * 此清除必须在所有忽略分支之前执行. */
        g_app.pending_stop_request = 0;
        /* 会话进行中忽略(原生语义: 对话中按键无效果; 打断只走唤醒词引擎).
         * 字幕推送(0x23A)会触发 msg_server 1ms 内回发本命令, 若在
         * Connecting/Listening/Speaking 态处理将误打断 TTS 播放. */
        {
            xiaozhi_state_t _cur = state_machine_get_state(&g_app.sm);
            if (_cur == kStateConnecting || _cur == kStateListening || _cur == kStateSpeaking)
            {
                PLOG_I("IPC", "MSG_SAIR_AI_START: 会话进行中(%s), 忽略",
                       state_machine_get_state_name(_cur));
                if (resp_result)
                    *resp_result = 1;
                break;
            }
        }
        if (g_last_bcast_ms && now - g_last_bcast_ms < 300)
        {
            PLOG_I("IPC", "MSG_SAIR_AI_START: 广播后%llums回环, 忽略",
                   (unsigned long long)(now - g_last_bcast_ms));
            if (resp_result)
                *resp_result = 1;
            break;
        }
        if (now - g_app.last_button_wakeup_ms < 2000)
        {
            PLOG_I("IPC", "MSG_SAIR_AI_START: 唤醒按钮冷却中 (%llums < 2000ms), 忽略",
                   (unsigned long long)(now - g_app.last_button_wakeup_ms));
        }
        else
        {
            PLOG_I("IPC", "MSG_SAIR_AI_START: 唤醒按钮触发唤醒");
            g_app.last_button_wakeup_ms = now;
            g_app.pending_wakeup = 1;
            g_app.pending_wakeup_type = 1;
            /* msg_server 唤醒转发的原生序列是 0x3EC(AI_STOP) 先行抢占、0x3EB(AI_START)
             * 紧随(2026-08-27 实测日志). 若不清挂起停止, 新会话在 Listening 建立
             * 0.5s 内即被主循环收敛块掐死(唤醒回环自噬). 新唤醒=旧停止已失去意义. */
            g_app.pending_stop_request = 0;
            if (g_app.self_pipe[1] >= 0)
            {
                char c = 'W';
                write(g_app.self_pipe[1], &c, 1);
            }
            kill(getpid(), SIGUSR1);
        }
        if (resp_result)
            *resp_result = 1;
        break;
    }
    case MSG_SAIR_AI_STOP:      /* smart_player 播放态(status==4)抢占入口 -- M2 §3/R1 §17.6 */
    {
        /* 字幕清屏(0x231)触发的 sair_ai_stop 回环: 忽略, 保护 AutoStop 会话;
         * 真实 smart_player 抢占(窗外到达)正常处理 */
        uint64_t _now = get_time_ms();
        if (g_last_end_evt_ms && _now - g_last_end_evt_ms < 800)
        {
            PLOG_I("IPC", "MSG_SAIR_AI_STOP: 字幕清屏回环(%llums), 忽略",
                   (unsigned long long)(_now - g_last_end_evt_ms));
            if (resp_result)
                *resp_result = 1;
            break;
        }
        /* 唤醒广播(0x235)触发的 msg_server 契约性回环: msg_server 播唤醒动画
         * 前按契约回发 AI_STOP 停旧 AI. 打断场景(2026-08-30 实录)该回环在
         * 广播后 ~42ms 到达, 正好砸在刚同步建立的 Listening 上, 杀死打断后
         * 的新会话("唤醒打断变退出"). 正常唤醒不受影响(0x3EC 到达时还在
         * Connecting, 收敛块不管 Connecting, 后续 AI_START 清挂起).
         * 窗口 800ms: 真正的 smart_player 抢占在此窗内到达会被误忽略一次,
         * 但 smart_player 的仲裁抢占是持续的(每次 status 查询都会发), 下一条
         * (窗外)即生效; 且音乐被唤醒打断本就是预期行为. */
        if (g_last_awake_bcast_ms && _now - g_last_awake_bcast_ms < 800)
        {
            PLOG_I("IPC", "MSG_SAIR_AI_STOP: 唤醒广播回环(%llums), 忽略",
                   (unsigned long long)(_now - g_last_awake_bcast_ms));
            if (resp_result)
                *resp_result = 1;
            break;
        }
        g_app.pending_stop_request = 1;   /* 主循环统一收敛(复用 pending_api_abort 模式) */
        if (resp_result)
            *resp_result = 1;
        break;
    }
    case MSG_SAIR_STATUS_UPDATE:
    case MSG_SAIR_GET_INFO:     /* M2 §11/R1 §17: get_info 空闲全零即合法, 必须回满 2836B */
    {
        int zlen = hdr->payload_size > 2836 ? hdr->payload_size : 2836;
        memset(hdr->payload, 0, zlen);
        hdr->payload_size = 2836;
        if (resp_result)
            *resp_result = 1;
        break;
    }
    case MSG_SAIR_ASR_START:      /* 平台SDK IPC消息ID - 语音识别开始 */
    case MSG_SAIR_ASR_STOP:       /* 平台SDK IPC消息ID - 语音识别停止 */
    case MSG_SAIR_AEC_START:      /* 平台SDK IPC消息ID - 回声消除开始 */
    case MSG_SAIR_AEC_STOP:       /* 平台SDK IPC消息ID - 回声消除停止 */
    case MSG_SAIR_CHOOSE_AI:      /* 平台SDK IPC消息ID - 选择AI */
    case MSG_SAIR_REGISTER_CB:    /* 平台SDK IPC消息ID - 注册回调 */
    case MSG_SAIR_POST_EVENT:
    case MSG_SAIR_RECORD_REQUEST:
        if (resp_result)
            *resp_result = 1;
        break;
    case MSG_SAIR_GET_EVENT:
    {
        pthread_mutex_lock(&g_subtitle_mutex);
        if (g_subtitle.type != SUBTITLE_TYPE_NONE)
        {
            int content_len = strlen(g_subtitle.content) + 1;
            int total_size = (int)sizeof(int) + content_len;
            if (total_size <= 4096)
            {
                memcpy(hdr->payload, &g_subtitle.type, sizeof(int));
                memcpy(hdr->payload + sizeof(int), g_subtitle.content, content_len);
                hdr->payload_size = total_size;
            }
            PLOG_I("SUB", "GET_EVENT响应: type=%d content='%.64s'", g_subtitle.type, g_subtitle.content);
        }
        else
        {
            PLOG_D("SUB", "GET_EVENT响应: 无字幕内容");
        }
        pthread_mutex_unlock(&g_subtitle_mutex);
        if (resp_result)
            *resp_result = 1;
        break;
    }
    default:
        PLOG_D("IPC", "服务消息: 未处理 cmd=0x%X", cmd);
        break;
    }
}

/**
 * @brief 状态变更回调
 *        当状态机状态转换时被调用，负责各状态下的资源管理和操作
 *        - Starting: 设置活跃看门狗超时
 *        - Activating: 检查设备激活状态
 *        - Idle: 等待唤醒，清理上一次会话资源
 *        - Connecting: 建立WebSocket连接
 *        - Listening: 等待用户语音输入
 *        - Speaking: 播放TTS语音
 *        - Cleaning: 释放会话资源
 * @param from 源状态
 * @param to 目标状态
 * @param user_data 用户数据，传入app_context_t指针
 */
static void do_session_cleanup(app_context_t *app, uint64_t now, uint64_t prev_state_enter_time_ms)
{
    uint64_t cleanup_elapsed_ms = now - prev_state_enter_time_ms;
    PLOG_I("CLEANUP", "清理耗时 %llums", (unsigned long long)cleanup_elapsed_ms);

    app->last_cleaning_end_time_ms = now;
    app->in_session = 0;

    if (app->proto_initialized)
    {
        if (protocol_handler_is_connected(&app->proto))
        {
            protocol_handler_disconnect(&app->proto);
        }
        uint64_t step_ms = get_time_ms();
        protocol_handler_destroy(&app->proto);
        app->proto_initialized = 0;
        PLOG_D("CLEANUP", "协议销毁: %llums", (unsigned long long)(get_time_ms() - step_ms));
    }
    if (app->player_initialized)
    {
        audio_player_stop(&app->player);
        uint64_t step_ms = get_time_ms();
        audio_player_destroy(&app->player);
        app->player_initialized = 0;
        PLOG_D("CLEANUP", "播放器销毁: %llums", (unsigned long long)(get_time_ms() - step_ms));
    }
    if (app->recorder_initialized)
    {
        audio_recorder_module_stop_sending(&app->recorder_mod);
        audio_precache_stop(&app->recorder_mod.precache);
        app->recorder_mod.proto = NULL;
        PLOG_D("CLEANUP", "录音模块已停止 (保留编码器供precache复用)");
    }

    uint64_t total_ms = get_time_ms() - now;
    PLOG_I("CLEANUP", "总清理耗时: %llums (目标 < 200ms)", (unsigned long long)total_ms);
}

static void on_state_changed(xiaozhi_state_t from, xiaozhi_state_t to, void *user_data)
{
    app_context_t *app = (app_context_t *)user_data;

    uint64_t prev_state_enter_time_ms = app->state_enter_time_ms;
    app->state_enter_time_ms = get_time_ms();
    uint64_t now = app->state_enter_time_ms;

    api_server_write_status();

    /* 使用时长打点: 只累计 Speaking(助手TTS播放)真实时长(§四口径) */
    {
        static uint64_t speak_started_ms;
        if (from == kStateSpeaking && to != kStateSpeaking)
            use_limit_on_speaking(get_time_ms() - speak_started_ms);
        else if (to == kStateSpeaking && from != kStateSpeaking)
            speak_started_ms = now;

        /* 会话中断(达限/时段锁)统一走主循环 use_limit_session_break_poll:
         * 播提示后延迟断开(提示播完), 状态回调不再直接掐断 */
    }

    switch (to)
    {
    case kStateStarting:
        PLOG_I("STATE", "Starting: 初始化中");
        watchdog_set_timeout(&app->watchdog, WD_TIMEOUT_ACTIVE);
        break;
    case kStateActivating:
        PLOG_I("STATE", "Activating: 检查设备激活状态");
        watchdog_set_timeout(&app->watchdog, WD_TIMEOUT_ACTIVE);
        break;
    case kStateIdle:
        PLOG_I("STATE", "Idle: 等待唤醒");
        watchdog_set_timeout(&app->watchdog, WD_TIMEOUT_IDLE);
        display_ctrl_set_session(0);          /* 会话域息屏: 会话结束恢复亮屏 */
        if (from == kStateCleaning)
        {
            do_session_cleanup(app, now, prev_state_enter_time_ms);
        }
        app->wakeup_cooldown_done = 0;
        if (app->wakeup.started && !wakeup_is_feed_active(&app->wakeup))
        {
            wakeup_resume_feed(&app->wakeup);
            if (app->pending_wakeup)
            {
                PLOG_I("WAKEUP", "恢复时清除过期的唤醒事件");
                app->pending_wakeup = 0;
                app->pending_wakeup_type = 0;
            }
        }
        break;
    case kStateConnecting:
        PLOG_I("STATE", "Connecting: 建立云端连接");
        watchdog_set_timeout(&app->watchdog, WD_TIMEOUT_ACTIVE);
        /* 连接时暂停唤醒词检测 */
        if (app->wakeup.started)
        {
            wakeup_pause_feed(&app->wakeup);
        }

        /* OTA配置未就绪则无法连接 */
        if (!app->ota_config_received)
        {
            PLOG_W("STATE", "OTA配置未获取, 无法连接");
            state_machine_transition(&app->sm, kStateCleaning);
            break;
        }

        /* 启动连接线程 */
        app->connecting = 1;
        {
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setstacksize(&attr, 64 * 1024);
            pthread_create(&app->connect_thread, &attr, connect_thread_func, app);
            pthread_attr_destroy(&attr);
        }
        break;
    case kStateListening:
        PLOG_I("STATE", "Listening: 等待用户语音 (唤醒后+%llums)",
               (unsigned long long)(get_time_ms() - app->session_start_ms));
        watchdog_set_timeout(&app->watchdog, WD_TIMEOUT_ACTIVE);
        display_ctrl_set_session(1);          /* 会话域息屏管理开始计时 */
        if (protocol_handler_is_connected(&app->proto))
        {
            if (app->listening_mode == LISTENING_MODE_REALTIME && from == kStateSpeaking)
            {
                PLOG_I("STATE", "Realtime模式: Speaking→Listening, 跳过重复start_listening");
            }
            else
            {
                protocol_handler_send_start_listening(&app->proto,
                    app->listening_mode == LISTENING_MODE_REALTIME ? "realtime" : "auto");
            }
        }
        if (app->recorder_initialized)
        {
            if (app->recorder_mod.precache.active)
            {
                audio_recorder_module_start_sending(&app->recorder_mod);
                if (protocol_handler_is_connected(&app->proto))
                {
                    int cached = audio_precache_drain_to_proto(&app->recorder_mod.precache, &app->proto);
                    PLOG_I("STATE", "DEBUG: precache排空完成, 发送了%d帧", cached);
                }
                else
                {
                    audio_precache_stop(&app->recorder_mod.precache);
                    PLOG_W("STATE", "precache排空时连接不可用, 丢弃缓存");
                }
            }
            else if (!audio_recorder_module_is_sending(&app->recorder_mod))
            {
                audio_recorder_module_start_sending(&app->recorder_mod);
            }
        }
        if (app->wakeup.started && wakeup_is_feed_active(&app->wakeup))
        {
            wakeup_pause_feed(&app->wakeup);
        }
        break;
    case kStateSpeaking:
        PLOG_I("STATE", "Speaking: 播放TTS语音 (模式=%s)",
               app->listening_mode == LISTENING_MODE_REALTIME ? "realtime" : "autostop");
        watchdog_set_timeout(&app->watchdog, WD_TIMEOUT_ACTIVE);
        if (app->listening_mode == LISTENING_MODE_REALTIME)
        {
            if (app->recorder_initialized && !audio_recorder_module_is_sending(&app->recorder_mod))
            {
                audio_recorder_module_start_sending(&app->recorder_mod);
            }
            if (app->wakeup.started && wakeup_is_feed_active(&app->wakeup))
            {
                wakeup_pause_feed(&app->wakeup);
            }
        }
        else
        {
            if (app->wakeup.started && !wakeup_is_feed_active(&app->wakeup))
            {
                wakeup_resume_feed(&app->wakeup);
            }
        }
        break;
    case kStateCleaning:
        PLOG_I("STATE", "Cleaning: 释放会话资源");
        watchdog_set_timeout(&app->watchdog, WD_TIMEOUT_ACTIVE);
        app->connecting = 0;
        app->ignore_tts_audio = 0;
        subtitle_clear();
        /* 清理时暂停唤醒词检测 */
        if (app->wakeup.started)
        {
            wakeup_pause_feed(&app->wakeup);
        }
        /* 断开协议连接 */
        if (app->proto_initialized)
        {
            if (protocol_handler_is_connected(&app->proto))
            {
                protocol_handler_disconnect(&app->proto);
            }
        }
        /* 停止录音发送 */
        if (app->recorder_initialized)
        {
            audio_precache_stop(&app->recorder_mod.precache);
            audio_recorder_module_stop_sending(&app->recorder_mod);
        }
        /* 停止并释放播放器音轨 */
        if (app->player_initialized)
        {
            audio_player_stop(&app->player);
        }
        broadcast_sair_end();
        app->pending_cleaning_done = 1;
        break;
    }
}

/**
 * @brief 检查各类超时条件
 *        - 会话总超时
 *        - WiFi断连检测
 *        - 激活状态检查
 *        - 连接/监听/说话/清理超时
 * @param app 应用上下文指针
 */
static void check_timeouts(app_context_t *app)
{
    uint64_t now = get_time_ms();
    xiaozhi_state_t state = state_machine_get_state(&app->sm);

    /* 会话总超时检查（Speaking状态下TTS活跃时暂停计时） */
    if (app->in_session && now - app->session_start_ms > g_app.session_timeout_ms)
    {
        if (state == kStateSpeaking && app->last_tts_audio_ms > 0
            && now - app->last_tts_audio_ms < SPEAK_TIMEOUT_MS)
        {
            app->session_start_ms = now - g_app.session_timeout_ms;
        }
        else if (state != kStateCleaning)
        {
            PLOG_W("TIMEOUT", "会话超时");
            state_machine_transition(&app->sm, kStateCleaning);
            return;
        }
    }

    /* WiFi连接状态定期检查 */
    if (now - app->last_wifi_check_ms > WIFI_CHECK_INTERVAL_MS)
    {
        app->last_wifi_check_ms = now;
        int wifi_ok = config_manager_check_wifi();
        if (!wifi_ok && (state == kStateConnecting || state == kStateListening || state == kStateSpeaking))
        {
            PLOG_W("WIFI", "WiFi在 %s 状态断开, 进入 Cleaning",
                   state_machine_get_state_name(state));
            if (app->mcp_initialized && app->mcp.sound_tts_play)
                app->mcp.sound_tts_play(13);
            state_machine_transition(&app->sm, kStateCleaning);
            return;
        }
    }

    switch (state)
    {
    case kStateActivating:
        if (app->pending_api_activate)
        {
            app->pending_api_activate = 0;
            int wifi_ok = config_manager_check_wifi();
            PLOG_I("ACT", "手动激活: WiFi=%s ota_done=%d ota_config=%d",
                   wifi_ok ? "已连接" : "未连接", app->ota_done, app->ota_config_received);
            if (wifi_ok && app->ota_done && !app->ota_config_received)
            {
                if (app->needs_activation)
                {
                    if (app->mcp_initialized && app->mcp.sound_tts_play)
                        app->mcp.sound_tts_play(11);
                }
                config_manager_check_activation(&app->config);
                if (app->config.has_ws_config)
                {
                    app->ota_config_received = 1;
                    app->needs_activation = 0;
                    state_machine_transition(&app->sm, kStateIdle);
                }
                else
                {
                    app->needs_activation = 1;
                    PLOG_W("ACT", "激活失败, 请重试");
                }
            }
            else if (wifi_ok && app->ota_done && app->ota_config_received)
            {
                PLOG_I("ACT", "已有配置, 转换到 Idle");
                state_machine_transition(&app->sm, kStateIdle);
            }
            app->last_activation_check_ms = now;
        }
        else if (now - app->last_activation_check_ms > ACTIVATION_CHECK_INTERVAL_MS)
        {
            int wifi_ok = config_manager_check_wifi();
            PLOG_I("ACT", "WiFi状态: %s ota_done=%d ota_config=%d",
                   wifi_ok ? "已连接" : "未连接", app->ota_done, app->ota_config_received);
            if (wifi_ok && app->ota_done && app->ota_config_received)
            {
                PLOG_I("ACT", "OTA完成且配置已获取, 转换到 Idle");
                state_machine_transition(&app->sm, kStateIdle);
            }
            app->last_activation_check_ms = now;
        }
        break;

    case kStateConnecting:
        /* 连接超时（含hello等待额外5秒） */
        if (now - app->state_enter_time_ms > HELLO_TIMEOUT_MS + 5000)
        {
            PLOG_W("TIMEOUT", "连接超时");
            state_machine_transition(&app->sm, kStateCleaning);
        }
        break;

    case kStateListening:
        /* 监听超时 */
        if (now - app->state_enter_time_ms > g_app.listen_timeout_ms)
        {
            PLOG_W("TIMEOUT", "监听超时");
            state_machine_transition(&app->sm, kStateCleaning);
        }
        break;

    case kStateSpeaking:
        /* 说话超时（TTS数据活跃时持续刷新） */
        if (app->last_tts_audio_ms > 0 && now - app->last_tts_audio_ms < SPEAK_TIMEOUT_MS)
        {
            app->state_enter_time_ms = now - SPEAK_TIMEOUT_MS;
        }
        else if (now - app->state_enter_time_ms > SPEAK_TIMEOUT_MS)
        {
            PLOG_W("TIMEOUT", "说话超时");
            state_machine_transition(&app->sm, kStateCleaning);
        }
        break;

    case kStateCleaning:
        /* 清理超时（pending_cleaning_done未触发时强制回到Idle） */
        if (now - app->state_enter_time_ms > CLEANUP_TIMEOUT_MS)
        {
            PLOG_W("TIMEOUT", "清理超时 (pending_cleaning_done 未触发?), 强制回到 Idle");
            state_machine_transition(&app->sm, kStateIdle);
        }
        break;

    default:
        break;
    }
}

/**
 * @brief 处理挂起的唤醒事件
 *        根据当前状态决定如何处理唤醒：
 *        - Speaking(自动停止模式): 打断当前TTS，重新进入Listening
 *        - Listening/Connecting: 忽略（已在会话中）
 *        - Idle: 开始新会话
 * @param app 应用上下文指针
 */
static void process_pending_wakeup(app_context_t *app)
{
    /* 每日使用时限闸门: 达限后不连接服务器/不播回复, 仅提醒音一次(§四) */
    if (use_limit_should_block_wakeup())
    {
        return;
    }

    if (!app->pending_wakeup)
        return;
    app->pending_wakeup = 0;

    xiaozhi_state_t state = state_machine_get_state(&app->sm);
    PLOG_I("WAKEUP", "在 %s 状态处理唤醒, type=%d",
           state_machine_get_state_name(state), app->pending_wakeup_type);

    if (state == kStateSpeaking)
    {
        PLOG_I("WAKEUP", "Speaking 中检测到用户打断!");
        app->ignore_tts_audio = 1;
        app->player.aborted = true;
        audio_player_stop(&app->player);
        /* 被打断回复的字幕立即清屏(与 Realtime 抢话路径 1680 行同款):
         * 打字机作废 + 0x233/0x231 两连发清容器 */
        subtitle_interrupt();
        if (protocol_handler_is_connected(&app->proto))
        {
            protocol_handler_send_abort(&app->proto, "wake_word_detected");
            /* 2026-08-27 实测回滚: 打断清队列会丢用户说话开头帧, 且与云端
             * abort-新TTS 竞态误伤新回复(用户实录). 回声残留交给 NLMS 消除. */
            if (app->listening_mode != LISTENING_MODE_REALTIME)
            {
                protocol_handler_clear_send_queue(&app->proto);
            }
            /* abort 网络在途期间云端可能已开新 TTS 流(用户实录: 字幕有新回复
             * 但音频被 abort 掐断). abort 后重发 listen start 令服务器明确回到
             * 监听态, 新语音/新回复链路即刻重建, 避免竞态误伤. */
            protocol_handler_send_start_listening(&app->proto,
                app->listening_mode == LISTENING_MODE_REALTIME ? "realtime" : "auto");
        }
        broadcast_sair_awake(app->pending_wakeup_type);
        if (app->recorder_initialized && app->precache_enabled
            && app->listening_mode != LISTENING_MODE_REALTIME)
        {
            audio_precache_start(&app->recorder_mod.precache);
        }
        state_machine_transition(&app->sm, kStateListening);
        return;
    }

    /* 已在活跃会话中，忽略唤醒 */
    if (state == kStateListening || state == kStateConnecting)
    {
        PLOG_D("WAKEUP", "在活跃会话状态 %s 中忽略唤醒", state_machine_get_state_name(state));
        return;
    }

    /* OTA配置未就绪，忽略唤醒 */
    if (!app->ota_config_received)
    {
        PLOG_W("WAKEUP", "忽略唤醒 - OTA配置尚未获取");
        return;
    }

    /* WiFi未连接时忽略唤醒，避免无网络时反复尝试连接 */
    if (!config_manager_check_wifi())
    {
        PLOG_W("WAKEUP", "忽略唤醒 - WiFi未连接");
        return;
    }

    /* 清理后冷却期内，忽略唤醒 */
    uint64_t now = get_time_ms();
    if (now - app->last_cleaning_end_time_ms < POST_CLEANUP_COOLDOWN_MS)
    {
        PLOG_D("WAKEUP", "清理后冷却中, 忽略");
        return;
    }

    /* 开始新会话 */
    app->last_wakeup_ms = now;
    app->in_session = 1;
    app->session_start_ms = now;

    if (app->recorder_initialized && app->precache_enabled)
    {
        audio_precache_start(&app->recorder_mod.precache);
        PLOG_I("WAKEUP", "音频预缓存已启动, 唤醒后音频将被缓存");
    }

    broadcast_sair_awake(app->pending_wakeup_type);

    state_machine_transition(&app->sm, kStateConnecting);
}

/**
 * @brief 处理挂起的返回键退出事件
 *        在Speaking/Listening/Connecting状态下终止当前会话
 * @param app 应用上下文指针
 */
static void process_pending_key(app_context_t *app, volatile sig_atomic_t *flag,
                                const char *key_name, const char *abort_reason)
{
    if (!*flag)
        return;
    *flag = 0;

    xiaozhi_state_t state = state_machine_get_state(&app->sm);
    PLOG_I("KEY", "%s, 当前状态 %s", key_name, state_machine_get_state_name(state));

    switch (state)
    {
    case kStateSpeaking:
        app->player.aborted = true;
        audio_player_stop(&app->player);
        if (protocol_handler_is_connected(&app->proto))
        {
            protocol_handler_send_abort(&app->proto, abort_reason);
            protocol_handler_clear_send_queue(&app->proto);
        }
        state_machine_transition(&app->sm, kStateCleaning);
        break;
    case kStateConnecting:
    case kStateListening:
        state_machine_transition(&app->sm, kStateCleaning);
        break;
    default:
        break;
    }
}

/**
 * @brief 早期初始化函数（constructor属性，在main之前执行）
 *        重置崩溃信号处理器为默认值，并强制链接applib符号
 */
#include <sys/file.h>

static int g_instance_lock_fd = -1;

static int check_single_instance(void)
{
    const char *lock_path = "/var/upgrade/sair.pid";
    int my_pid = getpid();

    g_instance_lock_fd = open(lock_path, O_WRONLY | O_CREAT, 0644);
    if (g_instance_lock_fd < 0)
    {
        PLOG_E("BOOT", "无法打开 %s: %s", lock_path, strerror(errno));
        return -1;
    }

    if (flock(g_instance_lock_fd, LOCK_EX | LOCK_NB) != 0)
    {
        if (errno == EWOULDBLOCK)
        {
            char buf[16];
            int n = read(g_instance_lock_fd, buf, sizeof(buf) - 1);
            int old_pid = (n > 0) ? atoi(buf) : 0;
            PLOG_W("BOOT", "另一个sair实例正在运行 (pid=%d), 终止之", old_pid);
            close(g_instance_lock_fd);
            g_instance_lock_fd = -1;

            if (old_pid > 0 && old_pid != my_pid)
            {
                kill(old_pid, SIGTERM);
                usleep(500000);
                if (kill(old_pid, 0) == 0)
                {
                    PLOG_W("BOOT", "sair (pid=%d) 未退出, 强制终止", old_pid);
                    kill(old_pid, SIGKILL);
                    usleep(500000);
                }
            }

            int lock_retries = 5;
            while (lock_retries > 0)
            {
                g_instance_lock_fd = open(lock_path, O_WRONLY | O_CREAT, 0644);
                if (g_instance_lock_fd < 0)
                    return -1;
                if (flock(g_instance_lock_fd, LOCK_EX | LOCK_NB) == 0)
                    break;
                close(g_instance_lock_fd);
                g_instance_lock_fd = -1;
                lock_retries--;
                if (lock_retries > 0)
                {
                    PLOG_I("BOOT", "实例锁仍被占用, 等待1秒 (剩余重试%d次)", lock_retries);
                    sleep(1);
                }
            }
            if (lock_retries <= 0)
            {
                PLOG_E("BOOT", "无法获取实例锁: %s", strerror(errno));
                return -1;
            }
        }
        else
        {
            PLOG_E("BOOT", "flock失败: %s", strerror(errno));
            close(g_instance_lock_fd);
            g_instance_lock_fd = -1;
            return -1;
        }
    }

    if (ftruncate(g_instance_lock_fd, 0) == 0)
    {
        char buf[16];
        int len = snprintf(buf, sizeof(buf), "%d\n", my_pid);
        write(g_instance_lock_fd, buf, len);
    }

    PLOG_I("BOOT", "单实例锁已获取 (pid=%d)", my_pid);
    return 0;
}

static void do_hot_update(app_context_t *app)
{
    struct stat st_sair;
    if (stat("/var/upgrade/sair", &st_sair) != 0)
    {
        PLOG_E("OTA", "热更新: /var/upgrade/sair不存在, 跳过");
        return;
    }

    PLOG_I("OTA", "热更新: 优雅停止资源");

    if (app->wakeup.started)
        wakeup_stop(&app->wakeup);

    if (app->recorder_initialized)
    {
        audio_recorder_module_stop_sending(&app->recorder_mod);
        audio_recorder_module_destroy(&app->recorder_mod);
        app->recorder_initialized = 0;
    }

    app->recorder_running = 0;

    if (app->player_initialized)
    {
        audio_player_stop(&app->player);
        audio_player_release_track(&app->player);
        audio_player_destroy(&app->player);
        app->player_initialized = 0;
    }

    if (app->proto_initialized)
    {
        if (protocol_handler_is_connected(&app->proto))
        {
            protocol_handler_disconnect(&app->proto);
        }
        protocol_handler_destroy(&app->proto);
        app->proto_initialized = 0;
    }

    usleep(500000);
    PLOG_I("OTA", "热更新: 资源已释放, 执行新版本 /var/upgrade/sair");

    unlink("/tmp/sair_status.json");
    unlink("/tmp/sair_config.json");
    unlink("/tmp/sair_diag.json");
    unlink("/tmp/sair_cmd.json");

    if (g_instance_lock_fd >= 0)
    {
        flock(g_instance_lock_fd, LOCK_UN);
        close(g_instance_lock_fd);
        g_instance_lock_fd = -1;
    }

    int max_fd = sysconf(_SC_OPEN_MAX);
    if (max_fd > 4096)
        max_fd = 4096;
    for (int fd = 3; fd < max_fd; fd++)
        close(fd);

    char *new_argv[] = {"/var/upgrade/sair", NULL};
    execvp("/var/upgrade/sair", new_argv);

    PLOG_E("OTA", "execvp 失败");
    _exit(1);
}

__attribute__((constructor)) static void early_init(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);

    /* 强制引用applib_init，确保动态链接器在main前加载libapplib.so */
    volatile void *force_applib_ref = (void *)applib_init;
    (void)force_applib_ref;
}

/**
 * @brief 主函数
 *        初始化所有模块，启动子线程，进入主事件循环
 * @param argc 参数数量
 * @param argv 参数数组
 * @return 0正常退出
 */
static int check_network(void)
{
    FILE *f = fopen("/sys/class/net/wlan0/operstate", "r");
    if (f)
    {
        char state[32] = {0};
        fgets(state, sizeof(state), f);
        fclose(f);
        if (strstr(state, "up"))
            return 1;
    }
    f = fopen("/proc/net/route", "r");
    if (f)
    {
        char line[256];
        while (fgets(line, sizeof(line), f))
        {
            unsigned int dest, mask;
            if (sscanf(line, "%*s %x %*x %*x %*x %*x %*x %x", &dest, &mask) >= 1)
            {
                if (dest == 0 && mask != 0)
                {
                    fclose(f);
                    return 1;
                }
            }
        }
        fclose(f);
    }
    return 0;
}

static int wait_for_network(int timeout_sec)
{
    PLOG_I("NET", "等待网络连接 (最多%d秒)...", timeout_sec);
    int waited = 0;
    while (waited < timeout_sec)
    {
        if (check_network())
        {
            PLOG_I("NET", "网络已连接 (等待%d秒)", waited);
            return 0;
        }
        sleep(2);
        waited += 2;
    }
    PLOG_W("NET", "等待网络超时 (%d秒)", timeout_sec);
    return -1;
}

int main(int argc, char *argv[])
{
    int ret;

    plog_init(PLOG_PATH);
    plog_set_level(PLOG_LEVEL_INFO);

    {
        const char *ver_file = "/var/upgrade/.xiaozhi_version";
        char old_ver[64] = {0};
        FILE *vfp = fopen(ver_file, "r");
        if (vfp)
        {
            if (fgets(old_ver, sizeof(old_ver), vfp))
            {
                int vlen = strlen(old_ver);
                while (vlen > 0 && (old_ver[vlen - 1] == '\n' || old_ver[vlen - 1] == '\r'))
                    old_ver[--vlen] = '\0';
            }
            fclose(vfp);
        }
        if (strcmp(old_ver, XIAOZHI_VERSION) != 0)
        {
            plog_close();
            truncate(PLOG_PATH, 0);
            plog_init(PLOG_PATH);
            plog_set_level(PLOG_LEVEL_DEBUG);
            PLOG_I("BOOT", "版本变更 %s -> %s, 已清除旧日志", old_ver[0] ? old_ver : "(无)", XIAOZHI_VERSION);
            plog_set_level(PLOG_LEVEL_INFO);
            vfp = fopen(ver_file, "w");
            if (vfp)
            {
                fprintf(vfp, "%s\n", XIAOZHI_VERSION);
                fclose(vfp);
            }
        }
    }

    PLOG_I("BOOT", "========================================");
    PLOG_I("BOOT", "=== 小智助手启动标记 ===");
    PLOG_I("BOOT", "=== pid=%d time=%ld ===", getpid(), (long)time(NULL));
    PLOG_I("BOOT", "========================================");

    srand((unsigned int)time(NULL) ^ (unsigned int)getpid());

    check_single_instance();

    if (wait_for_network(300) != 0)
    {
        PLOG_W("BOOT", "网络不可用, 30秒后退出等待重启");
        sleep(30);
        return 1;
    }

    PLOG_I("MAIN", "小智助手启动中, pid=%d", getpid());

    /* 写入版本标识文件 */
    {
        FILE *id_fp = fopen("/var/upgrade/.xiaozhi_sair", "w");
        if (id_fp)
        {
            fprintf(id_fp, "xiaozhi-assistant v%s pid=%d\n", XIAOZHI_VERSION, getpid());
            fclose(id_fp);
        }
    }

    /* sair 是助手二进制文件名（平台约束），设置argv[0]使applib识别正确 */
    char sair_argv0[] = "/var/upgrade/sair";
    argv[0] = sair_argv0;

    /* 确保mqueue文件系统已挂载（IPC消息队列依赖） */
    {
        PLOG_I("INIT", "确保 mqueue 文件系统已挂载");
        struct stat mq_stat;
        if (stat("/dev/mqueue", &mq_stat) != 0)
        {
            mkdir("/dev/mqueue", 0777);
        }
        if (stat("/dev/mqueue/.", &mq_stat) != 0 ||
            access("/dev/mqueue/manager_mq", F_OK) != 0)
        {
            PLOG_I("INIT", "正在挂载 mqueue 文件系统");
            int mq_ret = mount("none", "/dev/mqueue", "mqueue", 0, NULL);
            if (mq_ret == 0)
            {
                PLOG_I("INIT", "mqueue 挂载成功");
            }
            else
            {
                PLOG_W("INIT", "mqueue 挂载失败 (errno=%d), 继续尝试", errno);
            }
        }
        else
        {
            PLOG_I("INIT", "mqueue 已挂载");
        }
    }

    /* applib初始化前预处理（热更新检测等） */
    int is_hot_update = 0;
    {
        is_hot_update = pre_applib_init();
        PLOG_I("BOOT", "is_hot_update=%d", is_hot_update);
    }

    /* 注册信号处理器 */
    setup_signals();

    /* 初始化applib框架 */
    ret = applib_init(argc, argv);
    PLOG_I("INIT", "applib_init 返回值=%d errno=%d", ret, errno);

    re_register_signals();

    int applib_ok = (ret != 0);
    if (!applib_ok)
    {
        PLOG_E("INIT", "applib_init 失败 (ret=%d, errno=%d)", ret, errno);
    }

    {
        void *libapplib = dlopen("libapplib.so", RTLD_LAZY | RTLD_NOLOAD);
        if (libapplib)
        {
            app_info_t **real_ptr = (app_info_t **)dlsym(libapplib, "g_this_app_info");
            if (real_ptr && *real_ptr)
            {
                g_this_app_info = *real_ptr;
                PLOG_I("INIT", "g_this_app_info 通过 dlsym 同步: %p", (void *)g_this_app_info);
            }
        }
    }

    PLOG_I("INIT", "g_this_app_info=%p", (void *)g_this_app_info);

    prctl(PR_SET_NAME, "sair_main");

    if (!g_this_app_info)
    {
        PLOG_W("INIT", "g_this_app_info 为空, 禁用看门狗");
        sys_forbid_soft_watchdog(1);
        set_soft_watchdog_timeout(60000);
        int wd_fd = open("/dev/watchdog", O_WRONLY);
        if (wd_fd >= 0)
        {
            write(wd_fd, "V", 1);
            close(wd_fd);
        }
    }

    if (set_sched_priority() < 0)
    {
        PLOG_W("INIT", "设置调度优先级失败, 以普通优先级继续");
    }

    if (is_hot_update)
    {
        PLOG_I("INIT", "热更新: 确保音频服务运行中");
        start_service("audio_service");
        usleep(300000);
    }
    else
    {
        PLOG_I("INIT", "启动音频服务");
        start_service("audio_service");
    }

    app_context_t *app = &g_app;
    memset(app, 0, sizeof(*app));
    app->self_pipe[0] = -1;
    app->self_pipe[1] = -1;
    app->msg_pipe[0] = -1;
    app->msg_pipe[1] = -1;
    app->boot_time_ms = get_time_ms();
    g_app.listen_timeout_ms = LISTEN_TIMEOUT_MS;
    g_app.session_timeout_ms = SESSION_TIMEOUT_MS;
    g_app.wakeup_cooldown_ms = WAKEUP_COOLDOWN_MS;
    g_app.ws_ping_interval_ms = WS_PING_INTERVAL_MS;
    /* 默认 AutoStop(2026-08-29 用户决策): Realtime 依赖的本地 NLMS 回声消除
     * 效果有限(X14 终论: 原生持续 AEC 输出接口不存在, 软方案已达上限),
     * Realtime 双讲场景回声残留影响体验; AutoStop 打断走唤醒词更可靠. */
    g_app.listening_mode = LISTENING_MODE_AUTOSTOP;
    app->precache_enabled = read_precache_enabled();
    PLOG_I("INIT", "音频预缓存: %s", app->precache_enabled ? "已启用" : "已禁用");
    read_custom_ws_url(app->custom_ws_url, sizeof(app->custom_ws_url));
    if (app->custom_ws_url[0])
        PLOG_I("INIT", "自定义WS URL: %s", app->custom_ws_url);

    /* 创建self-pipe用于子线程通知主循环 */
    if (pipe(app->self_pipe) == 0)
    {
        fcntl(app->self_pipe[0], F_SETFL, O_NONBLOCK);
        fcntl(app->self_pipe[1], F_SETFL, O_NONBLOCK);
        PLOG_I("INIT", "self-pipe 已创建: r=%d w=%d", app->self_pipe[0], app->self_pipe[1]);
    }
    else
    {
        PLOG_W("INIT", "pipe() 失败: %d", errno);
    }

    pthread_mutex_init(&app->msg_mutex, NULL);

    /* 初始化状态机并注册状态变更观察者 */
    state_machine_init(&app->sm);
    state_machine_add_observer(&app->sm, kStateAny, 100, on_state_changed, app);

    /* 初始化配置管理器 */
    config_manager_init(&app->config);
    strncpy(app->device_mac, app->config.device_mac, sizeof(app->device_mac) - 1);
    strncpy(app->client_id, app->config.client_id, sizeof(app->client_id) - 1);

    /* 尝试从缓存文件加载WebSocket配置 */
    {
        FILE *fp = fopen("/var/upgrade/.ws_config", "r");
        if (fp)
        {
            char line1[512] = {0}, line2[512] = {0}, line3[64] = {0};
            if (fgets(line1, sizeof(line1), fp))
            {
                int len = strlen(line1);
                while (len > 0 && (line1[len - 1] == '\n' || line1[len - 1] == '\r'))
                    line1[--len] = '\0';
            }
            if (fgets(line2, sizeof(line2), fp))
            {
                int len = strlen(line2);
                while (len > 0 && (line2[len - 1] == '\n' || line2[len - 1] == '\r'))
                    line2[--len] = '\0';
            }
            if (fgets(line3, sizeof(line3), fp))
            {
                int len = strlen(line3);
                while (len > 0 && (line3[len - 1] == '\n' || line3[len - 1] == '\r'))
                    line3[--len] = '\0';
            }
            fclose(fp);
            if (line1[0] && line2[0])
            {
                strncpy(app->config.ws_url, line1, sizeof(app->config.ws_url) - 1);
                strncpy(app->config.ws_token, line2, sizeof(app->config.ws_token) - 1);
                app->config.has_ws_config = 1;
                app->ota_config_received = 1;
                PLOG_I("INIT", "已加载缓存的ws配置: url=%s", app->config.ws_url);
            }
            if (line3[0])
            {
                strncpy(app->config.activation_code, line3, sizeof(app->config.activation_code) - 1);
                PLOG_I("INIT", "已加载缓存的激活码: %s", app->config.activation_code);
            }
        }
    }

    {
        FILE *mfp = fopen("/var/upgrade/.mcp_endpoint", "r");
        if (mfp)
        {
            char mline[512] = {0};
            if (fgets(mline, sizeof(mline), mfp))
            {
                int mlen = strlen(mline);
                while (mlen > 0 && (mline[mlen - 1] == '\n' || mline[mlen - 1] == '\r'))
                    mline[--mlen] = '\0';
                if (mline[0])
                {
                    strncpy(app->config.mcp_endpoint, mline, sizeof(app->config.mcp_endpoint) - 1);
                    PLOG_I("INIT", "已加载缓存的MCP接入点: %s", app->config.mcp_endpoint);
                }
            }
            fclose(mfp);
        }
    }

    {
        FILE *mfp = fopen("/var/upgrade/.listening_mode", "r");
        if (mfp)
        {
            char mline[32] = {0};
            if (fgets(mline, sizeof(mline), mfp))
            {
                int mlen = strlen(mline);
                while (mlen > 0 && (mline[mlen - 1] == '\n' || mline[mlen - 1] == '\r'))
                    mline[--mlen] = '\0';
                if (strcmp(mline, "realtime") == 0)
                {
                    app->listening_mode = LISTENING_MODE_REALTIME;
                    PLOG_I("INIT", "已加载监听模式: realtime");
                }
                else if (strcmp(mline, "autostop") == 0)
                {
                    app->listening_mode = LISTENING_MODE_AUTOSTOP;
                    PLOG_I("INIT", "已加载监听模式: autostop");
                }
            }
            fclose(mfp);
        }
    }

    {
        /* AEC方案持久化(.aec_mode): cloud=云端声明+本地旁路, local=本地NLMS */
        FILE *afp = fopen("/var/upgrade/.aec_mode", "r");
        if (afp)
        {
            char aline[32] = {0};
            if (fgets(aline, sizeof(aline), afp))
            {
                int alen = strlen(aline);
                while (alen > 0 && (aline[alen-1] == 0x0A || aline[alen-1] == 0x0D))
                    aline[--alen] = 0;
                if (strcmp(aline, "cloud") == 0)
                {
                    app->aec_mode = 1;
                    PLOG_I("INIT", "AEC方案: cloud(云端)");
                }
            }
            fclose(afp);
        }
    }

    api_server_write_config();

    /* 初始化音频分发器 */
    audio_dispatcher_init(&app->audio_disp);
    heap_check("pre-wakeup");

    /* 早期初始化录音模块（仅编码器+回调，无proto），使precache可用 */
    {
        int rec_ret = audio_recorder_module_early_init(&app->recorder_mod, &app->audio_disp);
        if (rec_ret == 0)
        {
            app->recorder_initialized = 1;
            PLOG_I("INIT", "录音模块早期初始化成功 (precache可用)");
        }
        else
        {
            PLOG_W("INIT", "录音模块早期初始化失败: %d", rec_ret);
        }
    }
    heap_check("post-recorder-early");

    /* 初始化唤醒词检测模块 */
    ret = wakeup_init(&app->wakeup, &app->audio_disp, on_wakeup_event, app);
    PLOG_I("INIT", "wakeup_init 返回值=%d", ret);
    heap_check("post-wakeup");

    /* 初始化触摸按键模块 */
    ret = touch_key_init(&app->touch_key, on_key_event, app);
    PLOG_I("INIT", "touch_key_init 返回值=%d", ret);
    heap_check("post-touch");

    /* 初始化MCP设备控制模块 */
    ret = mcp_handler_init(&app->mcp);
    if (ret == 0)
    {
        app->mcp_initialized = 1;
        mcp_handler_set_send_cb(&app->mcp, mcp_send_handler, app);
        PLOG_I("INIT", "mcp_handler_init 成功");
    }
    else
    {
        PLOG_W("INIT", "mcp_handler_init 失败: %d (MCP已禁用)", ret);
    }

    /* 注册IPC消息分发器 */
    use_limit_init();          /* 每日使用时长(§四) */
    display_ctrl_start();      /* 会话域息屏管理器(§八需求③) */

    /* 拉起 xwebd 面板守护(rootfs squashfs 只读无法挂 rcS, 借助手进程宿主化, 幂等) */
    if (access("/var/upgrade/xwebd", X_OK) == 0 &&
        system("pidof xwebd >/dev/null 2>&1") != 0)
    {
        system("/var/upgrade/xwebd -d >/dev/null 2>&1 &");
        PLOG_I("INIT", "xwebd 已由助手拉起 (-d)");
    }

    /* 每日动画开关: 按配置立即占位/恢复(联网下载前生效) */
    boot_push_apply(boot_push_disable_enabled());

    ret = register_srv_dispatcher(proc_srv_msg);
    PLOG_I("INIT", "register_srv_dispatcher 返回值=%d", ret);
    heap_check("post-register");

    ret = register_sys_dispatcher(proc_sys_msg);
    PLOG_I("INIT", "register_sys_dispatcher 返回值=%d", ret);

    g_running = 1;
    app->running = 1;

    api_server_start();
    PLOG_I("INIT", "文件IPC接口已启动");

    watchdog_init(&app->watchdog);

    PLOG_I("INIT", "主循环将使用poll超时驱动定时检查");

    app->recorder_running = 1;
    {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 64 * 1024);
        pthread_create(&app->recorder_thread, &attr, recorder_thread_func, app);
        pthread_attr_destroy(&attr);
        {
            struct sched_param sp;
            sp.sched_priority = 12;
            if (pthread_setschedparam(app->recorder_thread, SCHED_RR, &sp) == 0)
                PLOG_I("INIT", "录音线程已设置 SCHED_RR 优先级 12");
            else
                PLOG_W("INIT", "录音线程设置调度策略失败");
        }
        PLOG_I("INIT", "音频录音线程已启动 (64KB栈)");
    }

    /* 启动OTA线程 */
    {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 128 * 1024);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&app->ota_thread, &attr, ota_thread_func, app);
        pthread_attr_destroy(&attr);
        PLOG_I("INIT", "OTA线程已启动 (128KB栈)");
    }

    /* 初始状态转换到Activating */
    state_machine_transition(&app->sm, kStateActivating);
    app->state_enter_time_ms = get_time_ms();
    app->last_activation_check_ms = get_time_ms();
    app->last_activation_retry_ms = get_time_ms();
    app->last_wifi_check_ms = get_time_ms();

    /* 启动IPC消息接收线程 */
    {
        app->msg_thread_running = 1;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 64 * 1024);
        pthread_create(&app->msg_thread, &attr, msg_thread_func, app);
        pthread_attr_destroy(&attr);
        PLOG_I("INIT", "消息接收线程已启动 (64KB栈)");
    }

    PLOG_I("MAIN", "进入事件循环 (非阻塞)");

    uint64_t last_slow_check_ms = 0;

    while (g_running)
    {
        /* 清空self-pipe（消费所有待处理通知） */
        {
            char pipe_buf[64];
            while (read(app->self_pipe[0], pipe_buf, sizeof(pipe_buf)) > 0)
            {
            }
        }

        /* 处理IPC消息 */
        {
            pthread_mutex_lock(&app->msg_mutex);
            if (app->msg_available)
            {
                char local_buf[MSG_SIZE];
                memcpy(local_buf, app->msg_buf, MSG_SIZE);
                app->msg_available = 0;
                pthread_mutex_unlock(&app->msg_mutex);
                dispatch_msg(local_buf);
            }
            else
            {
                pthread_mutex_unlock(&app->msg_mutex);
            }
        }

        /* OTA完成后从Activating转换到Idle */
        if (app->ota_done && app->ota_config_received &&
            state_machine_get_state(&app->sm) == kStateActivating)
        {
            PLOG_I("MAIN", "OTA就绪, 转换到 Idle");
            state_machine_transition(&app->sm, kStateIdle);
        }

        if (app->proto_initialized)
        {
            if (protocol_handler_is_connected(&app->proto))
            {
                protocol_handler_poll(&app->proto, 10);
            }
            else if (app->proto.connected)
            {
                protocol_handler_poll(&app->proto, 50);
            }
        }

        /* 处理各类挂起事件 */
        process_pending_wakeup(app);
        process_pending_key(app, &app->pending_key_exit, "BACK键退出", "user_key_exit");
        process_pending_key(app, &app->pending_key_home, "HOME键", "user_key_home");

        /* 字幕打字机推进 + 自动清屏 */
        subtitle_typing_tick();

        /* 每日动画占位维护(60s 节流, 配置变化时生效) */
        boot_push_tick();

        /* 处理API中止请求 */
        if (app->pending_api_abort)
        {
            app->pending_api_abort = 0;
            PLOG_I("API", "处理挂起的中止请求");
            xiaozhi_state_t cur = state_machine_get_state(&app->sm);
            if (cur == kStateListening || cur == kStateSpeaking)
            {
                state_machine_transition(&app->sm, kStateCleaning);
            }
        }

        /* smart_player 音源仲裁 / 每日限时中断: 停止当前会话(§五 #3) */
        if (app->pending_stop_request)
        {
            app->pending_stop_request = 0;
            PLOG_I("IPC", "处理挂起的外部停止请求");
            xiaozhi_state_t cur = state_machine_get_state(&app->sm);
            if (cur == kStateListening || cur == kStateSpeaking)
            {
                state_machine_transition(&app->sm, kStateCleaning);
            }
        }

        /* 限时/时段锁统一轮询: 锁定首次进入播提示并开 2.5s 延迟窗,
         * 窗口到点(提示播完)才断开——避免"说到一半突然哑火" */
        if (app->in_session && use_limit_session_break_poll())
        {
            xiaozhi_state_t cur = state_machine_get_state(&app->sm);
            if (cur == kStateListening || cur == kStateSpeaking)
            {
                PLOG_I("UL", "锁定提示已播完, 断开并清理");
                state_machine_transition(&app->sm, kStateCleaning);
            }
            use_limit_break_reset();
        }

        /* 处理API激活请求 */
        if (app->pending_api_activate)
        {
            app->pending_api_activate = 0;
            PLOG_I("API", "处理挂起的激活请求");
            int wifi_ok = config_manager_check_wifi();
            if (wifi_ok)
            {
                config_manager_check_activation(&app->config);
                api_server_write_status();
                if (app->config.has_ws_config)
                {
                    app->needs_activation = 0;
                    xiaozhi_state_t cur = state_machine_get_state(&app->sm);
                    if (cur == kStateActivating)
                    {
                        app->ota_config_received = 1;
                        state_machine_transition(&app->sm, kStateIdle);
                    }
                    PLOG_I("ACT", "设备已激活, 获取到WebSocket配置");
                }
                else if (app->config.activation_code[0])
                {
                    app->needs_activation = 1;
                    PLOG_I("ACT", "获取到激活码: %s", app->config.activation_code);
                }
                else
                {
                    PLOG_W("ACT", "激活检查完成, 未获取到激活码或配置");
                }
            }
            else
            {
                PLOG_W("ACT", "WiFi未连接, 无法进行激活检查");
            }
        }

        /* 处理API配置变更 */
        if (app->pending_api_config)
        {
            app->pending_api_config = 0;
            PLOG_I("API", "处理挂起的配置: %s", app->pending_config_buf);
            char *tok = app->pending_config_buf;
            while (tok && *tok)
            {
                char *sep = strchr(tok, ';');
                if (sep)
                    *sep = '\0';
                char *eq = strchr(tok, '=');
                if (eq)
                {
                    *eq = '\0';
                    char *key = tok;
                    char *val = eq + 1;
                    if (strcmp(key, "ws_url") == 0 && val[0])
                    {
                        if (app->proto_initialized && protocol_handler_is_connected(&app->proto))
                        {
                            PLOG_I("API", "ws_url变更前断开连接");
                            protocol_handler_disconnect(&app->proto);
                        }
                        strncpy(app->config.ws_url, val, sizeof(app->config.ws_url) - 1);
                        app->config.has_ws_config = 1;
                        PLOG_I("API", "ws_url 已更新为 %s", app->config.ws_url);
                    }
                }
                tok = sep ? sep + 1 : NULL;
            }
            memset(app->pending_config_buf, 0, sizeof(app->pending_config_buf));
        }

        /* 处理API唤醒请求 */
        if (app->pending_api_wakeup)
        {
            app->pending_api_wakeup = 0;
            PLOG_I("API", "处理挂起的唤醒请求");
            xiaozhi_state_t cur = state_machine_get_state(&app->sm);
            if (cur == kStateIdle)
            {
                app->pending_wakeup = 1;
                app->pending_wakeup_type = 0;
            }
        }

        /* 清理完成后转换到Idle */
        if (app->pending_cleaning_done)
        {
            app->pending_cleaning_done = 0;
            xiaozhi_state_t cur = state_machine_get_state(&app->sm);
            if (cur == kStateCleaning)
            {
                state_machine_transition(&app->sm, kStateIdle);
            }
        }

        /* 处理热更新 */
        if (g_hot_update_pending)
        {
            g_hot_update_pending = 0;
            do_hot_update(app);
        }

        /* Idle状态下启动唤醒词检测（延迟1.5秒冷却） */
        {
            xiaozhi_state_t cur_state = state_machine_get_state(&app->sm);
            if (cur_state == kStateIdle && app->wakeup.initialized && !app->wakeup.started)
            {
                if (!app->wakeup_cooldown_done)
                {
                    uint64_t idle_elapsed = get_time_ms() - app->state_enter_time_ms;
                    if (idle_elapsed >= 1500)
                    {
                        app->wakeup_cooldown_done = 1;
                        if (!app->wakeup.started)
                        {
                            PLOG_I("STATE", "唤醒冷却完成 (%llums), 启动唤醒检测",
                                   (unsigned long long)idle_elapsed);
                            wakeup_start(&app->wakeup);
                            app->wakeup_start_time_ms = get_time_ms();
                        }
                        if (app->pending_wakeup)
                        {
                            PLOG_I("WAKEUP", "启动时清除过期的唤醒事件");
                            app->pending_wakeup = 0;
                            app->pending_wakeup_type = 0;
                        }
                    }
                }
            }
            /* 唤醒feed自愈(2026-08-30): 异常会话(如云端瞬时断开)的状态转换
             * 竞态可能使 paused 标志未被 resume, 导致 Idle 下唤醒引擎收不到
             * 音频(实测: 会话结束后唤醒失灵). 主循环周期检查强制恢复,
             * 无论根因是何种竞态, 下一循环 tick 即自愈 */
            if (cur_state == kStateIdle && app->wakeup.started &&
                !wakeup_is_feed_active(&app->wakeup))
            {
                wakeup_resume_feed(&app->wakeup);
                PLOG_W("WAKEUP", "feed 意外未激活, 已自愈恢复");
            }
        }

        /* 检查各类超时 */
        check_timeouts(app);

        {
            uint64_t now_ms = get_time_ms();
            if (now_ms - last_slow_check_ms >= 2000)
            {
                last_slow_check_ms = now_ms;
                api_server_check_commands();

                struct stat trigger_st;
                if (stat("/tmp/xiaozhi_wakeup_trigger", &trigger_st) == 0)
                {
                    unlink("/tmp/xiaozhi_wakeup_trigger");
                    PLOG_I("DEBUG", "检测到模拟唤醒触发");
                    app->pending_wakeup = 1;
                    app->pending_wakeup_type = 0;
                    process_pending_wakeup(app);
                }

                struct stat reload_st;
                if (stat("/var/upgrade/.reload_config", &reload_st) == 0)
                {
                    unlink("/var/upgrade/.reload_config");
                    PLOG_I("CFG", "检测到配置重载触发");
                    int changed = config_manager_reload(&app->config);
                    (void)changed;
                }
            }
        }

        {
            xiaozhi_state_t cur_state = state_machine_get_state(&app->sm);
            int poll_ms = (cur_state == kStateListening || cur_state == kStateSpeaking || cur_state == kStateConnecting) ? 50 : 200;
            struct pollfd pfd;
            pfd.fd = app->self_pipe[0];
            pfd.events = POLLIN;
            poll(&pfd, 1, poll_ms);
        }

        if (g_sigusr1_received)
        {
            g_sigusr1_received = 0;
            api_server_check_commands();
        }
    }

    g_running = 0;
    PLOG_I("MAIN", "事件循环已退出");

    /* 等待消息接收线程结束 */
    app->msg_thread_running = 0;
    {
        char c = 'X';
        if (app->self_pipe[1] >= 0)
            write(app->self_pipe[1], &c, 1);
    }
    pthread_join(app->msg_thread, NULL);

    /* 等待录音线程结束 */
    app->recorder_running = 0;
    pthread_join(app->recorder_thread, NULL);

    /* 清理所有模块资源 */
    if (app->proto_initialized)
    {
        if (protocol_handler_is_connected(&app->proto))
        {
            protocol_handler_disconnect(&app->proto);
        }
        protocol_handler_destroy(&app->proto);
    }
    if (app->player_initialized)
    {
        audio_player_destroy(&app->player);
    }
    if (app->recorder_initialized)
    {
        audio_recorder_module_destroy(&app->recorder_mod);
    }

    wakeup_destroy(&app->wakeup);
    touch_key_destroy(&app->touch_key);
    if (app->mcp_initialized)
    {
        mcp_handler_destroy(&app->mcp);
    }
    watchdog_destroy(&app->watchdog);
    config_manager_destroy(&app->config);
    state_machine_destroy(&app->sm);

    if (app->self_pipe[0] >= 0)
        close(app->self_pipe[0]);
    if (app->self_pipe[1] >= 0)
        close(app->self_pipe[1]);

    pthread_mutex_destroy(&app->msg_mutex);

    applib_quit();

    if (g_instance_lock_fd >= 0)
    {
        flock(g_instance_lock_fd, LOCK_UN);
        close(g_instance_lock_fd);
        g_instance_lock_fd = -1;
    }

    PLOG_I("MAIN", "小智助手已退出");
    plog_close();
    return 0;
}

