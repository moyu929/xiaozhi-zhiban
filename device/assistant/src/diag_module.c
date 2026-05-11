/**
 * @file diag_module.c
 * @brief 运行时健康检查模块
 *
 * 检查 assistant 运行时的健康状态，包括：
 * - 配置文件读写、日志系统、看门狗
 * - WiFi连接、磁盘空间、WebSocket连接状态
 *
 * 部署前置环境检查已移至 xwebd 的 /api/assistant/env 接口。
 */

#include "diag_module.h"
#include "app_context.h"
#include "protocol_handler.h"
#include "plog.h"
#include "xiaozhi_config.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#define TAG "DIAG"
#define DIAG_TEMP_FILE "/var/upgrade/.diag_test_tmp"

static void diag_add(diag_result_t *r, const char *name, int ok, const char *message)
{
    if (r->count >= DIAG_MAX_ITEMS)
    {
        PLOG_W(TAG, "自检项超过上限 %d，跳过: %s", DIAG_MAX_ITEMS, name);
        return;
    }
    r->items[r->count].name = name;
    r->items[r->count].ok = ok;
    r->items[r->count].message = message;
    r->count++;
}

static void check_websocket(diag_result_t *r)
{
    extern app_context_t g_app;
    if (g_app.proto_initialized && protocol_handler_is_connected(&g_app.proto))
    {
        diag_add(r, "WebSocket", 1, "已连接云端");
    }
    else if (g_app.proto_initialized)
    {
        diag_add(r, "WebSocket", 0, "未连接云端");
    }
    else
    {
        diag_add(r, "WebSocket", 0, "协议未初始化");
    }
}

static void check_config_dir(diag_result_t *r)
{
    const char *test_path = DIAG_TEMP_FILE;
    const char *test_data = "diag_test";
    int fd = open(test_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        diag_add(r, "配置文件", 0, "配置文件读写失败: 无法创建文件");
        return;
    }
    if (write(fd, test_data, strlen(test_data)) < 0)
    {
        close(fd);
        unlink(test_path);
        diag_add(r, "配置文件", 0, "配置文件读写失败: 写入失败");
        return;
    }
    close(fd);

    char read_buf[32] = {0};
    fd = open(test_path, O_RDONLY);
    if (fd < 0)
    {
        unlink(test_path);
        diag_add(r, "配置文件", 0, "配置文件读写失败: 无法读取文件");
        return;
    }
    int n = read(fd, read_buf, sizeof(read_buf) - 1);
    close(fd);
    unlink(test_path);

    if (n > 0 && strcmp(read_buf, test_data) == 0)
    {
        diag_add(r, "配置文件", 1, "配置文件读写正常");
    }
    else
    {
        diag_add(r, "配置文件", 0, "配置文件读写失败: 内容校验不一致");
    }
}

static void check_log(diag_result_t *r)
{
    int fd = open(PLOG_PATH, O_WRONLY | O_APPEND);
    if (fd >= 0)
    {
        close(fd);
        diag_add(r, "日志系统", 1, "日志系统正常");
    }
    else
    {
        diag_add(r, "日志系统", 0, "日志系统异常");
    }
}

static void check_watchdog(diag_result_t *r)
{
    if (access("/usr/lib/libapplib.so", R_OK) == 0)
    {
        diag_add(r, "看门狗", 1, "看门狗接口库可访问");
    }
    else
    {
        diag_add(r, "看门狗", 0, "看门狗接口库不可访问");
    }
}

static void check_wifi(diag_result_t *r)
{
    int fd = open("/proc/net/wireless", O_RDONLY);
    if (fd < 0)
    {
        diag_add(r, "WiFi", 0, "WiFi未连接");
        return;
    }
    char line[256];
    int has_content = 0;
    int pos = 0;
    int n;
    while ((n = read(fd, line + pos, 1)) > 0 && pos < (int)sizeof(line) - 2)
    {
        if (line[pos] == '\n')
        {
            line[pos] = '\0';
            if (pos > 10 && line[0] != 'I' && line[0] != ' ')
            {
                has_content = 1;
                break;
            }
            pos = 0;
        }
        else
        {
            pos++;
        }
    }
    close(fd);
    if (has_content)
    {
        diag_add(r, "WiFi", 1, "WiFi已连接");
    }
    else
    {
        diag_add(r, "WiFi", 0, "WiFi未连接");
    }
}

