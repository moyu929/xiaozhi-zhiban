/**
 * @file wakeup_module.c
 * @brief 唤醒词检测模块
 *
 * 本模块负责语音唤醒词的检测与处理，主要功能包括：
 * - 加载ASR（自动语音识别）引擎动态库并初始化
 * - 配置唤醒词、命令词及其阈值
 * - 管理AEC（回声消除）、VAD（语音活动检测）、UDA等资源
 * - 通过音频分发器接收音频数据并送入ASR引擎
 * - 检测到唤醒词时触发回调通知上层
 */

#include "wakeup_module.h"
#include "xiaozhi_config.h"
#include "reverse/sair_asr_api.h"
#include "reverse/applib_api.h"
#include "plog.h"
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

/* 产品ID，用于DUI平台识别 */
static char g_product_id[64] = {0};
/* 设备ID */
static char g_device_id[64] = {0};
/* 主唤醒词（拼音形式） */
static char g_main_wake_word[64] = {0};
/* 主唤醒词检测阈值 */
static char g_main_wake_thresh[16] = {0};
/* 自定义唤醒词 */
static char g_custom_wake_word[64] = {0};
/* 自定义唤醒词检测阈值 */
static char g_custom_wake_thresh[16] = {0};
/* 本地命令词 */
static char g_local_command_word[64] = {0};
/* 本地命令词检测阈值 */
static char g_local_command_thresh[16] = {0};
/* 唤醒模型资源路径 */
static char g_wakeup_res_path[256] = {0};
/* VAD（语音活动检测）资源路径 */
static char g_vad_res_path[256] = {0};
/* AEC（回声消除）资源路径 */
static char g_aec_res_path[256] = {0};
/* UDA（通用数据采集）资源路径 */
static char g_uda_res_path[256] = {0};
/* 主配置文件路径 */
static char g_major_config[256] = {0};
/* 双重检测配置文件路径 */
static char g_dcheck_config[256] = {0};
/* 商城模式标志 */
static int g_malls_mode = 0;

/* 全局唤醒模块实例指针，供ASR回调使用（volatile防止编译器优化） */
static volatile wakeup_module_t *g_wakeup_mod = NULL;

/**
 * @brief 解析资源文件路径
 *
 * 如果路径不是绝对路径，依次在升级目录和默认资源目录中查找，
 * 将路径转换为实际可访问的绝对路径。
 *
 * @param path      资源路径缓冲区（输入/输出）
 * @param path_size 缓冲区大小
 * @return 0=成功，-1=资源文件未找到
 */
static int resolve_resource_path(char *path, int path_size)
{
    char temp[512];

    /* 已经是绝对路径，直接返回 */
    if (path[0] == '/')
    {
        return 0;
    }

    /* 优先从升级目录查找 */
    snprintf(temp, sizeof(temp), "/var/upgrade/%s", path);
    if (access(temp, R_OK) == 0)
    {
        strncpy(path, temp, path_size - 1);
        path[path_size - 1] = '\0';
        return 0;
    }

    /* 其次从默认资源目录查找 */
    snprintf(temp, sizeof(temp), "/usr/local/resource/duilite/%s", path);
    if (access(temp, R_OK) == 0)
    {
        strncpy(path, temp, path_size - 1);
        path[path_size - 1] = '\0';
        return 0;
    }

    PLOG_W("WAKEUP", "资源文件未找到: %s", path);
    return -1;
}

/**
 * @brief 加载唤醒模块配置
 *
 * 从系统配置中读取产品ID、唤醒词、阈值、资源路径等参数，
 * 若配置缺失则使用默认值，并验证关键资源文件是否存在。
 *
 * @return 0=成功，-1=关键资源缺失
 */
