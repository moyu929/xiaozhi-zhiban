/**
 * @file watchdog.c
 * @brief 小智助手看门狗模块实现
 *
 * 本文件实现了看门狗喂狗和系统资源监控功能：
 * - 通过共享内存(app_running_list)找到sair应用信息，定期更新看门狗超时时间戳
 * - 后台线程每10秒喂狗一次，同时监控内存和CPU负载
 * - 内存低于1MB时发出低内存警告
 * - 支持动态设置看门狗超时时间（活跃/空闲不同超时）
 */

#include "watchdog.h"
#include "xiaozhi_config.h"
#include "plog.h"
#include "reverse/applib_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sys/prctl.h>

/* 平台SDK外部接口：软看门狗控制 */
extern int set_soft_watchdog_timeout(int timeout_ms);
extern int sys_forbid_soft_watchdog(int forbid);

/**
 * @brief 在app_running_list共享内存中查找sair应用信息
 *        先按PID查找，再按名称查找，同时提取消息队列名称
 *        sair 是 Assistant 的二进制文件名（平台约束不可改名）
 * @param wd 看门狗结构体指针
 * @return 0成功，-1未找到
 */
static int find_sair_app_info(watchdog_t *wd)
{
    /* 释放之前映射的共享内存 */
    if (wd->shm_ptr)
    {
        munmap(wd->shm_ptr, APP_RUNNING_LIST_SIZE);
        wd->shm_ptr = NULL;
    }
    if (wd->shm_fd >= 0)
    {
        close(wd->shm_fd);
        wd->shm_fd = -1;
    }

    wd->shm_fd = shm_open("app_running_list", O_RDWR, 0);
    if (wd->shm_fd < 0)
        return -1;

    wd->shm_ptr = mmap(NULL, APP_RUNNING_LIST_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, wd->shm_fd, 0);
    if (wd->shm_ptr == MAP_FAILED)
    {
        close(wd->shm_fd);
        wd->shm_fd = -1;
        return -1;
    }

    /* 第一轮：按PID查找（热更新场景下PID不变） */
    int my_pid = getpid();
    char *data = (char *)wd->shm_ptr;

    for (int base = 0x20; base + 0x188 <= APP_RUNNING_LIST_SIZE; base += 0x188)
    {
        char *entry = data + base;
        int entry_pid = *(int *)(entry + 0x08);
        if (entry_pid == my_pid)
        {
            wd->sair_app_info = entry;
            PLOG_I("WD", "通过 pid=%d 找到 sair, 偏移 0x%x", my_pid, base);
            /* 提取消息队列名称 */
            char *mq_ptr = entry + 0x24;
            if (mq_ptr[0] == '/')
            {
                strncpy(wd->mq_name, mq_ptr, sizeof(wd->mq_name) - 1);
                wd->mq_name[sizeof(wd->mq_name) - 1] = '\0';
            }
            return 0;
        }
    }

    /* 第二轮：按名称查找sair（助手二进制文件名） */
    for (int base = 0x20; base + 0x188 <= APP_RUNNING_LIST_SIZE; base += 0x188)
    {
        char *entry = data + base;
        char *name_ptr = entry + 0x0c;
        if (name_ptr[0] != '\0' && strstr(name_ptr, "sair") != NULL)
        {
            wd->sair_app_info = entry;
            PLOG_I("WD", "通过名称找到 sair, 偏移 0x%x pid=%d", base, *(int *)(entry + 0x08));
            char *mq_ptr = entry + 0x24;
            if (mq_ptr[0] == '/')
            {
                strncpy(wd->mq_name, mq_ptr, sizeof(wd->mq_name) - 1);
                wd->mq_name[sizeof(wd->mq_name) - 1] = '\0';
            }
            return 0;
        }
    }

    PLOG_W("WD", "在 app_running_list 中未找到 sair");
    return -1;
}

/**
 * @brief 喂狗操作
 *        更新共享内存中sair应用信息的心跳时间戳
 *        如果未找到sair_app_info则尝试重新查找
 * @param wd 看门狗结构体指针
 */
void watchdog_feed(watchdog_t *wd)
{
    if (!wd->sair_app_info)
    {
        if (find_sair_app_info(wd) != 0)
        {
            wd->feed_fail_count++;
            if (wd->feed_fail_count % 6 == 1)
            {
                PLOG_W("WD", "喂狗失败 (次数=%d), 未找到 sair_app_info", wd->feed_fail_count);
            }
            return;
        }
    }

    if (wd->sair_app_info)
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now_ms = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        uint32_t timeout = wd->timeout_ms > 0 ? wd->timeout_ms : WD_TIMEOUT_ACTIVE;
        /* 更新心跳截止时间戳（偏移0x70处） */
        *(uint64_t *)((char *)wd->sair_app_info + 0x70) = now_ms + timeout;
        __sync_synchronize(); /* 内存屏障，确保写入可见 */
        wd->feed_fail_count = 0;
    }
}

/**
 * @brief 设置看门狗超时时间
 * @param wd 看门狗结构体指针
 * @param timeout_ms 超时时间（毫秒）
 */
