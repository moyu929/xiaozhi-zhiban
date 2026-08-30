/**
 * @file mcp_handler.c
 * @brief MCP（Model Context Protocol）处理器实现
 *
 * 实现MCP协议的设备端处理，包括：
 * - 动态加载设备控制库（libmsg_server_api.so）
 * - MCP工具调用处理（获取设备状态、设置音量/亮度、重启/关机等）
 * - IoT指令处理
 * - JSON-RPC协议响应
 * - 系统信息读取（/proc文件系统）
 *
 * 注意：MCP协议中的 "xiaozhi-assistant" 是服务名标识，保留不改
 */

#include "mcp_handler.h"
#include "plog.h"
#include "xiaozhi_config.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/input.h>
#include "platform_power.h"
#include "use_limit.h"
#include "display_ctrl.h"
#include "reverse/applib_api.h"   /* get_config/set_config/sync_config (libapconfig) */

/* 动态加载符号的宏，加载失败时输出警告日志 */
#define LOAD_SYM(h, name, type)                     \
    do                                              \
    {                                               \
        mcp->name = (type)dlsym(h, #name);          \
        if (!mcp->name)                             \
            PLOG_W("MCP", "符号未找到: %s", #name); \
    } while (0)

/**
 * @brief 初始化MCP处理器
 * @param mcp MCP处理器实例指针
 * @return 0成功，-1失败
 *
 * 动态加载libmsg_server_api.so库并解析所需的函数符号
 */
int mcp_handler_init(mcp_handler_t *mcp)
{
    if (!mcp)
        return -1;
    memset(mcp, 0, sizeof(*mcp));

    /* 动态加载设备控制库 */
    mcp->lib_handle = dlopen("libmsg_server_api.so", RTLD_NOW);
    if (!mcp->lib_handle)
    {
        PLOG_E("MCP", "加载 libmsg_server_api.so 失败: %s", dlerror());
        return -1;
    }

    /* 加载音频控制函数 */
    LOAD_SYM(mcp->lib_handle, sound_set_sys_volume, int (*)(int));
    LOAD_SYM(mcp->lib_handle, sound_get_sys_volume, int (*)(void));
    LOAD_SYM(mcp->lib_handle, sound_set_sys_mute, int (*)(int));
    LOAD_SYM(mcp->lib_handle, sound_is_sys_mute, int (*)(void));

    LOAD_SYM(mcp->lib_handle, power_get_charge_status, int (*)(int *));
    LOAD_SYM(mcp->lib_handle, power_get_battery_cap, int (*)(void));
    LOAD_SYM(mcp->lib_handle, power_get_battery_voltage, int (*)(void));

    /* 加载TTS播放函数 */
    LOAD_SYM(mcp->lib_handle, sound_tts_play, void (*)(int));

    PLOG_I("MCP", "初始化完成，已加载 libmsg_server_api.so");
    return 0;
}

/**
 * @brief 销毁MCP处理器，释放资源
 * @param mcp MCP处理器实例指针
 */
void mcp_handler_destroy(mcp_handler_t *mcp)
{
    if (!mcp)
        return;
    if (mcp->lib_handle)
    {
        dlclose(mcp->lib_handle);
        mcp->lib_handle = NULL;
    }
    PLOG_I("MCP", "已销毁");
}

/**
 * @brief 设置JSON消息发送回调
 * @param mcp MCP处理器实例指针
 * @param send_cb 发送回调函数
 * @param user_data 传递给回调的用户数据
 */
void mcp_handler_set_send_cb(mcp_handler_t *mcp, mcp_send_json_cb_t send_cb, void *user_data)
{
    if (!mcp)
        return;
    mcp->send_json = send_cb;
    mcp->user_data = user_data;
}

/**
 * @brief 从JSON数据中查找指定键的字符串值
 * @param json JSON数据指针
 * @param len JSON数据长度
 * @param key 要查找的键名
 * @param out 输出缓冲区
 * @param out_size 输出缓冲区大小
 * @return 找到返回输出缓冲区指针，未找到返回NULL
 */
static const char *find_string(const char *json, size_t len, const char *key, char *out, int out_size)
{
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\"", key);

    const char *p = (const char *)memmem(json, len, search_key, strlen(search_key));
    if (!p)
        return NULL;

    p += strlen(search_key);
    while (p < json + len && (*p == ' ' || *p == ':' || *p == '\t'))
        p++;
    if (p >= json + len || *p != '"')
        return NULL;
    p++;

    /* 提取字符串值，处理转义字符 */
    int i = 0;
    while (p < json + len && *p != '"' && i < out_size - 1)
    {
        if (*p == '\\' && p + 1 < json + len)
        {
            p++;
            switch (*p)
            {
            case 'n':
                out[i++] = '\n';
                break;
            case 'r':
                out[i++] = '\r';
                break;
            case 't':
                out[i++] = '\t';
                break;
            case '"':
                out[i++] = '"';
                break;
            case '\\':
                out[i++] = '\\';
                break;
            default:
                out[i++] = *p;
                break;
            }
        }
        else
        {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return out;
}

/**
 * @brief 从JSON数据中查找指定键的整数值
 * @param json JSON数据指针
 * @param len JSON数据长度
 * @param key 要查找的键名
 * @param default_val 默认值
 * @return 找到的整数值，未找到返回默认值
 */
static int find_int(const char *json, size_t len, const char *key, int default_val)
{
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\"", key);
    const char *p = (const char *)memmem(json, len, search_key, strlen(search_key));
    if (!p)
        return default_val;
    p += strlen(search_key);
    while (p < json + len && (*p == ' ' || *p == ':' || *p == '\t'))
        p++;
    return atoi(p);
}

/**
 * @brief 发送MCP成功响应
 * @param mcp MCP处理器实例指针
 * @param id 请求ID
 * @param text 响应文本内容
 */
static void send_mcp_response(mcp_handler_t *mcp, int64_t id, const char *text)
{
    if (!mcp->send_json)
        return;

    char json[1024];
    int n = snprintf(json, sizeof(json),
                     "{\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":%lld,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"%s\"}]}}}",
                     (long long)id, text);

    mcp->send_json(json, n, mcp->user_data);
}

/**
 * @brief 发送MCP错误响应
 * @param mcp MCP处理器实例指针
 * @param id 请求ID
 * @param code 错误码
 * @param message 错误消息
 */
static void send_mcp_error(mcp_handler_t *mcp, int64_t id, int code, const char *message)
{
    if (!mcp->send_json)
        return;

    char json[1024];
    int n = snprintf(json, sizeof(json),
                     "{\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":%lld,\"error\":{\"code\":%d,\"message\":\"%s\"}}}",
                     (long long)id, code, message);

    mcp->send_json(json, n, mcp->user_data);
}

/**
 * @brief 从/proc文件中读取指定键的整数值
 * @param path /proc文件路径
 * @param key 要查找的键名
 * @return 找到的整数值，未找到返回-1
 */
static int read_proc_int(const char *path, const char *key)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char line[256];
    int val = -1;
    while (fgets(line, sizeof(line), f))
    {
        if (strstr(line, key) == line)
        {
            char *p = line + strlen(key);
            while (*p == ' ' || *p == ':')
                p++;
            val = atoi(p);
            break;
        }
    }
    fclose(f);
    return val;
}

static int inject_key_event(int key_code)
{
    int fd = open("/dev/input/event2", O_WRONLY | O_NONBLOCK);
    if (fd < 0)
    {
        PLOG_W("MCP", "inject_key: 打开/dev/input/event2失败: %m");
        return -1;
    }
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    gettimeofday(&ev.time, NULL);
    ev.type = EV_KEY;
    ev.code = key_code;
    ev.value = 1;
    write(fd, &ev, sizeof(ev));
    memset(&ev, 0, sizeof(ev));
    gettimeofday(&ev.time, NULL);
    ev.type = EV_SYN;
    ev.code = SYN_REPORT;
    ev.value = 0;
    write(fd, &ev, sizeof(ev));
    memset(&ev, 0, sizeof(ev));
    gettimeofday(&ev.time, NULL);
    ev.type = EV_KEY;
    ev.code = key_code;
    ev.value = 0;
    write(fd, &ev, sizeof(ev));
    memset(&ev, 0, sizeof(ev));
    gettimeofday(&ev.time, NULL);
    ev.type = EV_SYN;
    ev.code = SYN_REPORT;
    ev.value = 0;
    write(fd, &ev, sizeof(ev));
    close(fd);
    return 0;
}

static int inject_key_repeat(int key_code, int count)
{
    for (int i = 0; i < count; i++)
    {
        if (inject_key_event(key_code) < 0)
            return -1;
        if (i < count - 1)
            usleep(80000);
    }
    PLOG_I("MCP", "inject_key_repeat: code=%d count=%d", key_code, count);
    return 0;
}

static int exec_tool(mcp_handler_t *mcp, const char *name, const char *args_json, size_t args_len, char *result, int result_size)
{
    /* 获取设备状态：音量、亮度、充电状态、电池、CPU负载、内存 */
    if (strcmp(name, "self.get_device_status") == 0)
    {
        int vol = 0, charge_st = 0, battery = 0;
        if (mcp->sound_get_sys_volume)
            vol = mcp->sound_get_sys_volume();
        if (mcp->power_get_charge_status)
            mcp->power_get_charge_status(&charge_st);
        if (mcp->power_get_battery_cap)
            battery = mcp->power_get_battery_cap();

        int mem_total = read_proc_int("/proc/meminfo", "MemTotal");
        int mem_free = read_proc_int("/proc/meminfo", "MemFree");
        int cached = read_proc_int("/proc/meminfo", "Cached");

        char loadbuf[64] = "";
        FILE *lf = fopen("/proc/loadavg", "r");
        if (lf)
        {
            fgets(loadbuf, sizeof(loadbuf), lf);
            fclose(lf);
        }
        char *sp = strchr(loadbuf, ' ');
        if (sp)
            *sp = '\0';

        int mem_used_kb = (mem_total > 0 && mem_free >= 0) ? (mem_total - mem_free - (cached > 0 ? cached : 0)) : 0;
        int vol_pct = (vol >= 0 && vol <= 40) ? vol * 100 / 40 : 0;

        snprintf(result, result_size,
                 "volume=%d%% charging=%s battery=%d%% cpu_load=%s mem_used=%dKB mem_free=%dKB mem_total=%dKB",
                 vol_pct, charge_st ? "yes" : "no", battery,
                 loadbuf[0] ? loadbuf : "N/A",
                 mem_used_kb, mem_free >= 0 ? mem_free : 0, mem_total >= 0 ? mem_total : 0);
        return 0;
    }

    if (strcmp(name, "self.audio_speaker.volume_up") == 0)
    {
        int vol = 0;
        if (mcp->sound_get_sys_volume)
            vol = mcp->sound_get_sys_volume();
        int steps = (vol < 40) ? 2 : 0;
        int ret = -1;
        if (steps > 0)
            ret = inject_key_repeat(INJECT_KEY_VOLUP, steps);
        if (ret < 0 && mcp->sound_set_sys_volume && vol < 40)
        {
            mcp->sound_set_sys_volume(vol + 2);
            PLOG_I("MCP", "volume_up: 按键注入失败, 后备sound_set %d->%d", vol, vol + 2);
        }
        else
        {
            PLOG_I("MCP", "volume_up: 按键注入%d步 (vol=%d)", steps, vol);
        }
        snprintf(result, result_size, "volume up (%d->%d)", vol, vol < 40 ? vol + 2 : vol);
        return 0;
    }

    if (strcmp(name, "self.audio_speaker.volume_down") == 0)
    {
        int vol = 0;
        if (mcp->sound_get_sys_volume)
            vol = mcp->sound_get_sys_volume();
        int steps = (vol > 0) ? 2 : 0;
        int ret = -1;
        if (steps > 0)
            ret = inject_key_repeat(INJECT_KEY_VOLDOWN, steps);
        if (ret < 0 && mcp->sound_set_sys_volume && vol > 0)
        {
            mcp->sound_set_sys_volume(vol - 2);
            PLOG_I("MCP", "volume_down: 按键注入失败, 后备sound_set %d->%d", vol, vol - 2);
        }
        else
        {
            PLOG_I("MCP", "volume_down: 按键注入%d步 (vol=%d)", steps, vol);
        }
        snprintf(result, result_size, "volume down (%d->%d)", vol, vol > 0 ? vol - 2 : vol);
        return 0;
    }

    if (strcmp(name, "self.reboot") == 0)
    {
        snprintf(result, result_size, "rebooting");
        PLOG_I("MCP", "收到重启请求");
        platform_power_reboot();
        return 0;
    }

    /* 关机 */
    if (strcmp(name, "self.poweroff") == 0)
    {
        /* Q4/R-03: 走原生 MSG_SHORTCUT_POWER 动画关机链(P3/M1 §8), 弃 system("poweroff")
         * 注: 不播任何预置语音——原生关机动画链零播报(launcher 反汇编实证),
         * 旧代码误调 sound_tts_play(10), 而 id=10 实为低电提醒语音
         * "小主人我快没电了"(Package.dat/tts_10.mp3, 5.8s), 关机时会
         * 误播低电提醒(2026-08-30 实机实测) */
        snprintf(result, result_size, "powering off");
        PLOG_I("MCP", "收到关机请求");
        platform_power_shutdown_elegant();
        return 0;
    }

    /* 每日使用时长状态（§四, 只读; 配置键真库接口为 get_config(C1 §5)） */
    if (strcmp(name, "self.limit_info") == 0)
    {
        long spent = use_limit_spent_sec();
        int locked = use_limit_is_locked();
        int enable = use_limit_get_enable();
        int minutes = use_limit_get_minutes();
        long delay = use_limit_delay_until();

        if (!enable)
        {
            snprintf(result, result_size,
                     "今日已使用%d分钟。每日使用时长限制当前未开启。",
                     (int)(spent / 60));
        }
        else if (locked)
        {
            snprintf(result, result_size,
                     "今日使用时长已用完，已使用%d分钟，上限%d分钟，现在是锁定状态。",
                     (int)(spent / 60), minutes);
        }
        else if (minutes > 0)
        {
            long remain = (long)minutes * 60 - spent;
            if (remain < 0)
                remain = 0; /* 延迟窗口内: 已超限但豁免中 */
            snprintf(result, result_size,
                     "今日已使用%d分钟，上限%d分钟，还剩%d分钟%s。",
                     (int)(spent / 60), minutes, (int)(remain / 60),
                     delay > 0 ? "（临时延长中）" : "");
        }
        else
        {
            snprintf(result, result_size,
                     "今日已使用%d分钟。每日使用时长限制未设置上限。",
                     (int)(spent / 60));
        }
        /* 时段限制状态附加 */
        if (use_limit_out_of_span())
        {
            int nlen = strlen(result);
            snprintf(result + nlen, result_size - nlen, "现在不在允许使用的时段哦。");
        }
        PLOG_I("MCP", "limit_info: enable=%d min=%d spent=%lds locked=%d",
               enable, minutes, spent, locked);
        return 0;
    }

    /* 每日时长临时延迟: "再延长10分钟" (名字后缀带分钟数, 同 screen_off_set 模式)
     * 家长开关 USE_LIMIT_DELAY_TOOL 控制, 默认关闭(防孩子语音绕过限制) */
    if (strncmp(name, "self.limit_delay ", 18) == 0)
    {
        if (!use_limit_delay_tool_enabled())
        {
            snprintf(result, result_size,
                     "延长使用时间的功能没有开启，请让爸爸妈妈在设置里打开哦");
            PLOG_I("MCP", "limit_delay 被拒: 语音延迟工具未开启");
            return 0;
        }
        int mins = atoi(name + 18);
        if (mins <= 0 || mins > 720)
        {
            snprintf(result, result_size, "延长时间须为1到720分钟");
            return 0;
        }
        long until = use_limit_request_delay(mins);
        (void)until;
        snprintf(result, result_size,
                 "好的，已延长使用时间%d分钟", mins);
        PLOG_I("MCP", "limit_delay %d分钟, 到期epoch=%ld", mins, until);
        return 0;
    }

    /* 主动息屏: 立即灭屏, 交流不受影响(§八③延伸, 2026-08-30) */
    if (strcmp(name, "self.screen_off_now") == 0)
    {
        int r = display_ctrl_manual_off();
        snprintf(result, result_size,
                 r == 0 ? "好的，屏幕已关闭，随时可以聊天" : "屏幕当前已是关闭状态");
        PLOG_I("MCP", "screen_off_now: ret=%d", r);
        return 0;
    }

    /* 主动亮屏: 手动息屏后语音唤醒屏幕(2026-08-30) */
    if (strcmp(name, "self.screen_on_now") == 0)
    {
        int r = display_ctrl_manual_on();
        snprintf(result, result_size,
                 r == 0 ? "好的，屏幕已打开" : "屏幕现在就是亮着的");
        PLOG_I("MCP", "screen_on_now: ret=%d", r);
        return 0;
    }

    /* 会话域息屏秒数设置（§八③, 形如 "self.screen_off_set 30"） */
    if (strncmp(name, "self.screen_off_set ", 20) == 0)
    {
        extern void display_ctrl_reload_config(void);
        int sec = atoi(name + 20);
        if (sec < 0) sec = 0;
        char _v[16];
        snprintf(_v, sizeof(_v), "%d", sec);
        set_config("SCREEN_OFF_IDLE_SEC", _v, strlen(_v));
        sync_config();
        display_ctrl_reload_config();
        snprintf(result, result_size, "screen-off idle seconds = %d%s",
                 sec, sec > 0 ? "" : " (disabled)");
        PLOG_I("MCP", "screen_off_set(%d)", sec);
        return 0;
    }

    /* 获取系统信息 */
    if (strcmp(name, "self.get_system_info") == 0)
    {
        int vol = 0, charge_st = 0, battery = 0;
        if (mcp->sound_get_sys_volume)
            vol = mcp->sound_get_sys_volume();
        if (mcp->power_get_charge_status)
            mcp->power_get_charge_status(&charge_st);
        if (mcp->power_get_battery_cap)
            battery = mcp->power_get_battery_cap();

        snprintf(result, result_size,
                 "xiaozhi-assistant P5 | vol=%d/40 chg=%d bat=%d%%",
                 vol, charge_st, battery);
        return 0;
    }

    /* 清理垃圾文件和缓存 */
    if (strcmp(name, "self.clean_junk") == 0)
    {
        int freed_kb = 0;

        /* 清理/tmp目录下的日志和临时文件 */
        DIR *d = opendir("/tmp");
        if (d)
        {
            struct dirent *ent;
            char path[320];
            while ((ent = readdir(d)) != NULL)
            {
                if (ent->d_name[0] == '.')
                    continue;
                if (strstr(ent->d_name, "log.") || strstr(ent->d_name, ".log") ||
                    strstr(ent->d_name, ".tmp") || strstr(ent->d_name, ".bak") ||
                    strstr(ent->d_name, "core."))
                {
                    snprintf(path, sizeof(path), "/tmp/%s", ent->d_name);
                    struct stat st;
                    if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
                    {
                        freed_kb += (int)(st.st_size / 1024);
                        unlink(path);
                        PLOG_I("MCP", "已清理 %s (%lld 字节)", path, (long long)st.st_size);
                    }
                }
            }
            closedir(d);
        }

        /* 释放系统页面缓存 */
        int mem_free_before = read_proc_int("/proc/meminfo", "MemFree");

        system("echo 3 > /proc/sys/vm/drop_caches 2>/dev/null");

        int mem_free_after = read_proc_int("/proc/meminfo", "MemFree");
        int reclaimed = (mem_free_after >= 0 && mem_free_before >= 0) ? (mem_free_after - mem_free_before) : 0;

        snprintf(result, result_size,
                 "cleaned %dKB from tmp files, cache dropped, mem reclaimed ~%dKB",
                 freed_kb, reclaimed > 0 ? reclaimed : 0);
        return 0;
    }

    /* 列出所有可用的MCP工具 */
    if (strcmp(name, "self.get_mcp_tools") == 0)
    {
        snprintf(result, result_size,
                 "Available MCP tools: "
                 "1.self.get_device_status - Get device status (volume%%,battery,CPU,memory); "
                 "2.self.audio_speaker.volume_up - Increase volume by one step (shows native volume bar); "
                 "3.self.audio_speaker.volume_down - Decrease volume by one step (shows native volume bar); "
                 "4.self.get_system_info - Get system info; "
                 "5.self.clean_junk - Clean temp files and drop caches; "
                 "6.self.get_mcp_tools - List all MCP tools; "
                 "7.self.reboot - Restart device, ONLY for explicit 重启/重新启动 requests (user only); "
                 "8.self.poweroff - Power off / shut down, for 关机/关闭/不玩了 requests, native shutdown animation (user only); "
                 "9.self.limit_info - Ask today's usage time (今日已用/上限/剩余); "
                 "10.self.limit_delay N - Extend usage time N minutes when daily limit reached (再延长N分钟); "
                 "11.self.screen_off_set N - Set screen-off idle seconds (0=off); "
                 "12.self.screen_off_now - Turn off screen immediately, voice chat continues; "
                 "13.self.screen_on_now - Turn screen back on. 亮屏/打开屏幕");
        return 0;
    }

    snprintf(result, result_size, "unknown tool: %s", name);
    return -1;
}

/**
 * @brief 处理MCP/IoT消息
 * @param mcp MCP处理器实例指针
 * @param json JSON消息字符串
 * @param len JSON消息长度
 *
 * 支持的消息类型：
 * - mcp: MCP协议消息（tools/call、tools/list、initialize等）
 * - iot: IoT设备控制消息（批量执行设备命令）
 */
void mcp_handler_process_message(mcp_handler_t *mcp, const char *json, size_t len)
{
    if (!mcp || !json || len == 0)
        return;

    char type_str[64] = {0};
    find_string(json, len, "type", type_str, sizeof(type_str));

    if (strcmp(type_str, "mcp") == 0)
    {
        /* 解析MCP消息的payload部分 */
        const char *payload_start = (const char *)memmem(json, len, "\"payload\"", 9);
        if (!payload_start)
        {
            PLOG_W("MCP", "MCP消息缺少payload");
            return;
        }
        const char *p = payload_start + 9;
        while (p < json + len && (*p == ' ' || *p == ':' || *p == '\t'))
            p++;
        if (p >= json + len || *p != '{')
            return;

        size_t payload_len = len - (p - json);
        char method[128] = {0};
        find_string(p, payload_len, "method", method, sizeof(method));

        int64_t id = find_int(p, payload_len, "id", -1);

        if (strcmp(method, "tools/call") == 0)
        {
            /* 处理工具调用请求 */
            const char *params_start = (const char *)memmem(p, payload_len, "\"params\"", 8);
            if (!params_start)
            {
                send_mcp_error(mcp, id, -32602, "missing params");
                return;
            }
            const char *pp = params_start + 8;
            while (pp < p + payload_len && (*pp == ' ' || *pp == ':' || *pp == '\t'))
                pp++;
            if (pp >= p + payload_len || *pp != '{')
                return;

            /* 解析工具名称和参数 */
            size_t params_len = payload_len - (pp - p);
            char tool_name[128] = {0};
            find_string(pp, params_len, "name", tool_name, sizeof(tool_name));

            /* 解析arguments对象 */
            const char *args_start = NULL;
            size_t args_len = 0;
            const char *a = (const char *)memmem(pp, params_len, "\"arguments\"", 11);
            if (a)
            {
                a += 11;
                while (a < pp + params_len && (*a == ' ' || *a == ':' || *a == '\t'))
                    a++;
                if (a < pp + params_len && *a == '{')
                {
                    args_start = a;
                    /* 通过括号深度匹配找到arguments对象的结束位置 */
                    const char *end = a;
                    int depth = 1;
                    while (end < pp + params_len && depth > 0)
                    {
                        if (*end == '{')
                            depth++;
                        else if (*end == '}')
                            depth--;
                        end++;
                    }
                    args_len = end - a;
                }
            }

            if (tool_name[0] == '\0')
            {
                send_mcp_error(mcp, id, -32602, "missing tool name");
                return;
            }

            /* 执行工具并返回结果 */
            char result[512];
            int ret = exec_tool(mcp, tool_name,
                                args_start ? args_start : json,
                                args_start ? args_len : len,
                                result, sizeof(result));

            PLOG_I("MCP", "工具=%s ID=%lld 结果=%s", tool_name, (long long)id, result);

            if (ret == 0)
            {
                send_mcp_response(mcp, id, result);
            }
            else
            {
                send_mcp_error(mcp, id, -32603, result);
            }
        }
        else if (strcmp(method, "tools/list") == 0)
        {
            /* 返回可用工具列表 */
            PLOG_I("MCP", "tools/list 请求, ID=%lld", (long long)id);
            if (mcp->send_json)
            {
                /* 11 个工具的长描述实测逼近 5K(2026-08-30 事故: 5120 截断
                 * 产生非法 JSON, 云端解析失败即断连 → "唤醒后秒退"),
                 * 扩到 8192 并加截断熔断日志 */
                char json[8192];
                int n = snprintf(json, sizeof(json),
                                 "{\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":%lld,"
                                 "\"result\":{\"tools\":["
                                 "{\"name\":\"self.get_device_status\",\"description\":\"Device status: volume, battery, charging, CPU, memory. 问设备状态/电量/音量\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
                                 "{\"name\":\"self.audio_speaker.volume_up\",\"description\":\"Volume up one step, native volume bar shown. 大点声/调大音量\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
                                 "{\"name\":\"self.audio_speaker.volume_down\",\"description\":\"Volume down one step, native volume bar shown. 小点声/调小音量\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
                                 "{\"name\":\"self.get_system_info\",\"description\":\"System info: version, volume, battery. 查版本/系统信息\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
                                 "{\"name\":\"self.clean_junk\",\"description\":\"Clean temp files and caches to free memory. 清理垃圾/清理内存\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
                                 "{\"name\":\"self.get_mcp_tools\",\"description\":\"List all device MCP tools. 有哪些工具\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
                                 "{\"name\":\"self.reboot\",\"description\":\"Restart device. ONLY for 重启/重新启动, NOT for 关机 (use self.poweroff).\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}},\"annotations\":{\"audience\":[\"user\"]}},"
                                 "{\"name\":\"self.limit_info\",\"description\":\"Query today's usage time: used/limit/remaining minutes. 今天用了多久/还剩多久.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
                                 "{\"name\":\"self.limit_delay\",\"description\":\"Extend usage N minutes, needs parent enable. 延长时间/再玩N分钟. Append minutes: self.limit_delay 10\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
                                 "{\"name\":\"self.screen_off_now\",\"description\":\"Turn off screen now, voice chat continues. 关屏幕/息屏. Touch wakes screen.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
                                 "{\"name\":\"self.screen_on_now\",\"description\":\"Turn the screen back on after it was turned off. Use when user says 亮屏/打开屏幕/把屏幕点亮.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
                                 "{\"name\":\"self.poweroff\",\"description\":\"Shut down device with native animation, for 关机/关闭/不玩了/睡觉. NOT for 重启 (use self.reboot).\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}},\"annotations\":{\"audience\":[\"user\"]}}"
                                 "]}}}",
                                 (long long)id);
                if (n >= (int)sizeof(json) - 1)
                    PLOG_E("MCP", "tools/list 响应被截断(%d>=%d), JSON 非法!", n, (int)sizeof(json));
                mcp->send_json(json, n, mcp->user_data);
            }
        }
        else if (strcmp(method, "initialize") == 0)
        {
            /* MCP协议初始化响应 */
            PLOG_I("MCP", "初始化: 协议版本=2024-11-05");
            if (mcp->send_json)
            {
                char json[512];
                int n = snprintf(json, sizeof(json),
                                 "{\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":%lld,"
                                 "\"result\":{\"protocolVersion\":\"2024-11-05\","
                                 "\"capabilities\":{\"tools\":{}},"
                                 "\"serverInfo\":{\"name\":\"xiaozhi-assistant\",\"version\":\"P5\"}}}}",
                                 (long long)id);
                mcp->send_json(json, n, mcp->user_data);
            }
        }
        else if (strcmp(method, "notifications/initialized") == 0)
        {
            /* 服务器初始化完成通知 */
            PLOG_I("MCP", "服务器初始化完成通知");
        }
        else
        {
            PLOG_W("MCP", "未知方法: %s", method);
        }
    }
    else if (strcmp(type_str, "iot") == 0)
    {
        /* 处理IoT设备控制消息 */
        PLOG_I("MCP", "IoT消息: %.*s", (int)(len > 200 ? 200 : len), json);

        /* 解析commands数组 */
        const char *cmd_start = (const char *)memmem(json, len, "\"commands\"", 10);
        if (!cmd_start)
            return;
        const char *c = cmd_start + 10;
        while (c < json + len && (*c == ' ' || *c == ':' || *c == '\t'))
            c++;
        if (c >= json + len || *c != '[')
            return;

        /* 找到数组结束位置 */
        const char *arr_end = c;
        int depth = 1;
        while (arr_end < json + len && depth > 0)
        {
            if (*arr_end == '[')
                depth++;
            else if (*arr_end == ']')
                depth--;
            arr_end++;
        }

        /* 逐个解析并执行命令 */
        const char *item = c + 1;
        while (item < arr_end - 1)
        {
            while (item < arr_end - 1 && (*item == ' ' || *item == ',' || *item == '\n' || *item == '\t'))
                item++;
            if (item >= arr_end - 1 || *item != '{')
                break;

            /* 通过括号深度匹配找到命令对象的结束位置 */
            const char *item_end = item;
            int item_depth = 1;
            while (item_end < arr_end - 1 && item_depth > 0)
            {
                if (*item_end == '{')
                    item_depth++;
                else if (*item_end == '}')
                    item_depth--;
                item_end++;
            }
            size_t item_len = item_end - item;

            /* 提取命令名称并执行 */
            char cmd_name[128] = {0};
            find_string(item, item_len, "name", cmd_name, sizeof(cmd_name));

            if (cmd_name[0])
            {
                char result[256];
                exec_tool(mcp, cmd_name, item, item_len, result, sizeof(result));
            }
            item = item_end;
        }
    }
}