static void check_disk(diag_result_t *r)
{
    struct statvfs vfs;
    if (statvfs("/var/upgrade", &vfs) != 0)
    {
        diag_add(r, "磁盘空间", 0, "磁盘空间检查失败");
        return;
    }
    unsigned long long free_kb = (unsigned long long)vfs.f_bavail * vfs.f_bsize / 1024;
    if (free_kb > 1024)
    {
        static char disk_ok_msg[64];
        snprintf(disk_ok_msg, sizeof(disk_ok_msg), "磁盘空间充足(%lluKB可用)", free_kb);
        diag_add(r, "磁盘空间", 1, disk_ok_msg);
    }
    else
    {
        static char disk_fail_msg[64];
        snprintf(disk_fail_msg, sizeof(disk_fail_msg), "磁盘空间不足(仅%lluKB可用)", free_kb);
        diag_add(r, "磁盘空间", 0, disk_fail_msg);
    }
}

diag_result_t diag_run_all(void)
{
    diag_result_t result;
    memset(&result, 0, sizeof(result));

    PLOG_I(TAG, "开始执行运行时健康检查...");

    check_config_dir(&result);
    check_log(&result);
    check_watchdog(&result);
    check_wifi(&result);
    check_disk(&result);
    check_websocket(&result);

    int ok_count = 0, fail_count = 0;
    for (int i = 0; i < result.count; i++)
    {
        if (result.items[i].ok)
            ok_count++;
        else
            fail_count++;
    }
    snprintf(result.summary, sizeof(result.summary), "%d项正常, %d项异常", ok_count, fail_count);

    PLOG_I(TAG, "运行时健康检查完成: %s", result.summary);
    return result;
}

static void json_escape(const char *src, char *dst, int dst_size)
{
    int j = 0;
    for (int i = 0; src[i] && j < dst_size - 2; i++)
    {
        if (src[i] == '"' || src[i] == '\\')
        {
            if (j + 2 >= dst_size)
                break;
            dst[j++] = '\\';
            dst[j++] = src[i];
        }
        else if (src[i] == '\n')
        {
            if (j + 2 >= dst_size)
                break;
            dst[j++] = '\\';
            dst[j++] = 'n';
        }
        else if (src[i] == '\r')
        {
            if (j + 2 >= dst_size)
                break;
            dst[j++] = '\\';
            dst[j++] = 'r';
        }
        else
        {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
}

int diag_result_to_json(const diag_result_t *result, char *buf, int buf_size)
{
    int ok_count = 0, fail_count = 0;
    for (int i = 0; i < result->count; i++)
    {
        if (result->items[i].ok)
            ok_count++;
        else
            fail_count++;
    }

    int pos = 0;
    pos += snprintf(buf + pos, buf_size - pos,
                    "{\"ok_count\":%d,\"fail_count\":%d,\"total\":%d,\"summary\":\"%s\",\"items\":[",
                    ok_count, fail_count, result->count, result->summary);

    for (int i = 0; i < result->count; i++)
    {
        if (i > 0)
            pos += snprintf(buf + pos, buf_size - pos, ",");
        char esc_msg[256];
        json_escape(result->items[i].message, esc_msg, sizeof(esc_msg));
        pos += snprintf(buf + pos, buf_size - pos,
                        "{\"name\":\"%s\",\"ok\":%s,\"message\":\"%s\"}",
                        result->items[i].name,
                        result->items[i].ok ? "true" : "false",
                        esc_msg);
        if (pos >= buf_size - 1)
            break;
    }

    pos += snprintf(buf + pos, buf_size - pos, "]}");
    if (pos >= buf_size)
        pos = buf_size - 1;
    return pos;
}