void watchdog_set_timeout(watchdog_t *wd, uint32_t timeout_ms)
{
    if (!wd)
        return;
    wd->timeout_ms = timeout_ms;
    PLOG_I("WD", "超时时间设置为 %ums", timeout_ms);
}

/**
 * @brief 看门狗喂狗线程函数
 *        每10秒喂狗一次，每60秒（6个周期）输出一次内存和CPU负载信息
 *        内存低于1MB时输出低内存警告
 * @param arg 看门狗结构体指针
 * @return NULL
 */
static void *watchdog_thread_func(void *arg)
{
    watchdog_t *wd = (watchdog_t *)arg;
    prctl(PR_SET_NAME, "wd_feed");
    PLOG_I("WD", "看门狗喂狗线程已启动");

    int monitor_count = 0;
    while (wd->running)
    {
        watchdog_feed(wd);
        monitor_count++;

        /* 每60秒输出一次系统资源监控信息 */
        if (monitor_count % 6 == 0)
        {
            int mem_free = 0, mem_available = 0, mem_cached = 0;
            FILE *f = fopen("/proc/meminfo", "r");
            if (f)
            {
                char line[256];
                while (fgets(line, sizeof(line), f))
                {
                    if (strncmp(line, "MemFree:", 8) == 0)
                        sscanf(line + 8, " %d", &mem_free);
                    else if (strncmp(line, "MemAvailable:", 13) == 0)
                        sscanf(line + 13, " %d", &mem_available);
                    else if (strncmp(line, "Cached:", 7) == 0)
                        sscanf(line + 7, " %d", &mem_cached);
                }
                fclose(f);
            }

            /* 读取进程自身内存使用 */
            int vsz = 0, rss = 0;
            f = fopen("/proc/self/status", "r");
            if (f)
            {
                char line[256];
                while (fgets(line, sizeof(line), f))
                {
                    if (strncmp(line, "VmSize:", 7) == 0)
                        sscanf(line + 7, " %d", &vsz);
                    else if (strncmp(line, "VmRSS:", 6) == 0)
                        sscanf(line + 6, " %d", &rss);
                }
                fclose(f);
            }

            PLOG_I("MEM", "MemFree=%dKB Cached=%dKB VSZ=%dKB RSS=%dKB",
                   mem_free, mem_cached, vsz, rss);

            /* 低内存警告 */
            if (mem_free > 0 && mem_free < 1024)
            {
                PLOG_W("MEM", "内存不足: MemFree=%dKB < 1024KB!", mem_free);
            }

            /* 读取CPU负载 */
            f = fopen("/proc/loadavg", "r");
            if (f)
            {
                char load_buf[128];
                if (fgets(load_buf, sizeof(load_buf), f))
                {
                    load_buf[strcspn(load_buf, "\n")] = 0;
                    PLOG_I("CPU", "负载均值: %s", load_buf);
                }
                fclose(f);
            }
        }

        sleep(10);
    }

    PLOG_I("WD", "看门狗喂狗线程已退出");
    return NULL;
}

/**
 * @brief 初始化看门狗模块
 *        查找sair应用信息，创建喂狗线程
 * @param wd 看门狗结构体指针
 * @return 0成功，-1参数无效或线程创建失败
 */
int watchdog_init(watchdog_t *wd)
{
    if (!wd)
        return -1;
    memset(wd, 0, sizeof(watchdog_t));
    wd->shm_fd = -1;
    wd->running = 1;
    wd->timeout_ms = WD_TIMEOUT_ACTIVE;
    wd->feed_fail_count = 0;
    strncpy(wd->mq_name, "/sair_mq", sizeof(wd->mq_name) - 1); /* sair 是助手二进制文件名 */

    find_sair_app_info(wd);

    /* 创建喂狗线程（joinable，以便安全等待退出） */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 32 * 1024);
    int ret = pthread_create(&wd->thread, &attr, watchdog_thread_func, wd);
    pthread_attr_destroy(&attr);

    if (ret != 0)
    {
        PLOG_E("WD", "创建看门狗线程失败: %d", ret);
        wd->running = 0;
        return -1;
    }
    wd->thread_created = 1;

    {
        struct sched_param sp;
        sp.sched_priority = 6;
        if (pthread_setschedparam(wd->thread, SCHED_RR, &sp) == 0)
            PLOG_I("WD", "喂狗线程已设置 SCHED_RR 优先级 6");
        else
            PLOG_W("WD", "喂狗线程设置调度策略失败");
    }

    PLOG_I("WD", "看门狗线程已启动");
    return 0;
}

/**
 * @brief 销毁看门狗模块
 *        停止喂狗线程，释放共享内存映射
 * @param wd 看门狗结构体指针
 */
void watchdog_destroy(watchdog_t *wd)
{
    if (!wd)
        return;
    wd->running = 0;
    if (wd->thread_created)
    {
        pthread_join(wd->thread, NULL);
        wd->thread_created = 0;
    }
    if (wd->shm_ptr)
    {
        munmap(wd->shm_ptr, APP_RUNNING_LIST_SIZE);
        wd->shm_ptr = NULL;
        wd->sair_app_info = NULL;
    }
    if (wd->shm_fd >= 0)
    {
        close(wd->shm_fd);
        wd->shm_fd = -1;
    }
}