static int load_config(void)
{
    int ret;

    /* 读取产品ID，缺失时使用默认值 */
    ret = get_config("DUI_AI_PRODUCT_ID", g_product_id, sizeof(g_product_id));
    if (ret < 0 || g_product_id[0] == '\0')
    {
        strncpy(g_product_id, "278577032", sizeof(g_product_id) - 1);
    }

    /* 读取设备ID */
    ret = get_config("DUI_AI_DEVICE_ID", g_device_id, sizeof(g_device_id));
    (void)ret;

    /* 读取主唤醒词，缺失时使用默认唤醒词集合 */
    ret = get_config("MAIN_WAKE_WORD", g_main_wake_word, sizeof(g_main_wake_word));
    if (ret < 0 || g_main_wake_word[0] == '\0')
    {
        strncpy(g_main_wake_word,
                "zhi ban zhi ban,sheng yin da yi dian,sheng yin xiao yi dian,"
                "ting zhi bo fang,ji xu bo fang,shang yi shou,xia yi shou,ni hao zhi dao",
                sizeof(g_main_wake_word) - 1);
    }

    /* 读取主唤醒词阈值，缺失时使用默认0.5 */
    ret = get_config("MAIN_WAKE_THRESH", g_main_wake_thresh, sizeof(g_main_wake_thresh));
    if (ret < 0 || g_main_wake_thresh[0] == '\0')
    {
        strncpy(g_main_wake_thresh, "0.5", sizeof(g_main_wake_thresh) - 1);
    }

    /* 读取可选的自定义唤醒词和本地命令词配置 */
    get_config("CUSTOM_WAKE_WORD", g_custom_wake_word, sizeof(g_custom_wake_word));
    get_config("CUSTOM_WAKE_THRESH", g_custom_wake_thresh, sizeof(g_custom_wake_thresh));
    get_config("LOCAL_COMMAND_WORD", g_local_command_word, sizeof(g_local_command_word));
    get_config("LOCAL_COMMAND_THRESH", g_local_command_thresh, sizeof(g_local_command_thresh));

    /* 读取并解析唤醒模型资源路径 */
    ret = get_config("DUILITE_WAKEUP_RES", g_wakeup_res_path, sizeof(g_wakeup_res_path));
    if (ret < 0 || g_wakeup_res_path[0] == '\0')
    {
        strncpy(g_wakeup_res_path, "/usr/local/resource/duilite/wakeup.bin", sizeof(g_wakeup_res_path) - 1);
    }
    resolve_resource_path(g_wakeup_res_path, sizeof(g_wakeup_res_path));

    /* 读取并解析VAD资源路径 */
    ret = get_config("DUILITE_VAD_RES", g_vad_res_path, sizeof(g_vad_res_path));
    if (ret < 0 || g_vad_res_path[0] == '\0')
    {
        strncpy(g_vad_res_path, "/usr/local/resource/duilite/vad_aihome_v0.9b.bin", sizeof(g_vad_res_path) - 1);
    }
    resolve_resource_path(g_vad_res_path, sizeof(g_vad_res_path));

    /* 读取并解析本地AEC资源路径（唤醒词检测的回声消除前处理，非云端AEC） */
    ret = get_config("DUILITE_AEC_RES", g_aec_res_path, sizeof(g_aec_res_path));
    if (ret < 0 || g_aec_res_path[0] == '\0')
    {
        strncpy(g_aec_res_path,
                "/usr/local/resource/duilite/AEC_ch3-2-ch2_1ref_common_20181226_v0.9.4.bin",
                sizeof(g_aec_res_path) - 1);
    }
    resolve_resource_path(g_aec_res_path, sizeof(g_aec_res_path));

    /* 读取并解析UDA资源路径 */
    ret = get_config("DUILITE_UDA_RES", g_uda_res_path, sizeof(g_uda_res_path));
    if (ret < 0 || g_uda_res_path[0] == '\0')
    {
        strncpy(g_uda_res_path,
                "/usr/local/resource/duilite/UDA_asr_ch2_2_ch2_40mm_20181226_v1.1.0.8_wkppost1_asrpost0_v2.bin",
                sizeof(g_uda_res_path) - 1);
    }
    resolve_resource_path(g_uda_res_path, sizeof(g_uda_res_path));

    /* 检查唤醒模型文件是否存在，这是关键资源 */
    if (access(g_wakeup_res_path, F_OK) != 0)
    {
        PLOG_E("WAKEUP", "唤醒模型文件未找到: %s", g_wakeup_res_path);
        return -1;
    }

    PLOG_I("WAKEUP", "产品ID: %s", g_product_id);
    PLOG_I("WAKEUP", "唤醒模型: %s", g_wakeup_res_path);
    PLOG_I("WAKEUP", "VAD资源: %s", g_vad_res_path);
    PLOG_I("WAKEUP", "AEC资源: %s", g_aec_res_path);
    PLOG_I("WAKEUP", "UDA资源: %s", g_uda_res_path);
    return 0;
}

/**
 * @brief ASR引擎事件回调函数
 *
 * 处理ASR引擎的各类事件通知，包括初始化完成、唤醒检测、
 * VAD状态变化等，其中唤醒事件会触发上层回调。
 *
 * @param event_type 事件类型（ASR_EVENT_INIT_DONE等）
 * @param result     事件结果值
 */
