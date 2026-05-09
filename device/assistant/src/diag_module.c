/**
 * @file diag_module.c
 * @brief 自检（诊断）模块实现
 *
 * 当 Assistant 首次部署到设备时，依次检查各功能模块是否正常工作，
 * 并报告哪些正常、哪些异常及原因。检查项包括：
 * - applib框架、ASR引擎库、音频服务库、音频录制库
 * - WebSocket、配置文件、日志系统
 * - 看门狗、触摸按键、WiFi、磁盘空间
 *
 * 注意：此模块在API服务器线程中执行，不能调用malloc/strdup/fopen
 * 等会触发uClibc malloc全局锁的函数，否则会与主线程竞争导致崩溃。
 */

#include "diag_module.h"
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

static void diag_add(diag_result_t *r, const char *name, int ok, const char *message) {
    if (r->count >= 16) return;
    r->items[r->count].name = name;
    r->items[r->count].ok = ok;
    r->items[r->count].message = message;
    r->count++;
}

static int lib_exists(const char *name) {
    char path[256];
    snprintf(path, sizeof(path), "/usr/lib/%s", name);
    if (access(path, F_OK) == 0) return 1;
    snprintf(path, sizeof(path), "/lib/%s", name);
    if (access(path, F_OK) == 0) return 1;
    return 0;
}

static void check_applib(diag_result_t *r) {
    if (lib_exists("libapplib.so")) {
        diag_add(r, "applib框架", 1, "applib框架库文件存在");
    } else {
        diag_add(r, "applib框架", 0, "applib框架库文件不存在");
    }
}

static void check_asr(diag_result_t *r) {
    if (lib_exists("libsair_asr.so")) {
        diag_add(r, "ASR引擎库", 1, "ASR引擎库文件存在");
    } else {
        diag_add(r, "ASR引擎库", 0, "ASR引擎库文件不存在");
    }
}

static void check_audio_service(diag_result_t *r) {
    if (lib_exists("libaudio_service_api.so")) {
        diag_add(r, "音频服务库", 1, "音频服务库文件存在");
    } else {
        diag_add(r, "音频服务库", 0, "音频服务库文件不存在");
    }
}

static void check_audio_recorder(diag_result_t *r) {
    if (lib_exists("libaudio_recorder.so")) {
        diag_add(r, "音频录制库", 1, "音频录制库文件存在");
    } else {
        diag_add(r, "音频录制库", 0, "音频录制库文件不存在");
    }
}

static void check_tls(diag_result_t *r) {
    if (lib_exists("libmbedtls.so") && lib_exists("libmbedcrypto.so")) {
        diag_add(r, "TLS/SSL", 1, "TLS加密库文件存在");
    } else {
        diag_add(r, "TLS/SSL", 0, "TLS加密库文件不存在");
    }
}

static void check_websocket(diag_result_t *r) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
        close(fd);
        diag_add(r, "WebSocket", 1, "网络套接字创建正常");
    } else {
        diag_add(r, "WebSocket", 0, "网络套接字创建失败");
    }
}

static void check_config_dir(diag_result_t *r) {
    const char *test_path = DIAG_TEMP_FILE;
    const char *test_data = "diag_test";
    int fd = open(test_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        diag_add(r, "配置文件", 0, "配置文件读写失败: 无法创建文件");
        return;
    }
    if (write(fd, test_data, strlen(test_data)) < 0) {
        close(fd);
        unlink(test_path);
        diag_add(r, "配置文件", 0, "配置文件读写失败: 写入失败");
        return;
    }
    close(fd);

    char read_buf[32] = {0};
    fd = open(test_path, O_RDONLY);
    if (fd < 0) {
        unlink(test_path);
        diag_add(r, "配置文件", 0, "配置文件读写失败: 无法读取文件");
        return;
    }
    int n = read(fd, read_buf, sizeof(read_buf) - 1);
    close(fd);
    unlink(test_path);

    if (n > 0 && strcmp(read_buf, test_data) == 0) {
        diag_add(r, "配置文件", 1, "配置文件读写正常");
    } else {
        diag_add(r, "配置文件", 0, "配置文件读写失败: 内容校验不一致");
    }
}

static void check_log(diag_result_t *r) {
    int fd = open(PLOG_PATH, O_WRONLY | O_APPEND);
    if (fd >= 0) {
        close(fd);
        diag_add(r, "日志系统", 1, "日志系统正常");
    } else {
        diag_add(r, "日志系统", 0, "日志系统异常");
    }
}

static void check_watchdog(diag_result_t *r) {
    if (lib_exists("libapplib.so")) {
        diag_add(r, "看门狗", 1, "看门狗接口库文件存在");
    } else {
        diag_add(r, "看门狗", 0, "看门狗接口库文件不存在");
    }
}

