/**
 * @file platform_power.c
 * @brief 平台电源/音效封装实现（白名单隔离层，见 platform_power.h）
 */
#include "platform_power.h"
#include "plog.h"
#include "reverse/applib_api.h" /* broadcast_msg */

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

/* ---- 优雅关机 ---- */

/**
 * 关机兜底线程：给原生动画链 12s 时窗(关机动画 + manager 杀应用)。
 * 正常关机会经由 manager 杀死本进程，该线程根本活不到执行 system()；
 * 一旦活着执行到说明优雅链路失效，硬关兜底。时窗不宜过短，否则会
 * 在动画播放中途 poweroff -f 打断动画(实测教训)。
 */
static void *shutdown_fallback_thread(void *arg)
{
    (void)arg;
    sleep(12);
    PLOG_W("PW", "优雅关机 12s 未生效, fallback poweroff -f");
    system("poweroff -f");
    return NULL;
}

/* 广播 MSG_SHORTCUT_POWER(63) 子码 200(定时关机到期) —— 与 libsystime
 * 定时关机到期时 __systime_broadcast_msg2(63,200) 完全同一条消息
 * (P3 §5: launcher 订阅后进入关机场景播 shut_down.swf 动画)。
 * 弃 systime_set_timed_shutdown_time 方案: 该 setter 会写 SLEEP_TIME
 * 配置并落盘, 原生 App(mqtt_custom_server/setting) 设定时关机时伴随
 * "定时关机"弹窗确认流程, 实测走此路径关机前会误弹定时关机 UI
 * (2026-08-30 实机实测); 直发广播零配置副作用。 */
int platform_power_shutdown_elegant(void)
{
    char msg[8];
    memset(msg, 0, sizeof(msg));
    *(int *)msg = 63;   /* MSG_SHORTCUT_POWER */
    *(int *)(msg + 4) = 200; /* 子码: 定时关机到期(非 201 待机) */
    int ret = broadcast_msg(msg);
    PLOG_I("PW", "已广播 MSG_SHORTCUT_POWER(63,200), 待 launcher 关机场景 (ret=%d)", ret);

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

void platform_power_reboot(void)
{
    /* 原生无重启动画链（03 §8.3 仅关机有）；system("reboot") 即等价路径 */
    system("reboot");
}