static void asr_callback(int event_type, int result)
{
    volatile wakeup_module_t *mod = g_wakeup_mod;
    if (!mod || !mod->initialized)
        return;

    switch (event_type)
    {
    case ASR_EVENT_INIT_DONE:
        PLOG_I("ASR", "初始化完成: result=%d", result);
        mod->asr_init_done = 1;
        break;
    case ASR_EVENT_WAKEUP:
        PLOG_I("ASR", "检测到唤醒! type=%d", result);
        if (mod->on_wakeup)
        {
            mod->on_wakeup(result, mod->wakeup_user_data);
        }
        break;
    case ASR_EVENT_DIFF_WORD:
        PLOG_D("ASR", "差异化唤醒词: result=%d", result);
        break;
    case ASR_EVENT_VAD_CHANGE:
        PLOG_D("ASR", "VAD状态变化: result=%d", result);
        break;
    case ASR_EVENT_VAD_END:
        PLOG_D("ASR", "VAD结束");
        break;
    case ASR_EVENT_VAD_TIMEOUT:
        PLOG_D("ASR", "VAD超时");
        break;
    default:
        PLOG_D("ASR", "未知事件: type=%d result=%d", event_type, result);
        break;
    }
}

/**
 * @brief 唤醒模块音频数据回调
 *
 * 由音频分发器调用，将音频数据送入ASR引擎进行唤醒词检测。
 * 当模块暂停、未启动或ASR句柄无效时跳过处理。
 *
 * @param data      音频数据（16位PCM）
 * @param len       采样点数
 * @param user_data 用户数据（wakeup_module_t指针）
 */
static void wakeup_audio_callback(const int16_t *data, int len, void *user_data)
{
    wakeup_module_t *mod = (wakeup_module_t *)user_data;
    if (!mod || mod->paused || !mod->started || !mod->asr_handle)
        return;
    if (mod->f_asr_feed_data)
    {
        mod->f_asr_feed_data(mod->asr_handle, data, len * 2);
    }
}

/**
 * @brief 初始化唤醒模块
 *
 * 完成配置加载、动态库加载、ASR引擎初始化和音频回调注册。
 * 依次加载libalooper.so、libstream_source.so、libsair_asr.so，
 * 解析ASR引擎函数符号，设置参数并初始化引擎。
 *
 * @param mod       唤醒模块实例
 * @param disp      音频分发器实例（可为NULL）
 * @param on_wakeup 唤醒回调函数
 * @param user_data 回调用户数据
 * @return 0=成功，-1=失败
 */
