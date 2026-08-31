/**
 * @file api_server.c
 * @brief 文件IPC接口（替代原HTTP API服务器）
 *
 * 与xwebd通过文件系统通信，避免HTTP/TCP开销：
 * - 状态输出：/tmp/sair_status.json（状态变化时写入）
 * - 配置输出：/tmp/sair_config.json（配置变化时写入）
 * - 命令输入：/tmp/sair_cmd.json（xwebd写入，assistant定时读取执行后删除）
 * - 自检触发：/tmp/sair_diag_request（xwebd创建空文件触发）
 * - 自检输出：/tmp/sair_diag.json（assistant按需生成）
 */

#include "api_server.h"
#include "app_context.h"
#include "xiaozhi_config.h"
#include "plog.h"
#include "state_machine.h"
#include "config_manager.h"
#include "diag_module.h"
#include "use_limit.h"
#include "display_ctrl.h"
#include "applib_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#define TAG "IPC"

/* main.c 提供: bot_push 开关状态(点文件) */
extern int boot_push_disable_enabled(void);

static int json_escape(const char *s, char *out, int out_size)
{
    int i = 0;
    while (*s && i < out_size - 2)
    {
        if (*s == '"' || *s == '\\') { out[i++] = '\\'; out[i++] = *s; }
        else if (*s == '\n') { out[i++] = '\\'; out[i++] = 'n'; }
        else if (*s == '\r') { out[i++] = '\\'; out[i++] = 'r'; }
        else if (*s == '\t') { out[i++] = '\\'; out[i++] = 't'; }
        else { out[i++] = *s; }
        s++;
    }
    out[i] = '\0';
    return i;
}

extern app_context_t g_app;

static const char *state_to_string(xiaozhi_state_t state)
{
    switch (state)
    {
    case kStateStarting:   return "Starting";
    case kStateActivating: return "Activating";
    case kStateIdle:       return "Idle";
    case kStateConnecting: return "Connecting";
    case kStateListening:  return "Listening";
    case kStateSpeaking:   return "Speaking";
    case kStateCleaning:   return "Cleaning";
    default:               return "Unknown";
    }
}

static int write_file_atomic(const char *path, const char *data, int len)
{
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        PLOG_E(TAG, "写入 %s 失败: %s", tmp_path, strerror(errno));
        return -1;
    }

    int written = write(fd, data, len);
    close(fd);

    if (written != len)
    {
        unlink(tmp_path);
        PLOG_E(TAG, "写入 %s 不完整: %d/%d", tmp_path, written, len);
        return -1;
    }

    if (rename(tmp_path, path) != 0)
    {
        unlink(tmp_path);
        PLOG_E(TAG, "重命名 %s -> %s 失败: %s", tmp_path, path, strerror(errno));
        return -1;
    }

    return 0;
}

void api_server_write_status(void)
{
    xiaozhi_state_t state = state_machine_get_state(&g_app.sm);
    char esc_code[256];
    json_escape(g_app.config.activation_code, esc_code, sizeof(esc_code));
    char buf[512];
    int len = snprintf(buf, sizeof(buf),
        "{\"state\":\"%s\",\"version\":\"%s\",\"activation_code\":\"%s\",\"activated\":%s}\n",
        state_to_string(state),
        XIAOZHI_VERSION,
        esc_code,
        g_app.config.has_ws_config ? "true" : "false");
    write_file_atomic("/tmp/sair_status.json", buf, len);
}