static void check_touch_key(diag_result_t *r) {
    if (access("/dev/input/event2", F_OK) == 0) {
        diag_add(r, "触摸按键", 1, "触摸按键设备正常");
    } else {
        diag_add(r, "触摸按键", 0, "触摸按键设备未找到");
    }
}

static void check_wifi(diag_result_t *r) {
    int fd = open("/proc/net/wireless", O_RDONLY);
    if (fd < 0) {
        diag_add(r, "WiFi", 0, "WiFi未连接");
        return;
    }
    char line[256];
    int has_content = 0;
    int pos = 0;
    int n;
    while ((n = read(fd, line + pos, 1)) > 0 && pos < (int)sizeof(line) - 2) {
        if (line[pos] == '\n') {
            line[pos] = '\0';
            if (pos > 10 && line[0] != 'I' && line[0] != ' ') {
                has_content = 1;
                break;
            }
            pos = 0;
        } else {
            pos++;
        }
    }
    close(fd);
    if (has_content) {
        diag_add(r, "WiFi", 1, "WiFi已连接");
    } else {
        diag_add(r, "WiFi", 0, "WiFi未连接");
    }
}

static void check_disk(diag_result_t *r) {
    struct statvfs vfs;
    if (statvfs("/var/upgrade", &vfs) != 0) {
        diag_add(r, "磁盘空间", 0, "磁盘空间检查失败");
        return;
    }
    unsigned long long free_kb = (unsigned long long)vfs.f_bavail * vfs.f_bsize / 1024;
    if (free_kb > 1024) {
        static char disk_msg[64];
        snprintf(disk_msg, sizeof(disk_msg), "磁盘空间充足(%lluKB可用)", free_kb);
        diag_add(r, "磁盘空间", 1, disk_msg);
    } else {
        static char disk_msg[64];
        snprintf(disk_msg, sizeof(disk_msg), "磁盘空间不足(仅%lluKB可用)", free_kb);
        diag_add(r, "磁盘空间", 0, disk_msg);
    }
}

diag_result_t diag_run_all(void) {
    diag_result_t result;
    memset(&result, 0, sizeof(result));

    PLOG_I(TAG, "开始执行自检...");

    check_applib(&result);
    check_asr(&result);
    check_audio_service(&result);
    check_audio_recorder(&result);
    check_tls(&result);
    check_websocket(&result);
    check_config_dir(&result);
    check_log(&result);
    check_watchdog(&result);
    check_touch_key(&result);
    check_wifi(&result);
    check_disk(&result);

    int ok_count = 0, fail_count = 0;
    for (int i = 0; i < result.count; i++) {
        if (result.items[i].ok) ok_count++;
        else fail_count++;
    }
    snprintf(result.summary, sizeof(result.summary), "%d项正常, %d项异常", ok_count, fail_count);

    PLOG_I(TAG, "自检完成: %s", result.summary);
    return result;
}

static void json_escape(const char *src, char *dst, int dst_size) {
    int j = 0;
    for (int i = 0; src[i] && j < dst_size - 2; i++) {
        if (src[i] == '"' || src[i] == '\\') {
            if (j + 2 >= dst_size) break;
            dst[j++] = '\\';
            dst[j++] = src[i];
        } else if (src[i] == '\n') {
            if (j + 2 >= dst_size) break;
            dst[j++] = '\\';
            dst[j++] = 'n';
        } else if (src[i] == '\r') {
            if (j + 2 >= dst_size) break;
            dst[j++] = '\\';
            dst[j++] = 'r';
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
}

int diag_result_to_json(const diag_result_t *result, char *buf, int buf_size) {
    int ok_count = 0, fail_count = 0;
    for (int i = 0; i < result->count; i++) {
        if (result->items[i].ok) ok_count++;
        else fail_count++;
    }

    int pos = 0;
    pos += snprintf(buf + pos, buf_size - pos,
        "{\"ok_count\":%d,\"fail_count\":%d,\"total\":%d,\"summary\":\"%s\",\"items\":[",
        ok_count, fail_count, result->count, result->summary);

    for (int i = 0; i < result->count; i++) {
        if (i > 0) pos += snprintf(buf + pos, buf_size - pos, ",");
        char esc_msg[256];
        json_escape(result->items[i].message, esc_msg, sizeof(esc_msg));
        pos += snprintf(buf + pos, buf_size - pos,
            "{\"name\":\"%s\",\"ok\":%s,\"message\":\"%s\"}",
            result->items[i].name,
            result->items[i].ok ? "true" : "false",
            esc_msg);
        if (pos >= buf_size - 1) break;
    }

    pos += snprintf(buf + pos, buf_size - pos, "]}");
    if (pos >= buf_size) pos = buf_size - 1;
    return pos;
}
