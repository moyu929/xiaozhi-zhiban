/**
 * @file display_ctrl.c
 * @brief 会话域背光/息屏管理器实现（见 display_ctrl.h）
 */
#include "display_ctrl.h"
#include "plog.h"

#include "applib_api.h"          /* get_config_int */

#include <sys/prctl.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#define BL_BASE       "/sys/class/backlight/owl_backlight/"
#define BL_POWER      BL_BASE "bl_power"
#define BL_BRIGHTNESS BL_BASE "brightness"

#define POLL_INTERVAL_MS 500

typedef struct {
    pthread_t thread;
    volatile int running;
    volatile int session_active;
    volatile int screen_off;
    volatile uint64_t last_activity_ms;
    int interval_sec;            /* <=0 = 不息屏 */
    int saved_brightness;
} dc_ctx_t;

static dc_ctx_t g_dc;


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
static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + ts.tv_nsec / 1000000u;
}

static int write_sysfs(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0)
    {
        PLOG_W("DISP", "打开 %s 失败", path);
        return -1;
    }
    ssize_t n = write(fd, val, strlen(val));
    close(fd);
    return n > 0 ? 0 : -1;
}

static int read_brightness(int defval)
{
    FILE *fp = fopen(BL_BRIGHTNESS, "r");
    if (!fp)
        return defval;
    int v = defval;
    if (fscanf(fp, "%d", &v) != 1)
        v = defval;
    fclose(fp);
    return v;
}

static void screen_off_apply(void)
{
    if (g_dc.screen_off)
        return;
    g_dc.saved_brightness = read_brightness(200);
    if (write_sysfs(BL_POWER, "1") == 0)
    {
        g_dc.screen_off = 1;
        PLOG_I("DISP", "息屏 (saved brightness=%d)", g_dc.saved_brightness);
    }
}

static void screen_on_restore(const char *reason)
{
    if (!g_dc.screen_off)
        return;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", g_dc.saved_brightness > 0 ? g_dc.saved_brightness : 200);
    write_sysfs(BL_POWER, "0");
    write_sysfs(BL_BRIGHTNESS, buf);
    g_dc.screen_off = 0;
    PLOG_I("DISP", "亮屏 (%s, brightness=%s)", reason, buf);
}

static void *dc_thread_func(void *arg)
{
    (void)arg;
    prctl(PR_SET_NAME, "disp_ctrl", 0, 0, 0);

    while (g_dc.running)
    {
        usleep(POLL_INTERVAL_MS * 1000);
        if (g_dc.interval_sec > 0 && g_dc.session_active && !g_dc.screen_off &&
            now_ms() - g_dc.last_activity_ms >= (uint64_t)g_dc.interval_sec * 1000u)
        {
            screen_off_apply();
        }
    }
    return NULL;
}

void display_ctrl_start(void)
{
    if (g_dc.running)
        return;
    memset((void *)&g_dc, 0, sizeof(g_dc));
    g_dc.interval_sec = cfg_get_int("SCREEN_OFF_IDLE_SEC", 0);
    g_dc.last_activity_ms = now_ms();

    g_dc.running = 1;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 24 * 1024);
    if (pthread_create(&g_dc.thread, &attr, dc_thread_func, NULL) != 0)
    {
        PLOG_E("DISP", "管理线程创建失败, 功能停用");
        g_dc.running = 0;
    }
    pthread_attr_destroy(&attr);
    PLOG_I("DISP", "start: SCREEN_OFF_IDLE_SEC=%d%s",
           g_dc.interval_sec, g_dc.interval_sec > 0 ? "" : " (不息屏)");
}

void display_ctrl_reload_config(void)
{
    g_dc.interval_sec = cfg_get_int("SCREEN_OFF_IDLE_SEC", 0);
}

void display_ctrl_note_activity(void)
{
    g_dc.last_activity_ms = now_ms();
}

void display_ctrl_set_session(int active)
{
    g_dc.session_active = active;
    if (active)
        g_dc.last_activity_ms = now_ms();
    else if (g_dc.screen_off)
        screen_on_restore("session-end");
}

int display_ctrl_notify_input(void)
{
    if (g_dc.screen_off)
    {
        screen_on_restore("input");
        g_dc.last_activity_ms = now_ms();  /* 只点亮，计数重置，吞掉该次输入 */
        return 1;
    }
    if (g_dc.session_active)
        g_dc.last_activity_ms = now_ms();
    return 0;
}