void api_server_write_config(void)
{
    int plog_lvl = plog_get_level();
    char esc_ws_url[1024], esc_ws_token[512], esc_mcp[1024];
    json_escape(g_app.config.ws_url, esc_ws_url, sizeof(esc_ws_url));
    json_escape(g_app.config.ws_token, esc_ws_token, sizeof(esc_ws_token));
    json_escape(g_app.config.mcp_endpoint, esc_mcp, sizeof(esc_mcp));
    /* 会话息屏秒数(display_ctrl 域, libapconfig 持久) */
    char so_buf[16] = "0";
    if (get_config("SCREEN_OFF_IDLE_SEC", so_buf, sizeof(so_buf)) <= 0 || !so_buf[0])
        snprintf(so_buf, sizeof(so_buf), "0");
    /* 当前系统音量(原生 sound 库, 0-40; -1=原生库未加载) */
    int vol = (g_app.mcp.sound_get_sys_volume) ? g_app.mcp.sound_get_sys_volume() : -1;
    char buf[4096];
    int len = snprintf(buf, sizeof(buf),
        "{\"ws_url\":\"%s\",\"ws_token\":\"%s\",\"log_level\":\"%s\","
        "\"listen_timeout\":%llu,\"session_timeout\":%llu,"
        "\"wakeup_cooldown\":%llu,\"ws_ping_interval\":%llu,"
        "\"mcp_endpoint\":\"%s\",\"listening_mode\":\"%s\",\"aec_mode\":\"%s\","
        "\"boot_push_disable\":%d,\"screen_off_idle_sec\":%d,\"volume\":%d,"
        "\"use_limit\":{\"enable\":%d,\"minutes\":%d,\"spent_sec\":%ld,"
        "\"remain_sec\":%ld,\"locked\":%d,\"delay_until\":%ld,\"delay_tool\":%d,"
        "\"sched\":{\"enable\":%d,\"days\":%d,\"spans\":\"%s\",\"in_span\":%d}}}\n",
        esc_ws_url,
        esc_ws_token,
        plog_lvl == PLOG_LEVEL_DEBUG ? "DEBUG" :
        plog_lvl == PLOG_LEVEL_INFO  ? "INFO" :
        plog_lvl == PLOG_LEVEL_WARN  ? "WARN" : "ERROR",
        (unsigned long long)g_app.listen_timeout_ms,
        (unsigned long long)g_app.session_timeout_ms,
        (unsigned long long)g_app.wakeup_cooldown_ms,
        (unsigned long long)g_app.ws_ping_interval_ms,
        esc_mcp,
        g_app.listening_mode == LISTENING_MODE_REALTIME ? "realtime" : "autostop",
        g_app.aec_mode ? "cloud" : "local",
        boot_push_disable_enabled(),
        atoi(so_buf),
        vol,
        use_limit_get_enable(),
        use_limit_get_minutes(),
        use_limit_spent_sec(),
        use_limit_get_enable() && use_limit_get_minutes() > 0
            ? (long)use_limit_get_minutes() * 60 - use_limit_spent_sec()
            : 0,
        use_limit_is_locked(),
        use_limit_delay_until(),
        use_limit_delay_tool_enabled(),
        use_limit_get_sched_enable(),
        use_limit_get_days(),
        use_limit_get_spans_csv(),
        use_limit_out_of_span() ? 0 : 1);
    write_file_atomic("/tmp/sair_config.json", buf, len);
}

static int parse_json_str(const char *json, const char *key, char *out, int out_size)
{
    /* 精确键匹配(同 parse_json_int: 防前缀子串键误命中) */
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    int klen = strlen(search);
    const char *p = json;
    const char *found = NULL;
    while ((p = strstr(p, search)) != NULL)
    {
        /* 键起点前的空白也要跨过: python json.dumps 生成 ", " / ": " 带空格,
         * 只认紧贴的 {/, 会丢弃第二个及以后的字段(2026-08-31 事故:
         * 面板整卡保存只有第一个字段生效, use_limit_minutes 排后全被丢) */
        const char *b = (p > json) ? p - 1 : json;
        while (b > json && (*b == ' ' || *b == '\t' || *b == '\n' || *b == '\r'))
            b--;
        if (p == json || *b == '{' || *b == ',')
        {
            const char *q = p + klen;
            while (*q == ' ' || *q == '\t')
                q++;
            if (*q == ':')
            {
                found = q + 1;
                break;
            }
        }
        p += klen;
    }
    if (!found) return -1;
    while (*found == ' ' || *found == '\t')
        found++;
    if (*found != '"') return -1;
    const char *s = found + 1;
    int i = 0;
    while (*s && *s != '"' && i < out_size - 1)
    {
        if (*s == '\\' && *(s + 1))
        {
            s++;
            out[i++] = *s++;
        }
        else
        {
            out[i++] = *s++;
        }
    }
    out[i] = '\0';
    return 0;
}

