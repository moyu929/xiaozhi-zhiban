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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#define TAG "IPC"

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
    char buf[512];
    int len = snprintf(buf, sizeof(buf),
        "{\"state\":\"%s\",\"version\":\"%s\",\"activation_code\":\"%s\",\"activated\":%s}\n",
        state_to_string(state),
        XIAOZHI_VERSION,
        g_app.config.activation_code,
        g_app.config.has_ws_config ? "true" : "false");
    write_file_atomic("/tmp/sair_status.json", buf, len);
}

void api_server_write_config(void)
{
    int plog_lvl = plog_get_level();
    char buf[1536];
    int len = snprintf(buf, sizeof(buf),
        "{\"ws_url\":\"%s\",\"ws_token\":\"%s\",\"log_level\":\"%s\","
        "\"listen_timeout\":%llu,\"session_timeout\":%llu,"
        "\"wakeup_cooldown\":%llu,\"ws_ping_interval\":%llu,"
        "\"mcp_endpoint\":\"%s\"}\n",
        g_app.config.ws_url,
        g_app.config.ws_token,
        plog_lvl == PLOG_LEVEL_DEBUG ? "DEBUG" :
        plog_lvl == PLOG_LEVEL_INFO  ? "INFO" :
        plog_lvl == PLOG_LEVEL_WARN  ? "WARN" : "ERROR",
        (unsigned long long)g_app.listen_timeout_ms,
        (unsigned long long)g_app.session_timeout_ms,
        (unsigned long long)g_app.wakeup_cooldown_ms,
        (unsigned long long)g_app.ws_ping_interval_ms,
        g_app.config.mcp_endpoint);
    write_file_atomic("/tmp/sair_config.json", buf, len);
}

static int parse_json_str(const char *json, const char *key, char *out, int out_size)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return -1;
    p += strlen(search);
    while (*p && (*p == ' ' || *p == ':' || *p == '\t'))
        p++;
    if (*p != '"') return -1;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < out_size - 1)
    {
        if (*p == '\\' && *(p + 1))
        {
            p++;
            out[i++] = *p++;
        }
        else
        {
            out[i++] = *p++;
        }
    }
    out[i] = '\0';
    return 0;
}

static int parse_json_int(const char *json, const char *key, int *out)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return -1;
    p += strlen(search);
    while (*p && (*p == ' ' || *p == ':' || *p == '\t'))
        p++;
    *out = atoi(p);
    return 0;
}

void api_server_check_commands(void)
{
    if (access("/tmp/sair_diag_request", F_OK) == 0)
    {
        unlink("/tmp/sair_diag_request");
        PLOG_I(TAG, "收到自检请求，生成诊断文件");
        diag_result_t result = diag_run_all();
        char buf[4096];
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
        PLOG_I(TAG, "热更新命令已排队");
        kill(getpid(), SIGUSR2);
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
