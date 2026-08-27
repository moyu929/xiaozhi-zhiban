/**
 * @file platform_power.c
 * @brief 平台电源/音效封装实现（白名单隔离层，见 platform_power.h）
 */
#include "platform_power.h"
#include "plog.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>

/* ---- libmsg_server_api: sound_tts_play(id) ------------------------------- */

typedef int (*snd_tts_fn_t)(int id);

static snd_tts_fn_t load_sound_tts_play(void)
{
    static snd_tts_fn_t fn;
    static int resolved;

    if (!resolved)
    {
        resolved = 1;
        void *h = dlopen("libmsg_server_api.so", RTLD_NOW | RTLD_LAZY);
        if (h)
        {
            fn = (snd_tts_fn_t)dlsym(h, "sound_tts_play");
        }
        if (!fn)
            PLOG_W("PW", "sound_tts_play 不可用: %s", h ? dlerror() : "dlopen failed");
    }
    return fn;
}

void platform_tts_play(int id)
{
    if (id < 0)
        return;

    snd_tts_fn_t fn = load_sound_tts_play();
    if (fn)
    {
        PLOG_I("PW", "tts_play(%d)", id);
        fn(id);
    }
}

/* ---- libsysime_api: set_timed_shutdown_time(sec) ------------------------- */

typedef int (*std_time_fn_t)(int seconds);

/**
 * 关机兜底线程：给原生动画链 8 s 时窗。正常关机会经由 manager 杀死本进程，
 * 该线程根本活不到执行 system()；一旦活着执行到说明优雅链路失效，硬关兜底。
 */
static void *shutdown_fallback_thread(void *arg)
{
    (void)arg;
    sleep(8);
    PLOG_W("PW", "优雅关机 8s 未生效, fallback poweroff -f");
    system("poweroff -f");
    return NULL;
}

int platform_power_shutdown_elegant(void)
{
    std_time_fn_t fn = NULL;

    void *h = dlopen("libsystime_api.so", RTLD_NOW | RTLD_LAZY);
    if (h)
    {
        dlerror();
        fn = (std_time_fn_t)dlsym(h, "set_timed_shutdown_time");
        if (!fn)
            PLOG_W("PW", "set_timed_shutdown_time 符号缺失: %s", dlerror());
    }
    else
    {
        PLOG_W("PW", "libsystime_api.so 打不开: %s", dlerror());
    }

    if (fn && fn(1) == 0)
    {
        PLOG_I("PW", "定时关机已设 1s, 待 systime 广播 MSG_SHORTCUT_POWER(200)");
                pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attr, 24 * 1024);
        if (pthread_create(&tid, &attr, shutdown_fallback_thread, NULL) != 0)
            PLOG_W("PW", "fallback 线程创建失败");
        pthread_attr_destroy(&attr);
        return 0;
    }

    PLOG_E("PW", "优雅关机入口不可用, 直接 fallback");
    system("poweroff -f");
    return -1;
}

void platform_power_reboot(void)
{
    /* 原生无重启动画链（03 §8.3 仅关机有）；system("reboot") 即等价路径 */
    system("reboot");
}