static int parse_json_int(const char *json, const char *key, int *out)
{
    /* 精确键匹配: 键名后必须是 ':'(跳过空白), 且键匹配从 JSON 起点或
     * '{'/',' 之后开始——朴素 strstr 会命中前缀子串键(2026-08-30 事故:
     * "delay_until" 的值被 "delay_tool"/"sched_enable" 解析重复命中,
     * epoch 值串进所有后续字段, 时段锁误开导致全部唤醒被拦) */
    char search[160];
    snprintf(search, sizeof(search), "\"%s\"", key);
    int klen = strlen(search);
    const char *p = json;
    while ((p = strstr(p, search)) != NULL)
    {
        /* 前一字符必须是对象边界(起始/{/,), 排除 "xxxdelay_min" 类子串键;
         * 边界判定先跨过键前空白——宽松格式(python json.dumps ", ")下
         * 第二个及以后的字段键前是空格, 紧贴判定会整字段丢弃 */
        const char *b = (p > json) ? p - 1 : json;
        while (b > json && (*b == ' ' || *b == '\t' || *b == '\n' || *b == '\r'))
            b--;
        if (p == json || *b == '{' || *b == ',')
        {
            const char *q = p + klen;
            while (*q == ' ' || *q == '\t')
                q++;
            if (*q == ':')
            {
                q++;
                while (*q == ' ' || *q == '\t')
                    q++;
                *out = atoi(q);
                return 0;
            }
        }
        p += klen;
    }
    return -1;
}