int wakeup_init(wakeup_module_t *mod, audio_dispatcher_t *disp,
                void (*on_wakeup)(int wakeup_type, void *user_data),
                void *user_data)
{
    if (!mod)
        return -1;
    memset(mod, 0, sizeof(wakeup_module_t));

    mod->dispatcher = disp;
    mod->on_wakeup = on_wakeup;
    mod->wakeup_user_data = user_data;

    /* 加载配置，资源缺失时仅警告不中断 */
    if (load_config() < 0)
    {
        PLOG_W("WAKEUP", "加载配置失败, 部分资源可能缺失");
    }

    /* 加载libalooper.so依赖库（非致命） */
    void *alooper_lib = dlopen("libalooper.so", RTLD_NOW | RTLD_GLOBAL);
    if (!alooper_lib)
    {
        PLOG_W("WAKEUP", "dlopen libalooper.so失败: %s (非致命)", dlerror());
    }

    /* 加载libstream_source.so依赖库（非致命） */
    void *stream_lib = dlopen("libstream_source.so", RTLD_NOW | RTLD_GLOBAL);
    if (!stream_lib)
    {
        PLOG_W("WAKEUP", "dlopen libstream_source.so失败: %s (非致命)", dlerror());
    }

    /* 预加载libdds.so（libsair_asr.so的运行时依赖） */
    void *dds_lib = dlopen("libdds.so", RTLD_NOW | RTLD_GLOBAL);
    if (!dds_lib)
    {
        PLOG_W("WAKEUP", "dlopen libdds.so失败: %s", dlerror());
    }

    /* 加载ASR引擎核心动态库（致命错误） */
    mod->asr_lib = dlopen("libsair_asr.so", RTLD_NOW | RTLD_GLOBAL);
    if (!mod->asr_lib)
    {
        PLOG_E("WAKEUP", "dlopen libsair_asr.so失败: %s", dlerror());
        return -1;
    }

    /* 解析ASR引擎函数符号 */
    mod->f_asr_set_params = (asr_set_params_t)dlsym(mod->asr_lib, "asr_engine_set_params");
    mod->f_asr_init = (asr_init_t)dlsym(mod->asr_lib, "asr_engine_init");
    mod->f_asr_finalize = (asr_finalize_t)dlsym(mod->asr_lib, "asr_engine_finalize");
    mod->f_asr_start = (asr_start_t)dlsym(mod->asr_lib, "asr_engine_start");
    mod->f_asr_stop = (asr_stop_t)dlsym(mod->asr_lib, "asr_engine_stop");
    mod->f_asr_feed_data = (asr_feed_data_t)dlsym(mod->asr_lib, "asr_engine_feed_data");

    /* 检查所有必要符号是否解析成功 */
    if (!mod->f_asr_set_params || !mod->f_asr_init || !mod->f_asr_start ||
        !mod->f_asr_feed_data || !mod->f_asr_stop || !mod->f_asr_finalize)
    {
        PLOG_E("WAKEUP", "ASR引擎函数符号解析失败");
        dlclose(mod->asr_lib);
        mod->asr_lib = NULL;
        return -1;
    }

    PLOG_I("WAKEUP", "ASR引擎符号加载完成");

    /* 设置ASR引擎参数：通道数4，注册回调 */
    if (mod->f_asr_set_params(0, 4, (void *)asr_callback) != 0)
    {
        PLOG_E("WAKEUP", "asr_engine_set_params失败");
        dlclose(mod->asr_lib);
        mod->asr_lib = NULL;
        return -1;
    }
    PLOG_I("WAKEUP", "ASR回调注册完成");

    /* 构建ASR配置并初始化引擎 */
    asr_config_t asr_cfg;
    memset(&asr_cfg, 0, sizeof(asr_cfg));
    asr_cfg.sample_rate = 16000;
    asr_cfg.channels = 3;
    strncpy(asr_cfg.product_id, g_product_id, sizeof(asr_cfg.product_id) - 1);
    strncpy(asr_cfg.device_id, g_device_id, sizeof(asr_cfg.device_id) - 1);
    strncpy(asr_cfg.main_wake_word, g_main_wake_word, sizeof(asr_cfg.main_wake_word) - 1);
    strncpy(asr_cfg.main_wake_thresh, g_main_wake_thresh, sizeof(asr_cfg.main_wake_thresh) - 1);
    /* 可选：自定义唤醒词 */
    if (g_custom_wake_word[0])
    {
        strncpy(asr_cfg.custom_wake_word, g_custom_wake_word, sizeof(asr_cfg.custom_wake_word) - 1);
        strncpy(asr_cfg.custom_wake_thresh, g_custom_wake_thresh, sizeof(asr_cfg.custom_wake_thresh) - 1);
    }
    /* 可选：本地命令词 */
    if (g_local_command_word[0])
    {
        strncpy(asr_cfg.local_command_word, g_local_command_word, sizeof(asr_cfg.local_command_word) - 1);
        strncpy(asr_cfg.local_command_thresh, g_local_command_thresh, sizeof(asr_cfg.local_command_thresh) - 1);
    }
    strncpy(asr_cfg.aec_res_path, g_aec_res_path, sizeof(asr_cfg.aec_res_path) - 1);
    strncpy(asr_cfg.wakeup_res_path, g_wakeup_res_path, sizeof(asr_cfg.wakeup_res_path) - 1);
    strncpy(asr_cfg.uda_res_path, g_uda_res_path, sizeof(asr_cfg.uda_res_path) - 1);
    strncpy(asr_cfg.vad_res_path, g_vad_res_path, sizeof(asr_cfg.vad_res_path) - 1);
    strncpy(asr_cfg.major_config, g_major_config, sizeof(asr_cfg.major_config) - 1);
    strncpy(asr_cfg.dcheck_config, g_dcheck_config, sizeof(asr_cfg.dcheck_config) - 1);
    asr_cfg.malls_mode = g_malls_mode;

    mod->asr_handle = mod->f_asr_init(&asr_cfg);
    if (!mod->asr_handle)
    {
        PLOG_E("WAKEUP", "asr_engine_init失败");
        dlclose(mod->asr_lib);
        mod->asr_lib = NULL;
        return -1;
    }
    PLOG_I("WAKEUP", "asr_engine_init成功, handle=%p", mod->asr_handle);

    mod->initialized = 1;
    g_wakeup_mod = mod;

    /* 注册音频分发器回调，接收音频数据 */
    if (disp)
    {
        audio_dispatcher_register(disp, wakeup_audio_callback, mod);
        PLOG_I("WAKEUP", "已注册音频分发器回调");
    }

    return 0;
}

