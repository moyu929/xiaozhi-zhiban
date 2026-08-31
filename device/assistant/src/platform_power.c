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

/* ---- libmusic_player_api: 本地音频文件播放 (原生 music_player 服务) ----
 * ABI 依据 M2 §5.1/§11.5 + alarm_play.so 调用序列印证:
 *   mp_open() 无参返回 handle (内部 start_service("music_player")+RPC 1000)
 *   mp_set_file(handle, url) / mp_play / mp_stop / mp_close(handle)
 *   mp_get_cur_time / mp_get_total_time 返回值即秒
 * 红线: 只经库函数调用, 绝不发裸 RPC(1001 close 裸发曾致服务死亡→整机重启) */

typedef void *(*mp_open_fn)(void);
typedef int (*mp_handle_fn)(void *);
typedef int (*mp_set_file_fn)(void *, const char *);
typedef int (*mp_time_fn)(void *);

/* 提示音播放中标志: media_play_thread 存续期置位, use_limit 据此
 * 在播放期+播后冷却窗内静默拦截唤醒(提示音人声被 ASR 误判自我触发) */
static volatile int g_media_playing = 0;

int platform_media_is_playing(void)
{
    return g_media_playing;
}

static void *media_play_thread(void *arg)
{
    char *path = (char *)arg;
    g_media_playing = 1;

    void *h = dlopen("libmusic_player_api.so", RTLD_NOW);
    if (!h)
    {
        PLOG_W("PW", "libmusic_player_api 加载失败: %s", dlerror());
        g_media_playing = 0;
        free(path);
        return NULL;
    }
    mp_open_fn f_open = (mp_open_fn)dlsym(h, "mp_open");
    mp_set_file_fn f_set = (mp_set_file_fn)dlsym(h, "mp_set_file");
    mp_handle_fn f_play = (mp_handle_fn)dlsym(h, "mp_play");
    mp_handle_fn f_stop = (mp_handle_fn)dlsym(h, "mp_stop");
    mp_handle_fn f_close = (mp_handle_fn)dlsym(h, "mp_close");
    mp_time_fn f_cur = (mp_time_fn)dlsym(h, "mp_get_cur_time");
    mp_time_fn f_total = (mp_time_fn)dlsym(h, "mp_get_total_time");
    if (!f_open || !f_set || !f_play || !f_stop || !f_close)
    {
        PLOG_W("PW", "music_player API 符号缺失");
        g_media_playing = 0;
        free(path);
        return NULL;
    }

    void *mp = f_open();
    if (!mp)
    {
        PLOG_W("PW", "mp_open 失败: %s", path);
        g_media_playing = 0;
        free(path);
        return NULL;
    }
    /* music_player URL 校验要求 file:// 前缀(M2 §5.3: check_url 家族) */
    char url[1100];
    snprintf(url, sizeof(url), "file://%s", path);
    /* 返回值语义(反汇编 0xa70): set_file 成功返回 strlen(url)(>0), 失败 -1;
     * play/stop 失败返回负数 */
    if (f_set(mp, url) < 0 || f_play(mp) < 0)
    {
        PLOG_W("PW", "mp_set_file/mp_play 失败: %s", path);
        f_close(mp);
        g_media_playing = 0;
        free(path);
        return NULL;
    }
    PLOG_I("PW", "播放提示音: %s", path);

    /* 轮询播完(纯停滞检测, 30s 超时兜底):
     * 不用 cur>=total 判定——music_player 对 16kHz 单声道 mp3 报的
     * total 偏短(实测 4.0s 文件报 ~3.5s), 到 total 即 stop 会截掉
     * 末尾 ~0.5s, 末字戛然而止(2026-08-31 实测事故). 改等播放位置
     * 真正停滞(播完引擎归位): cur 连续 3 轮(1.5s)不变视为播完.
     * 启动期(cur 尚为 0)不许停滞退出, 防未播先停 */
    int prev_cur = -1;
    int stagnant = 0;
    for (int i = 0; i < 60; i++)
    {
        usleep(500 * 1000);
        if (!f_cur)
            continue;
        int cur = f_cur(mp);
        stagnant = (cur == prev_cur) ? stagnant + 1 : 0;
        prev_cur = cur;
        if (stagnant >= 3 && (cur > 0 || i >= 6))
            break;
    }
    f_stop(mp);
    f_close(mp); /* 内含 stop_service, music_player 进程随之退出 */
    g_media_playing = 0;
    PLOG_I("PW", "提示音播完: %s", path);
    free(path);
    return NULL;
}

int platform_media_play_file(const char *path)
{
    if (!path || access(path, R_OK) != 0)
        return -1;

    char *p = strdup(path);
    if (!p)
        return -1;

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 32 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&tid, &attr, media_play_thread, p) != 0)
    {
        free(p);
        pthread_attr_destroy(&attr);
        return -1;
    }
    pthread_attr_destroy(&attr);
    return 0;
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

/* 广播 MSG_SHORTCUT_POWER(63) 子码 200 —— 定时关机到期消息
 * (libsystime __systime_broadcast_msg2(63,200) 同款, P3 §5).
 * launcher 收到后: 先弹"定时关机"提示 msgbox(libcommonui
 * msgbox_proc_sys 0x1aeb8: msg63/子码200 → msgbox(40,5,7), 弹窗为
 * 定时关机链固有 UI, 无法从 sair 侧消除), 随后进关机场景播
 * shut_down.swf 动画, manager 杀应用后断电.
 * 已试路线对比(2026-08-30 实机):
 *   systime_set_timed_shutdown_time(1): 同样弹窗+动画(殊途同归 63,200)
 *   send_async_msg("launcher",[113]): 无弹窗但无动画(直接关)
 * 用户决策: 接受弹窗换取原生动画, 观感代价记录在案. */
int platform_power_shutdown_elegant(void)
{
    char msg[8];
    memset(msg, 0, sizeof(msg));
    *(int *)msg = 63;        /* MSG_SHORTCUT_POWER */
    *(int *)(msg + 4) = 200; /* 子码: 定时关机到期 */
    int ret = broadcast_msg(msg);
    PLOG_I("PW", "已广播 MSG_SHORTCUT_POWER(63,200), 弹窗+动画+关机 (ret=%d)", ret);

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