void api_server_check_commands(void)
{
    if (access("/tmp/sair_diag_request", F_OK) == 0)
    {
        unlink("/tmp/sair_diag_request");
        PLOG_I(TAG, "收到自检请求，生成诊断文件");
        diag_result_t result = diag_run_all();
        char buf[16384];
        int len = diag_result_to_json(&result, buf, sizeof(buf));
        if (len > 0)
        {
            write_file_atomic("/tmp/sair_diag.json", buf, len);
        }
    }

    int fd = open("/tmp/sair_cmd.json", O_RDONLY);
    if (fd < 0)
        return;

    char buf[2048];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return;
    buf[n] = '\0';

    unlink("/tmp/sair_cmd.json");

    PLOG_I(TAG, "收到命令: %s", buf);

    char cmd[32] = {0};
    parse_json_str(buf, "cmd", cmd, sizeof(cmd));

    if (strcmp(cmd, "set_config") == 0)
    {
        char ws_url[512] = {0};
        char ws_token[512] = {0};
        char log_level[16] = {0};

        if (parse_json_str(buf, "ws_url", ws_url, sizeof(ws_url)) == 0 && ws_url[0])
        {
            if (strncmp(ws_url, "wss://", 6) == 0 || strncmp(ws_url, "ws://", 5) == 0)
            {
                memset(g_app.pending_config_buf, 0, sizeof(g_app.pending_config_buf));
                snprintf(g_app.pending_config_buf, sizeof(g_app.pending_config_buf), "ws_url=%s;", ws_url);
                __sync_synchronize();
                g_app.pending_api_config = 1;
                PLOG_I(TAG, "ws_url 配置变更已排队: %s", ws_url);
            }
            else
            {
                PLOG_E(TAG, "无效的 ws_url 格式: %s", ws_url);
            }
        }

        if (parse_json_str(buf, "ws_token", ws_token, sizeof(ws_token)) == 0)
        {
            memset(g_app.config.ws_token, 0, sizeof(g_app.config.ws_token));
            if (ws_token[0])
            {
                strncpy(g_app.config.ws_token, ws_token, sizeof(g_app.config.ws_token) - 1);
                PLOG_I(TAG, "ws_token 配置已更新");
            }
            else
            {
                PLOG_I(TAG, "ws_token 已清空");
            }
            api_server_write_config();
        }

        if (parse_json_str(buf, "log_level", log_level, sizeof(log_level)) == 0 && log_level[0])
        {
            if (strcmp(log_level, "DEBUG") == 0)
                plog_set_level(PLOG_LEVEL_DEBUG);
            else if (strcmp(log_level, "INFO") == 0)
                plog_set_level(PLOG_LEVEL_INFO);
            else if (strcmp(log_level, "WARN") == 0)
                plog_set_level(PLOG_LEVEL_WARN);
            else if (strcmp(log_level, "ERROR") == 0)
                plog_set_level(PLOG_LEVEL_ERROR);
            api_server_write_config();
        }
        {
            int val = 0;
            if (parse_json_int(buf, "listen_timeout", &val) == 0 && val >= 10000 && val <= 600000)
            {
                g_app.listen_timeout_ms = (uint64_t)val;
                PLOG_I(TAG, "listen_timeout 配置已更新: %d", val);
                api_server_write_config();
            }
        }
        {
            int val = 0;
            if (parse_json_int(buf, "session_timeout", &val) == 0 && val >= 30000 && val <= 900000)
            {
                g_app.session_timeout_ms = (uint64_t)val;
                PLOG_I(TAG, "session_timeout 配置已更新: %d", val);
                api_server_write_config();
            }
        }
        {
            int val = 0;
            if (parse_json_int(buf, "wakeup_cooldown", &val) == 0 && val >= 500 && val <= 30000)
            {
                g_app.wakeup_cooldown_ms = (uint64_t)val;
                PLOG_I(TAG, "wakeup_cooldown 配置已更新: %d", val);
                api_server_write_config();
            }
        }
        {
            int val = 0;
            if (parse_json_int(buf, "ws_ping_interval", &val) == 0 && val >= 5000 && val <= 120000)
            {
                g_app.ws_ping_interval_ms = (uint64_t)val;
                PLOG_I(TAG, "ws_ping_interval 配置已更新: %d", val);
                api_server_write_config();
            }
        }
        {
            char mcp_endpoint[512] = {0};
            if (parse_json_str(buf, "mcp_endpoint", mcp_endpoint, sizeof(mcp_endpoint)) == 0)
            {
                memset(g_app.config.mcp_endpoint, 0, sizeof(g_app.config.mcp_endpoint));
                if (mcp_endpoint[0])
                {
                    strncpy(g_app.config.mcp_endpoint, mcp_endpoint, sizeof(g_app.config.mcp_endpoint) - 1);
                    PLOG_I(TAG, "mcp_endpoint 配置已更新: %s", mcp_endpoint);
                }
                else
                {
                    PLOG_I(TAG, "mcp_endpoint 已清空");
                }
                api_server_write_config();
                FILE *mfp = fopen("/var/upgrade/.mcp_endpoint", "w");
                if (mfp)
                {
                    fprintf(mfp, "%s\n", mcp_endpoint);
                    fclose(mfp);
                }
            }
        }
        {
            char mode_str[32] = {0};
            if (parse_json_str(buf, "listening_mode", mode_str, sizeof(mode_str)) == 0 && mode_str[0])
            {
                int new_mode = g_app.listening_mode;
                if (strcmp(mode_str, "realtime") == 0)
                    new_mode = LISTENING_MODE_REALTIME;
                else if (strcmp(mode_str, "autostop") == 0)
                    new_mode = LISTENING_MODE_AUTOSTOP;
                if (new_mode != g_app.listening_mode)
                {
                    g_app.listening_mode = new_mode;
                    PLOG_I(TAG, "listening_mode 配置已更新: %s", mode_str);
                    api_server_write_config();
                    FILE *mfp = fopen("/var/upgrade/.listening_mode", "w");
                    if (mfp)
                    {
                        fprintf(mfp, "%s\n", mode_str);
                        fclose(mfp);
                    }
                }
            }
        }
        {
            char aec_str[32] = {0};
            if (parse_json_str(buf, "aec_mode", aec_str, sizeof(aec_str)) == 0 && aec_str[0])
            {
                int new_aec = (strcmp(aec_str, "cloud") == 0) ? 1 : 0;
                if (new_aec != g_app.aec_mode)
                {
                    g_app.aec_mode = new_aec;
                    PLOG_I(TAG, "aec_mode updated: %s (下次会话生效)", aec_str);
                    api_server_write_config();
                    FILE *afp = fopen("/var/upgrade/.aec_mode", "w");
                    if (afp)
                    {
                        fprintf(afp, "%s\n", new_aec ? "cloud" : "local");
                        fclose(afp);
                    }
                }
            }
        }
        {
            int val = 0;
            if (parse_json_int(buf, "boot_push_disable", &val) == 0 && val >= 0 && val <= 1)
            {
                extern int boot_push_disable_enabled(void);
                FILE *bfp = fopen("/var/upgrade/.boot_push_disable", "w");
                if (bfp)
                {
                    fprintf(bfp, "%d\n", val);
                    fclose(bfp);
                }
                PLOG_I(TAG, "boot_push_disable=%d 已持久化 (60s内或重启后生效)", val);
                api_server_write_config();
            }
        }
        {
            /* 系统音量(原生 sound 库, 0-40): 与语音 volume_up/down 同一通道, 立即生效 */
            int val = -1;
            if (parse_json_int(buf, "volume", &val) == 0 && val >= 0 && val <= 40)
            {
                if (g_app.mcp.sound_set_sys_volume)
                {
                    g_app.mcp.sound_set_sys_volume(val);
                    PLOG_I(TAG, "volume=%d 已设置(原生音量)", val);
                    api_server_write_config();
                }
                else
                {
                    PLOG_W(TAG, "volume 设置失败: 原生 sound 库未加载");
                }
            }
        }
        {
            /* 会话息屏秒数(display_ctrl 域): 与语音 self.screen_off_set 同款持久化 */
            int val = -1;
            if (parse_json_int(buf, "screen_off_idle_sec", &val) == 0 && val >= 0 && val <= 3600)
            {
                char sv[16];
                snprintf(sv, sizeof(sv), "%d", val);
                set_config("SCREEN_OFF_IDLE_SEC", sv, strlen(sv));
                sync_config();
                display_ctrl_reload_config();
                PLOG_I(TAG, "screen_off_idle_sec=%d 已设置(0=不息屏)", val);
                api_server_write_config();
            }
        }
        {
            /* 每日使用时长限制: 开关/上限分钟/临时延迟分钟 (§四) */
            int val = 0;
            if (parse_json_int(buf, "use_limit_enable", &val) == 0 && val >= 0 && val <= 1)
            {
                use_limit_set_enable(val);
                PLOG_I(TAG, "use_limit_enable=%d 已设置", val);
                api_server_write_config();
            }
            if (parse_json_int(buf, "use_limit_minutes", &val) == 0 && val >= 0 && val <= 1440)
            {
                use_limit_set_minutes(val);
                PLOG_I(TAG, "use_limit_minutes=%d 已设置", val);
                api_server_write_config();
            }
            if (parse_json_int(buf, "use_limit_delay_min", &val) == 0 && val > 0 && val <= 720)
            {
                long until = use_limit_request_delay(val);
                PLOG_I(TAG, "use_limit 延迟%d分钟, 到期epoch=%ld", val, until);
                api_server_write_config();
            }
            if (parse_json_int(buf, "use_limit_delay_tool", &val) == 0 && val >= 0 && val <= 1)
            {
                use_limit_set_delay_tool(val);
                PLOG_I(TAG, "use_limit_delay_tool=%d 已设置", val);
                api_server_write_config();
            }
            if (parse_json_int(buf, "use_limit_sched_days", &val) == 0)
            {
                /* 星期位图与时段一起生效(保持 enable/spans 原值) */
                use_limit_set_spans(use_limit_get_sched_enable(), val,
                                    use_limit_get_spans_csv());
                PLOG_I(TAG, "use_limit_sched_days=0x%x 已设置", val);
                api_server_write_config();
            }
            {
                char sv[24] = {0};
                int sen = -1;
                if (parse_json_str(buf, "use_limit_sched", sv, sizeof(sv)) == 0 && sv[0])
                {
                    /* 格式 "enable,days,span1,span2" span 可空: 如 "1,62,1600-2000," */
                    int en = 0, dys = 127;
                    char s1[12] = {0}, s2[12] = {0};
                    if (sscanf(sv, "%d,%d,%11[^,],%11s", &en, &dys, s1, s2) >= 2)
                    {
                        use_limit_set_schedule(en, dys, s1, s2);
                        PLOG_I(TAG, "use_limit_sched='%s' 已设置", sv);
                        api_server_write_config();
                    }
                }
                char svs[64] = {0};
                if (parse_json_str(buf, "use_limit_spans", svs, sizeof(svs)) == 0)
                {
                    /* 新格式(2026-08-31) enable,days,多时段CSV */
                    int en = 0, dys = 127;
                    if (sscanf(svs, "%d,%d", &en, &dys) >= 1)
                    {
                        /* csv = 第二个逗号之后(enable,days 已占前两段) */
                        const char *csvp = strchr(svs, ',');
                        if (csvp) csvp = strchr(csvp + 1, ',');
                        use_limit_set_spans(en, dys, csvp ? csvp + 1 : "");
                        PLOG_I(TAG, "use_limit_spans='%s' 已设置", svs);
                        api_server_write_config();
                    }
                }
                (void)sen;
            }
        }
    }
    else if (strcmp(cmd, "wakeup") == 0)
    {
        __sync_synchronize();
        g_app.pending_api_wakeup = 1;
        PLOG_I(TAG, "唤醒命令已排队");
    }
    else if (strcmp(cmd, "abort") == 0)
    {
        __sync_synchronize();
        g_app.pending_api_abort = 1;
        PLOG_I(TAG, "中止命令已排队");
    }
    else if (strcmp(cmd, "activate") == 0)
    {
        __sync_synchronize();
        g_app.pending_api_activate = 1;
        PLOG_I(TAG, "激活命令已排队");
    }
    else if (strcmp(cmd, "upgrade") == 0)
    {
        extern volatile sig_atomic_t g_hot_update_pending;
        g_hot_update_pending = 1;
        PLOG_I(TAG, "热更新命令已排队 (g_hot_update_pending=1)");
    }
    else if (strcmp(cmd, "play_prompt") == 0)
    {
        PLOG_I(TAG, "收到提示音播放命令(调试入口)");
        extern void use_limit_play_prompt(void);
        use_limit_play_prompt();
    }
    else if (strcmp(cmd, "poweroff") == 0)
    {
        /* 电池插件低电自护: 原生低电链实测不触发(2026-08-30 深放3.12V硬断电),
         * 由插件检测低电后经此命令走优雅关机链(动画+提示音) */
        PLOG_W(TAG, "收到低电关机命令, 走优雅关机链");
        extern int platform_power_shutdown_elegant(void);
        platform_power_shutdown_elegant();
    }
    else
    {
        PLOG_W(TAG, "未知命令: %s", cmd);
    }
}

int api_server_start(void)
{
    api_server_write_status();
    api_server_write_config();
    PLOG_I(TAG, "文件IPC接口已启动 (状态/配置文件已写入)");
    return 0;
}

void api_server_stop(void)
{
    unlink("/tmp/sair_status.json");
    unlink("/tmp/sair_config.json");
    unlink("/tmp/sair_diag.json");
    unlink("/tmp/sair_cmd.json");
    unlink("/tmp/sair_diag_request");
}