/**
 * @brief 启动唤醒词检测
 *
 * 等待ASR引擎初始化完成（最多5秒），然后启动ASR引擎开始检测。
 * 若ASR初始化超时，仍尝试启动。
 *
 * @param mod 唤醒模块实例
 * @return 0=成功，-1=失败
 */
int wakeup_start(wakeup_module_t *mod)
{
    if (!mod || !mod->initialized)
        return -1;
    if (mod->started)
        return 0;

    /* 等待ASR引擎初始化完成通知 */
    if (!mod->asr_init_done)
    {
        PLOG_I("WAKEUP", "等待ASR初始化完成...");
        int wait_count = 0;
        while (!mod->asr_init_done && wait_count < 50)
        {
            usleep(100000);
            wait_count++;
        }
        if (!mod->asr_init_done)
        {
            PLOG_W("WAKEUP", "ASR init_done超时(5秒), 仍然尝试启动 (DUI认证可能未完成但唤醒词仍可用)");
        }
        else
        {
            PLOG_I("WAKEUP", "ASR初始化完成, 等待耗时%dms", wait_count * 100);
        }
    }

    /* 启动ASR引擎 */
    if (mod->asr_handle && mod->f_asr_start)
    {
        int ret = mod->f_asr_start(mod->asr_handle);
        if (ret != 0)
        {
            PLOG_E("WAKEUP", "asr_engine_start失败: %d", ret);
            return -1;
        }
    }

    mod->started = 1;
    mod->paused = 0;
    PLOG_I("WAKEUP", "唤醒检测已启动");
    return 0;
}

/**
 * @brief 停止唤醒词检测
 *
 * 停止ASR引擎，暂停音频数据输入。
 *
 * @param mod 唤醒模块实例
 */
void wakeup_stop(wakeup_module_t *mod)
{
    if (!mod || !mod->started)
        return;
    if (mod->asr_handle && mod->f_asr_stop)
    {
        mod->f_asr_stop(mod->asr_handle);
    }
    mod->started = 0;
    mod->paused = 0;
    PLOG_I("WAKEUP", "唤醒检测已停止");
}

/**
 * @brief 关闭唤醒模块
 *
 * 停止检测并释放ASR引擎资源，但保留动态库句柄。
 *
 * @param mod 唤醒模块实例
 */
void wakeup_close(wakeup_module_t *mod)
{
    if (!mod)
        return;
    if (mod->started)
    {
        wakeup_stop(mod);
    }
    if (mod->asr_handle && mod->f_asr_finalize)
    {
        mod->f_asr_finalize(mod->asr_handle);
    }
    mod->asr_handle = NULL;
    mod->initialized = 0;
    PLOG_I("WAKEUP", "唤醒模块已关闭");
}

/**
 * @brief 暂停音频数据输入
 *
 * 暂停后音频回调将跳过数据送入ASR引擎，但引擎仍在运行。
 *
 * @param mod 唤醒模块实例
 */
void wakeup_pause_feed(wakeup_module_t *mod)
{
    if (!mod)
        return;
    mod->paused = 1;
    PLOG_D("WAKEUP", "音频输入已暂停");
}

/**
 * @brief 恢复音频数据输入
 *
 * @param mod 唤醒模块实例
 */
void wakeup_resume_feed(wakeup_module_t *mod)
{
    if (!mod)
        return;
    mod->paused = 0;
    PLOG_D("WAKEUP", "音频输入已恢复");
}

/**
 * @brief 检查音频数据输入是否活跃
 *
 * @param mod 唤醒模块实例
 * @return 1=活跃（已启动且未暂停），0=不活跃
 */
int wakeup_is_feed_active(wakeup_module_t *mod)
{
    if (!mod)
        return 0;
    return mod->started && !mod->paused;
}

/**
 * @brief 销毁唤醒模块
 *
 * 注销音频回调、关闭ASR引擎、卸载动态库，彻底释放所有资源。
 *
 * @param mod 唤醒模块实例
 */
void wakeup_destroy(wakeup_module_t *mod)
{
    if (!mod)
        return;
    /* 先置空全局指针，防止ASR回调访问正在销毁的模块 */
    g_wakeup_mod = NULL;
    mod->initialized = 0;
    /* 注销音频分发器回调 */
    if (mod->dispatcher)
    {
        audio_dispatcher_unregister(mod->dispatcher, wakeup_audio_callback);
    }
    wakeup_close(mod);
    /* 卸载ASR引擎动态库 */
    if (mod->asr_lib)
    {
        dlclose(mod->asr_lib);
        mod->asr_lib = NULL;
    }
}
