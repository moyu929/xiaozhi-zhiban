/**
 * @file touch_key.c
 * @brief 触摸按键驱动模块
 *
 * 本模块负责监听Goodix触摸屏的按键事件（HOME键和BACK键），
 * 通过独立线程读取Linux输入设备节点(/dev/input/event2)，
 * 检测到按键按下时触发上层回调通知。
 */

#include "touch_key.h"
#include "xiaozhi_config.h"
#include "plog.h"
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <sys/prctl.h>
#include <linux/input.h>

#ifndef PR_SET_NAME
#define PR_SET_NAME 15
#endif

/**
 * @brief 触摸按键监听线程函数
 *
 * 在独立线程中运行，负责：
 * 1. 打开触摸屏设备节点（带重试机制）
 * 2. 循环读取输入事件
 * 3. 过滤出HOME/BACK按键按下事件并触发回调
 *
 * @param arg 触摸按键模块实例指针(touch_key_t*)
 * @return 线程返回值（始终为NULL）
 */
static void *touchkey_thread_func(void *arg)
{
    touch_key_t *tk = (touch_key_t *)arg;
    struct input_event ev;

    /* 设置线程名称，便于调试识别 */
    prctl(PR_SET_NAME, "touch_key");

    /* 尝试打开触摸屏设备节点，失败时重试 */
    while (tk->running && tk->fd < 0)
    {
        tk->fd = open("/dev/input/event2", O_RDONLY | O_NONBLOCK);
        if (tk->fd >= 0)
        {
            PLOG_I("KEY", "触摸按键设备已打开 (event2=goodix-ts)");
            break;
        }
        PLOG_D("KEY", "等待 /dev/input/event2 (errno=%d)", errno);
        /* 等待1秒后重试 */
        for (int i = 0; i < 10 && tk->running; i++)
        {
            usleep(100000);
        }
    }

    /* 主事件循环：读取并处理按键事件 */
    while (tk->running)
    {
        int n = read(tk->fd, &ev, sizeof(ev));
        if (n < 0)
        {
            /* 非阻塞模式下无数据可读，正常情况 */
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                usleep(10000);
                continue;
            }
            PLOG_E("KEY", "读取错误: %d", errno);
            break;
        }
        /* 数据不完整，跳过 */
        if (n != sizeof(ev))
            continue;

        /* 过滤按键按下事件（value=1表示按下） */
        if (ev.type == EV_KEY && ev.value == 1)
        {
            if (ev.code == GOODIX_KEY_HOME || ev.code == GOODIX_KEY_BACK)
            {
                PLOG_I("KEY", "按键按下: code=%d", ev.code);
                tk->pending_key = ev.code;
                /* 触发上层按键回调 */
                if (tk->on_key)
                {
                    tk->on_key(ev.code, tk->user_data);
                }
            }
        }
    }

    return NULL;
}

/**
 * @brief 初始化触摸按键模块
 *
 * 初始化模块结构体，创建按键监听线程（栈大小64KB）。
 * 设备节点在子线程中延迟打开，避免阻塞初始化过程。
 *
 * @param tk        触摸按键模块实例
 * @param on_key    按键回调函数
 * @param user_data 回调用户数据
 * @return 0=成功，-1=失败
 */
int touch_key_init(touch_key_t *tk,
                   void (*on_key)(int key_code, void *user_data),
                   void *user_data)
{
    if (!tk)
        return -1;
    memset(tk, 0, sizeof(touch_key_t));

    tk->on_key = on_key;
    tk->user_data = user_data;
    tk->fd = -1;
    tk->running = 1;

    /* 创建监听线程，设置64KB栈大小 */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64 * 1024);
    int ret = pthread_create(&tk->thread, &attr, touchkey_thread_func, tk);
    pthread_attr_destroy(&attr);

    if (ret != 0)
    {
        PLOG_E("KEY", "创建线程失败: %d", ret);
        tk->running = 0;
        return -1;
    }
    tk->thread_created = 1;

    PLOG_I("KEY", "触摸按键线程已启动 (延迟打开设备)");
    return 0;
}

/**
 * @brief 销毁触摸按键模块
 *
 * 停止监听线程、关闭设备节点、等待线程退出。
 *
 * @param tk 触摸按键模块实例
 */
void touch_key_destroy(touch_key_t *tk)
{
    if (!tk)
        return;
    tk->running = 0;
    /* 关闭设备节点，使read()返回错误以退出线程循环 */
    if (tk->fd >= 0)
    {
        close(tk->fd);
        tk->fd = -1;
    }
    /* 等待线程退出 */
    if (tk->thread_created)
    {
        pthread_join(tk->thread, NULL);
        tk->thread_created = 0;
    }
    PLOG_I("KEY", "触摸按键线程已停止");
}
