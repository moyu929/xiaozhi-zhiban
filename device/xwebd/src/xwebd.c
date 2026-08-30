/**
 * xwebd.c - xwebd HTTP服务器主文件
 *
 * xwebd是一个轻量级嵌入式HTTP服务器，提供设备管理RESTful API，主要功能包括：
 * - 系统信息查询（CPU、内存、磁盘、电池、WiFi等）
 * - 音量/亮度/静音等硬件控制
 * - 文件管理（上传、下载、删除、批量删除、清理）
 * - 助手程序(sair)的部署、更新、卸载、状态查询、日志管理
 * - 系统电源控制（关机、重启）
 * - 服务状态查询（telnet、看门狗、自启动）
 * - 日志查看与配置管理
 *
 * 架构: 主进程(看门狗) -> 工作进程(HTTP服务器)，看门狗负责监控和自动重启工作进程
 * 注意: sair是Assistant(助手)的二进制文件名，受平台约束不可改名
 */
#include "xwebd_config.h"
#include "plugin_gateway.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <sched.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <poll.h>
#include <ctype.h>
#include <dirent.h>
#include <limits.h>

/* ===== 类型定义 ===== */

#define APPEND_PRINTF(buf, pos, size, ...) do { \
    if ((pos) < (size) - 1) { \
        (pos) += snprintf((buf) + (pos), (size) - (pos), __VA_ARGS__); \
        if ((pos) >= (size)) (pos) = (size) - 1; \
    } \
} while(0)

typedef int (*route_handler_t)(int, const char *, const char *);

typedef struct {
    const char *method;
    const char *path;
    route_handler_t handler;
} route_t;

#define BL_BRIGHTNESS_DEFVAL 200


/* ===== 全局变量 ===== */

#define TAG "XWD"

static volatile int g_running = 1;
static int g_server_fd = -1;
static int g_port = XWEBD_DEFAULT_PORT;
static int g_daemon = 0;

static int g_upload_max_mb = XWEBD_UPLOAD_MAX_MB_DEFAULT;
static volatile pid_t g_upload_pid = 0;

static int g_persist_usb_lun = -1;
static int g_persist_telnet = -1;
static int g_persist_led = -1;
static int g_persist_precache = 0;
static char g_persist_custom_ws_url[512] = "";

static int g_plog_fd = -1;
static int g_plog_level = 2;
static pthread_mutex_t g_plog_mutex = PTHREAD_MUTEX_INITIALIZER;
static off_t g_plog_size = 0;
static char g_plog_path[256] = XWEBD_LOG_PATH;
static struct timespec g_plog_last_sync = {0, 0};

static int g_watchdog_crash_count = 0;
static time_t g_watchdog_crash_start = 0;

static int g_cached_cpu_usage = -1;
static long long g_cpu_prev_user = 0, g_cpu_prev_nice = 0, g_cpu_prev_sys = 0, g_cpu_prev_idle = 0;
static int g_cpu_first_read = 1;

static int g_led_pending_value = -1;
static time_t g_led_retry_time = 0;

/* ===== 日志系统 ===== */

#define XLOG_E(tag, fmt, ...) xlog_write(0, tag, fmt, ##__VA_ARGS__)
#define XLOG_W(tag, fmt, ...) xlog_write(1, tag, fmt, ##__VA_ARGS__)
#define XLOG_I(tag, fmt, ...) xlog_write(2, tag, fmt, ##__VA_ARGS__)
#define XLOG_D(tag, fmt, ...) xlog_write(3, tag, fmt, ##__VA_ARGS__)

static const char *level_names[] = {"E", "W", "I", "D"};

static void xlog_rotate(void) {
    if (g_plog_size < XWEBD_LOG_MAX_SIZE) return;
    close(g_plog_fd);
    g_plog_fd = -1;

    int src_fd = open(g_plog_path, O_RDONLY);
    if (src_fd < 0) {
        g_plog_fd = open(g_plog_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        g_plog_size = 0;
        return;
    }

    off_t file_size = lseek(src_fd, 0, SEEK_END);
    off_t keep_offset = 0;
    if (file_size > XWEBD_LOG_KEEP_SIZE) keep_offset = file_size - XWEBD_LOG_KEEP_SIZE;

    char *buf = malloc(XWEBD_LOG_KEEP_SIZE);
    if (!buf) {
        close(src_fd);
        g_plog_fd = open(g_plog_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        g_plog_size = 0;
        return;
    }

    lseek(src_fd, keep_offset, SEEK_SET);
    ssize_t total_read = 0;
    while (total_read < XWEBD_LOG_KEEP_SIZE) {
        ssize_t n = read(src_fd, buf + total_read, XWEBD_LOG_KEEP_SIZE - total_read);
        if (n <= 0) break;
        total_read += n;
    }
    close(src_fd);

    char *start = buf;
    if (total_read > 0 && keep_offset > 0) {
        char *nl = memchr(buf, '\n', total_read);
        if (nl) { start = nl + 1; total_read -= (start - buf); }
    }

    g_plog_fd = open(g_plog_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (g_plog_fd >= 0 && total_read > 0) {
        write(g_plog_fd, start, total_read);
        g_plog_size = total_read;
    } else {
        g_plog_size = 0;
    }
    free(buf);
}

static void xlog_init(const char *path) {
    pthread_mutex_lock(&g_plog_mutex);
    if (g_plog_fd >= 0) close(g_plog_fd);
    if (path) { strncpy(g_plog_path, path, sizeof(g_plog_path) - 1); g_plog_path[sizeof(g_plog_path) - 1] = '\0'; }
    g_plog_fd = open(g_plog_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (g_plog_fd >= 0) {
        struct stat st;
        if (fstat(g_plog_fd, &st) == 0) g_plog_size = st.st_size;
        else g_plog_size = 0;
    }
    pthread_mutex_unlock(&g_plog_mutex);
}

static void xlog_close(void) {
    pthread_mutex_lock(&g_plog_mutex);
    if (g_plog_fd >= 0) { fsync(g_plog_fd); close(g_plog_fd); g_plog_fd = -1; }
    pthread_mutex_unlock(&g_plog_mutex);
}

static void xlog_vwrite(int level, const char *tag, const char *fmt, va_list ap) {
    if (level > g_plog_level) return;
    if (g_plog_fd < 0) return;

    char buf[512];
    int len = 0;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_buf;
    localtime_r(&ts.tv_sec, &tm_buf);

    len = snprintf(buf, sizeof(buf), "[%02d:%02d:%02d][%s][%s] ",
                   tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                   (level >= 0 && level <= 3) ? level_names[level] : "?",
                   tag ? tag : "");

    if (len > 0 && (size_t)len < sizeof(buf)) {
        int remain = sizeof(buf) - len - 2;
        int fmt_len = vsnprintf(buf + len, remain, fmt, ap);
        if (fmt_len > 0) len += (fmt_len < remain) ? fmt_len : remain;
    }
    if (len > 0 && (size_t)len < sizeof(buf)) buf[len++] = '\n';

    pthread_mutex_lock(&g_plog_mutex);
    if (g_plog_fd >= 0) {
        xlog_rotate();
        int w = write(g_plog_fd, buf, len);
        if (w > 0) g_plog_size += w;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - g_plog_last_sync.tv_sec >= 1) {
            fdatasync(g_plog_fd);
            g_plog_last_sync = now;
        }
    }
    pthread_mutex_unlock(&g_plog_mutex);
}

int xlog_write(int level, const char *tag, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    xlog_vwrite(level, tag, fmt, ap);
    va_end(ap);
    return 0;
}

/* ===== CPU增量采样(无需线程) ===== */

static void cpu_sample_update(void) {
    FILE *sf = fopen("/proc/stat", "r");
    if (!sf) return;
    long long user, nice, sys, idle;
    if (fscanf(sf, "cpu %lld %lld %lld %lld", &user, &nice, &sys, &idle) == 4) {
        if (g_cpu_first_read) {
            g_cpu_first_read = 0;
        } else {
            long long d_total = (user - g_cpu_prev_user) + (nice - g_cpu_prev_nice) + (sys - g_cpu_prev_sys) + (idle - g_cpu_prev_idle);
            long long d_idle = idle - g_cpu_prev_idle;
            if (d_total > 0)
                g_cached_cpu_usage = (int)(100 * (d_total - d_idle) / d_total);
        }
        g_cpu_prev_user = user; g_cpu_prev_nice = nice; g_cpu_prev_sys = sys; g_cpu_prev_idle = idle;
    }
    fclose(sf);
}

/* ===== 进程查找(替代popen pidof) ===== */

static pid_t find_pid_by_name(const char *name) {
    DIR *d = opendir("/proc");
    if (!d) return 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_type != DT_DIR) continue;
        char *end;
        long pid = strtol(ent->d_name, &end, 10);
        if (*end != '\0' || pid <= 0) continue;
        char comm_path[64];
        snprintf(comm_path, sizeof(comm_path), "/proc/%ld/comm", pid);
        char comm[256];
        int fd = open(comm_path, O_RDONLY);
        if (fd < 0) continue;
        int n = read(fd, comm, sizeof(comm) - 1);
        close(fd);
        if (n <= 0) continue;
        comm[n] = '\0';
        if (comm[n - 1] == '\n') comm[n - 1] = '\0';
        if (strcmp(comm, name) == 0) { closedir(d); return (pid_t)pid; }
    }
    closedir(d);
    return 0;
}

static pid_t find_pid_by_cmdline(const char *substr) {
    DIR *d = opendir("/proc");
    if (!d) return 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_type != DT_DIR) continue;
        char *end;
        long pid = strtol(ent->d_name, &end, 10);
        if (*end != '\0' || pid <= 0) continue;
        char cmdline_path[64];
        snprintf(cmdline_path, sizeof(cmdline_path), "/proc/%ld/cmdline", pid);
        char cmdline[512];
        int fd = open(cmdline_path, O_RDONLY);
        if (fd < 0) continue;
        int n = read(fd, cmdline, sizeof(cmdline) - 1);
        close(fd);
        if (n <= 0) continue;
        cmdline[n] = '\0';
        for (int i = 0; i < n - 1; i++) { if (cmdline[i] == '\0') cmdline[i] = ' '; }
        if (strstr(cmdline, substr)) { closedir(d); return (pid_t)pid; }
    }
    closedir(d);
    return 0;
}

/* ===== 呼吸灯控制 ===== */

static int send_led_cmd(int enable) {
    if (access("/tmp/service/mqtt_custom_server", F_OK) != 0) {
        XLOG_W(TAG, "呼吸灯控制: mqtt_custom_server socket不存在");
        return -1;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        XLOG_W(TAG, "呼吸灯控制: 创建socket失败: %s", strerror(errno));
        return -1;
    }
    struct timeval tv = {3, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    char my_path[128];
    snprintf(my_path, sizeof(my_path), "/tmp/service/xwebd_led_%d", getpid());
    unlink(my_path);
    strncpy(addr.sun_path, my_path, sizeof(addr.sun_path) - 1);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        XLOG_W(TAG, "呼吸灯控制: bind失败: %s", strerror(errno));
        close(fd);
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/tmp/service/mqtt_custom_server", sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        XLOG_W(TAG, "呼吸灯控制: connect失败: %s", strerror(errno));
        close(fd);
        unlink(my_path);
        return -1;
    }
    struct {
        int msg_type;
        int need_resp;
        int unknown;
        int data_size;
        int data;
    } msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_type = 0x2cec;
    msg.need_resp = 1;
    msg.unknown = 0;
    msg.data_size = 4;
    msg.data = enable;
    int ret = send(fd, &msg, sizeof(msg), 0);
    if (ret != sizeof(msg)) {
        XLOG_W(TAG, "呼吸灯控制: send失败: ret=%d expected=%d (%s)", ret, (int)sizeof(msg), strerror(errno));
        close(fd);
        unlink(my_path);
        return -1;
    }
    if (msg.need_resp) {
        char resp[256];
        int nr = recv(fd, resp, sizeof(resp), 0);
        if (nr <= 0) {
            XLOG_W(TAG, "呼吸灯控制: recv失败: %s", strerror(errno));
        } else {
            XLOG_I(TAG, "呼吸灯控制: 收到响应(%d字节)", nr);
        }
    }
    close(fd);
    unlink(my_path);
    XLOG_I(TAG, "呼吸灯控制: 已发送0x2cec消息 enable=%d -> mqtt_custom_server", enable);
    return 0;
}

/* ===== HTTP工具函数 ===== */

int send_response(int fd, int status, const char *content_type, const char *body, int body_len) {
    const char *status_text;
    switch (status) {
        case 200: status_text = "OK"; break;
        case 400: status_text = "Bad Request"; break;
        case 404: status_text = "Not Found"; break;
        case 503: status_text = "Service Unavailable"; break;
        case 500: status_text = "Internal Server Error"; break;
        default: status_text = "Unknown"; break;
    }
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "%s"
        "\r\n",
        status, status_text, content_type, body_len,
        status == 503 ? "Retry-After: 5\r\n" : "");
    if (send(fd, header, hlen, MSG_NOSIGNAL) < 0) return -1;
    if (body_len > 0 && body && send(fd, body, body_len, MSG_NOSIGNAL) < 0) return -1;
    return 0;
}

int send_json(int fd, int status, const char *json) {
    return send_response(fd, status, "application/json", json, strlen(json));
}

int send_error(int fd, int code, const char *message) {
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", message);
    return send_json(fd, code, buf);
}

static int parse_json_int(const char *json, const char *key, int *out) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return -1;
    p += strlen(search);
    while (*p && (*p == ' ' || *p == ':' || *p == '\t')) p++;
    *out = atoi(p);
    return 0;
}

static int parse_json_str(const char *json, const char *key, char *out, int out_size) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return -1;
    p += strlen(search);
    while (*p && (*p == ' ' || *p == ':' || *p == '\t')) p++;
    if (*p != '"') return -1;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < out_size - 1) {
        if (*p == '\\' && *(p + 1)) { p++; out[i++] = *p++; }
        else out[i++] = *p++;
    }
    out[i] = '\0';
    return 0;
}

static int parse_json_bool(const char *json, const char *key) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return -1;
    p += strlen(search);
    while (*p && (*p == ' ' || *p == ':' || *p == '\t')) p++;
    if (strncmp(p, "true", 4) == 0) return 1;
    if (strncmp(p, "false", 5) == 0) return 0;
    return -1;
}

static int hex_to_int(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int url_decode(const char *src, char *dst, int dst_size) {
    int i = 0;
    while (*src && i < dst_size - 1) {
        if (*src == '%' && isxdigit((unsigned char)*(src + 1)) && isxdigit((unsigned char)*(src + 2))) {
            dst[i++] = (char)(hex_to_int(*(src + 1)) * 16 + hex_to_int(*(src + 2)));
            src += 3;
        } else if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
    return i;
}

static int validate_path(const char *raw_path, char *resolved, size_t resolved_size) {
    char decoded[PATH_MAX];
    url_decode(raw_path, decoded, sizeof(decoded));

    if (strstr(decoded, "..")) return -1;

    char real_resolved[PATH_MAX];
    if (realpath(decoded, real_resolved) == NULL) {
        if (errno != ENOENT) return -1;
        strncpy(resolved, decoded, resolved_size - 1);
        resolved[resolved_size - 1] = '\0';
    } else {
        strncpy(resolved, real_resolved, resolved_size - 1);
        resolved[resolved_size - 1] = '\0';
    }

    if (strncmp(resolved, XWEBD_BASE_DIR, XWEBD_BASE_DIR_LEN) != 0) return -1;
    char next_char = resolved[XWEBD_BASE_DIR_LEN];
    if (next_char != '/' && next_char != '\0') return -1;

    return 0;
}

static int read_file_string(const char *path, char *buf, int buf_size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int n = read(fd, buf, buf_size - 1);
    close(fd);
    if (n < 0) return -1;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) n--;
    buf[n] = '\0';
    return n;
}

static int read_proc_line(const char *path, char *buf, int size) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int ok = (fgets(buf, size, f) != NULL);
    fclose(f);
    if (ok) { char *nl = strchr(buf, '\n'); if (nl) *nl = '\0'; }
    return ok ? 0 : -1;
}

/* ===== 文件辅助 ===== */


/* ===== 诊断辅助 ===== */

static int json_escape(const char *src, char *dst, int dst_size) {
    int j = 0;
    for (int i = 0; src[i] && j < dst_size - 2; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            if (j + 2 >= dst_size) break;
            dst[j++] = '\\'; dst[j++] = c;
        } else if (c == '\n') {
            if (j + 2 >= dst_size) break;
            dst[j++] = '\\'; dst[j++] = 'n';
        } else if (c == '\r') {
            if (j + 2 >= dst_size) break;
            dst[j++] = '\\'; dst[j++] = 'r';
        } else if (c == '\t') {
            if (j + 2 >= dst_size) break;
            dst[j++] = '\\'; dst[j++] = 't';
        } else if (c < 0x20) {
            if (j + 6 >= dst_size) break;
            j += snprintf(dst + j, dst_size - j, "\\u%04x", c);
        } else {
            dst[j++] = c;
        }
    }
    dst[j] = '\0';
    return j;
}

static int diag_append(char *buf, int pos, int size, int first,
                       const char *name, int ok, const char *msg,
                       int *ok_count, int *fail_count) {
    if (pos >= size - 1) return pos;
    pos += snprintf(buf + pos, size - pos,
        "%s{\"name\":\"%s\",\"ok\":%s,\"message\":\"%s\"}",
        first ? "" : ",", name, ok ? "true" : "false", msg);
    if (pos >= size) pos = size - 1;
    if (ok) (*ok_count)++; else (*fail_count)++;
    return pos;
}

/* ===== API处理函数: 通用 ===== */

static int handle_get_ping(int fd, const char *body, const char *query) {
    return send_json(fd, 200, "{\"ok\":true}");
}

static int handle_get_version(int fd, const char *body, const char *query) {
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"version\":\"%s\"}", XWEBD_VERSION);
    return send_json(fd, 200, buf);
}

static int handle_get_config(int fd, const char *body, const char *query) {
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"upload_max_mb\":%d,\"log_level\":\"%s\"}",
             g_upload_max_mb,
             g_plog_level == 3 ? "DEBUG" : g_plog_level == 2 ? "INFO" : g_plog_level == 1 ? "WARN" : "ERROR");
    return send_json(fd, 200, buf);
}

static int handle_put_config(int fd, const char *body, const char *query) {
    if (!body || !*body) return send_error(fd, 400, "Empty request body");

    int upload_mb = -1;
    char log_level[16] = {0};
    int has_changes = 0;

    if (parse_json_int(body, "upload_max_mb", &upload_mb) == 0) {
        if (upload_mb < 1 || upload_mb > 100) return send_error(fd, 400, "upload_max_mb must be 1-100");
        g_upload_max_mb = upload_mb;
        has_changes = 1;
    }
    if (parse_json_str(body, "log_level", log_level, sizeof(log_level)) == 0) {
        if (strcmp(log_level, "DEBUG") != 0 && strcmp(log_level, "INFO") != 0 &&
            strcmp(log_level, "WARN") != 0 && strcmp(log_level, "ERROR") != 0)
            return send_error(fd, 400, "Invalid log_level");
        if (strcmp(log_level, "DEBUG") == 0) g_plog_level = 3;
        else if (strcmp(log_level, "INFO") == 0) g_plog_level = 2;
        else if (strcmp(log_level, "WARN") == 0) g_plog_level = 1;
        else g_plog_level = 0;
        has_changes = 1;
    }

    if (!has_changes) return send_error(fd, 400, "No valid config fields provided");
    return send_json(fd, 200, "{\"ok\":true}");
}

/* ===== API处理函数: 系统 ===== */

static int handle_get_system(int fd, const char *body, const char *query) {
    char buf[XWEBD_RESP_BUF_SIZE];
    int len = 0;

    char model[64] = "Unknown", kernel[64] = "Unknown", cpu[128] = "";
    long mem_total = 0, mem_free = 0, mem_cached = 0;
    double uptime = 0;
    int battery = -1;
    int wifi_connected = 0;
    char wifi_ip[64] = "";
    long disk_total = 0, disk_used = 0, disk_free = 0;
    int assistant_installed = 0, assistant_running = 0;
    char assistant_version[32] = "";
    int cpu_usage = -1;
    int key_backlight = -1;

    read_proc_line("/proc/device-tree/model", model, sizeof(model));
    read_proc_line("/proc/version", kernel, sizeof(kernel));

    {
        FILE *f = fopen("/proc/cpuinfo", "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                if (strncmp(line, "model name", 10) == 0 || strncmp(line, "Processor", 9) == 0) {
                    char *p = strchr(line, ':');
                    if (p) { p++; while (*p == ' ' || *p == '\t') p++; char *nl = strchr(p, '\n'); if (nl) *nl = '\0'; strncpy(cpu, p, sizeof(cpu) - 1); }
                    break;
                }
            }
            fclose(f);
        }
    }

    {
        FILE *f = fopen("/proc/meminfo", "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                if (strncmp(line, "MemTotal:", 9) == 0) mem_total = atol(line + 9);
                else if (strncmp(line, "MemFree:", 8) == 0) mem_free = atol(line + 8);
                else if (strncmp(line, "Cached:", 7) == 0) mem_cached = atol(line + 7);
            }
            fclose(f);
        }
    }

    {
        char buf2[64];
        if (read_file_string("/proc/uptime", buf2, sizeof(buf2)) > 0) {
            char *sp = strchr(buf2, ' ');
            if (sp) *sp = '\0';
            uptime = atof(buf2);
        }
    }
    {
        char buf2[16];
        if (read_file_string("/sys/class/power_supply/battery/capacity", buf2, sizeof(buf2)) > 0)
            battery = atoi(buf2);
        else if (read_file_string("/sys/class/power_supply/bq27520/capacity", buf2, sizeof(buf2)) > 0)
            battery = atoi(buf2);
        else if (read_file_string("/sys/class/power_supply/max170xx_battery/capacity", buf2, sizeof(buf2)) > 0)
            battery = atoi(buf2);
        else {
            FILE *pf = popen("cat /sys/class/power_supply/*/capacity 2>/dev/null | head -1", "r");
            if (pf) {
                if (fgets(buf2, sizeof(buf2), pf)) {
                    int val = atoi(buf2);
                    if (val > 0) battery = val;
                }
                pclose(pf);
            }
        }
    }

    {
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock >= 0) {
            struct ifreq ifr;
            memset(&ifr, 0, sizeof(ifr));
            strncpy(ifr.ifr_name, "wlan0", IFNAMSIZ - 1);
            if (ioctl(sock, SIOCGIFFLAGS, &ifr) == 0 && (ifr.ifr_flags & IFF_UP) && (ifr.ifr_flags & IFF_RUNNING)) {
                wifi_connected = 1;
                if (ioctl(sock, SIOCGIFADDR, &ifr) == 0) {
                    struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
                    strncpy(wifi_ip, inet_ntoa(sin->sin_addr), sizeof(wifi_ip) - 1);
                }
            }
            close(sock);
        }
        if (!wifi_connected) {
            FILE *fp = popen("ifconfig wlan0 2>/dev/null | grep -qE 'inet addr:|inet [0-9]' && echo yes || echo no", "r");
            if (fp) {
                char tmp[8];
                if (fgets(tmp, sizeof(tmp), fp) && strncmp(tmp, "yes", 3) == 0) {
                    wifi_connected = 1;
                    if (!wifi_ip[0]) {
                        pclose(fp);
                        fp = popen("ifconfig wlan0 2>/dev/null | grep -oE 'inet addr:[0-9.]+|inet [0-9.]+' | grep -oE '[0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+'", "r");
                        if (fp) {
                            if (fgets(wifi_ip, sizeof(wifi_ip), fp)) {
                                char *nl = strchr(wifi_ip, '\n');
                                if (nl) *nl = '\0';
                            }
                            pclose(fp);
                        }
                        fp = NULL;
                    }
                }
                if (fp) pclose(fp);
            }
        }
    }

    {
        struct statvfs st;
        if (statvfs(XWEBD_BASE_DIR, &st) == 0) {
            disk_total = (long)((long long)st.f_blocks * st.f_bsize / 1024);
            disk_free = (long)((long long)st.f_bavail * st.f_bsize / 1024);
            disk_used = disk_total - disk_free;
        }
    }

    if (access(XWEBD_SAIR_BIN, X_OK) == 0) {
        assistant_installed = 1;
        if (find_pid_by_name("sair_main") > 0 || find_pid_by_name("sair") > 0)
            assistant_running = 1;
    }

    cpu_sample_update();
    cpu_usage = g_cached_cpu_usage;

    {
        char bl_buf[8] = {0};
        if (read_file_string("/sys/class/input/input2/bl_onoff", bl_buf, sizeof(bl_buf)) > 0) {
            key_backlight = (bl_buf[0] == '1') ? 1 : 0;
        }
    }

    char esc_model[128], esc_kernel[128], esc_cpu[256], esc_wifi_ip[64];
    json_escape(model, esc_model, sizeof(esc_model));
    json_escape(kernel, esc_kernel, sizeof(esc_kernel));
    json_escape(cpu, esc_cpu, sizeof(esc_cpu));
    json_escape(wifi_ip, esc_wifi_ip, sizeof(esc_wifi_ip));

    len = snprintf(buf, sizeof(buf),
        "{\"model\":\"%s\",\"kernel\":\"%s\",\"cpu\":\"%s\","
        "\"mem_total_kb\":%ld,\"mem_free_kb\":%ld,\"mem_cached_kb\":%ld,"
        "\"uptime_s\":%.0f,\"battery_cap\":%d,"
        "\"wifi_connected\":%s,\"wifi_ip\":\"%s\","
        "\"disk_total_kb\":%ld,\"disk_used_kb\":%ld,\"disk_free_kb\":%ld,"
        "\"assistant_installed\":%s,\"assistant_running\":%s,\"assistant_version\":\"%s\","
        "\"cpu_usage\":%d,\"key_backlight\":%d}",
        esc_model, esc_kernel, esc_cpu,
        mem_total, mem_free, mem_cached,
        uptime, battery,
        wifi_connected ? "true" : "false", esc_wifi_ip,
        disk_total, disk_used, disk_free,
        assistant_installed ? "true" : "false", assistant_running ? "true" : "false", assistant_version,
        cpu_usage, key_backlight);

    return send_response(fd, 200, "application/json", buf, len);
}

static int handle_get_logs(int fd, const char *body, const char *query) {
    int lines = 100;
    char level_filter = 0;
    int source = 0;

    if (query) {
        const char *p;
        p = strstr(query, "lines=");
        if (p) lines = atoi(p + 6);
        if (lines <= 0) lines = 100;
        if (lines > 500) lines = 500;
        p = strstr(query, "level=");
        if (p) level_filter = *(p + 6);
        p = strstr(query, "source=");
        if (p) source = atoi(p + 7);
    }

    const char *log_paths[] = {
        "/var/upgrade/xiaozhi.log",
        "/var/upgrade/xwebd.log"
    };
    const char *log_names[] = {"sair", "xwebd"};
    int log_count = (source >= 1 && source <= 2) ? 1 : 2;
    int start_idx = (source >= 1 && source <= 2) ? source - 1 : 0;

    char *buf = malloc(XWEBD_RESP_BUF_SIZE * 4);
    if (!buf) return send_error(fd, 500, "Out of memory");
    int buf_pos = 0;
    int buf_size = XWEBD_RESP_BUF_SIZE * 4;
    APPEND_PRINTF(buf, buf_pos, buf_size, "{\"logs\":[");

    int total_lines = 0;
    int written = 0;

    for (int li = 0; li < log_count && total_lines < lines; li++) {
        int idx = start_idx + li;
        FILE *f = fopen(log_paths[idx], "r");
        if (!f) continue;

        fseek(f, 0, SEEK_END);
        long file_size = ftell(f);

        int want_lines = lines - total_lines;
        long read_start = 0;
        if (file_size > 65536) read_start = file_size - 65536;
        fseek(f, read_start, SEEK_SET);

        char line[512];
        char **ring = malloc(sizeof(char*) * want_lines);
        int *ring_level = malloc(sizeof(int) * want_lines);
        int ring_count = 0;
        if (!ring || !ring_level) {
            if (ring) free(ring);
            if (ring_level) free(ring_level);
            fclose(f);
            continue;
        }

        while (fgets(line, sizeof(line), f)) {
            int line_len = strlen(line);
            while (line_len > 0 && (line[line_len-1] == '\n' || line[line_len-1] == '\r'))
                line[--line_len] = '\0';

            char line_level = 0;
            char *lb = strchr(line, '[');
            if (lb) {
                char *rb = strchr(lb + 1, ']');
                if (rb && rb > lb + 1) {
                    char *lb2 = strchr(rb + 1, '[');
                    if (lb2) {
                        char *rb2 = strchr(lb2 + 1, ']');
                        if (rb2 && rb2 == lb2 + 2) line_level = *(lb2 + 1);
                    }
                }
            }

            if (level_filter) {
                char effective_level = line_level ? line_level : 'I';
                if (effective_level != level_filter) continue;
            }

            if (ring_count < want_lines) {
                ring[ring_count] = strdup(line);
                ring_level[ring_count] = line_level;
                ring_count++;
            } else {
                free(ring[0]);
                memmove(ring, ring + 1, sizeof(char*) * (want_lines - 1));
                memmove(ring_level, ring_level + 1, sizeof(int) * (want_lines - 1));
                ring[want_lines - 1] = strdup(line);
                ring_level[want_lines - 1] = line_level;
            }
        }
        fclose(f);

        for (int i = 0; i < ring_count && total_lines < lines; i++) {
            if (written > 0) APPEND_PRINTF(buf, buf_pos, buf_size, ",");
            const char *lvl = "INFO";
            switch (ring_level[i]) {
                case 'E': lvl = "ERROR"; break;
                case 'W': lvl = "WARN"; break;
                case 'I': lvl = "INFO"; break;
                case 'D': lvl = "DEBUG"; break;
            }
            char escaped[1024];
            int ep = 0;
            for (const char *s = ring[i]; *s && ep < (int)sizeof(escaped) - 2; s++) {
                if (*s == '"' || *s == '\\') escaped[ep++] = '\\';
                escaped[ep++] = *s;
            }
            escaped[ep] = '\0';
            APPEND_PRINTF(buf, buf_pos, buf_size,
                "{\"source\":\"%s\",\"level\":\"%s\",\"text\":\"%s\"}",
                log_names[idx], lvl, escaped);
            written++;
            total_lines++;
            free(ring[i]);
        }
        free(ring);
        free(ring_level);
    }

    APPEND_PRINTF(buf, buf_pos, buf_size, "],\"count\":%d}", total_lines);
    int ret = send_json(fd, 200, buf);
    free(buf);
    return ret;
}

static int handle_get_services(int fd, const char *body, const char *query) {
    int telnet_running = (find_pid_by_name("telnetd") > 0);
    int watchdog_exists = (access(XWEBD_WATCHDOG_SH, X_OK) == 0);
    int watchdog_running = (find_pid_by_name("boot_watchdog") > 0 || find_pid_by_cmdline("boot_watchdog.sh") > 0);
    int xwebd_autostart = 0;
    {
        char buf2[1024];
        int n = read_file_string(XWEBD_TEST_SH, buf2, sizeof(buf2));
        if (n > 0 && strstr(buf2, "xwebd")) xwebd_autostart = 1;
    }
    int sair_installed = (access(XWEBD_SAIR_BIN, X_OK) == 0);
    int sair_running = (find_pid_by_name("sair_main") > 0 || find_pid_by_name("sair") > 0);
    int xwebd_running = (find_pid_by_name("xwebd") > 0);
    int usb_lun_enabled = 0;
    {
        char lun_buf[256] = "";
        int lf = open("/sys/class/android_usb/android0/f_mass_storage/lun0/file", O_RDONLY);
        if (lf >= 0) {
            int nr = read(lf, lun_buf, sizeof(lun_buf) - 1);
            close(lf);
            if (nr > 0) {
                lun_buf[nr] = '\0';
                while (nr > 0 && (lun_buf[nr-1] == '\n' || lun_buf[nr-1] == '\r')) lun_buf[--nr] = '\0';
                usb_lun_enabled = (lun_buf[0] != '\0');
            }
        }
    }
    int led_enabled = (g_persist_led < 0) ? 1 : g_persist_led;
    int precache_enabled = g_persist_precache;
    int key_backlight_enabled = -1;
    {
        char bl_buf[8] = {0};
        if (read_file_string("/sys/class/input/input2/bl_onoff", bl_buf, sizeof(bl_buf)) > 0) {
            key_backlight_enabled = (bl_buf[0] == '1') ? 1 : 0;
        }
    }

    char esc_ws_url[512] = "";
    json_escape(g_persist_custom_ws_url, esc_ws_url, sizeof(esc_ws_url));

    char buf[1024];
    int len = snprintf(buf, sizeof(buf),
        "{\"telnet\":{\"running\":%s},"
        "\"boot_watchdog\":{\"deployed\":%s,\"running\":%s},"
        "\"xwebd\":{\"running\":%s,\"autostart\":%s},"
        "\"sair\":{\"installed\":%s,\"running\":%s},"
        "\"usb_lun\":{\"enabled\":%s},"
        "\"led\":{\"enabled\":%s},"
        "\"audio_precache\":{\"enabled\":%s},"
        "\"custom_ws_url\":\"%s\","
        "\"key_backlight\":{\"enabled\":%s}}",
        telnet_running ? "true" : "false",
        watchdog_exists ? "true" : "false", watchdog_running ? "true" : "false",
        xwebd_running ? "true" : "false", xwebd_autostart ? "true" : "false",
        sair_installed ? "true" : "false", sair_running ? "true" : "false",
        usb_lun_enabled ? "true" : "false",
        led_enabled ? "true" : "false",
        precache_enabled ? "true" : "false",
        esc_ws_url,
        key_backlight_enabled == -1 ? "null" : (key_backlight_enabled ? "true" : "false"));
    return send_response(fd, 200, "application/json", buf, len);
}







static int is_usb_data_mode(void) {
    char enable[8] = "";
    char functions[64] = "";
    read_file_string("/sys/class/android_usb/android0/enable", enable, sizeof(enable));
    read_file_string("/sys/class/android_usb/android0/functions", functions, sizeof(functions));
    return (enable[0] == '1' && strstr(functions, "adb") != NULL);
}


static void save_persist_conf(void) {
    char buf[1024];
    int len = snprintf(buf, sizeof(buf), "usb_lun=%d\ntelnet=%d\nled=%d\nprecache=%d\ncustom_ws_url=%s\n",
                       g_persist_usb_lun >= 0 ? g_persist_usb_lun : 0,
                       g_persist_telnet >= 0 ? g_persist_telnet : 1,
                       g_persist_led >= 0 ? g_persist_led : 1,
                       g_persist_precache,
                       g_persist_custom_ws_url);
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", XWEBD_PERSIST_CONF);
    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        if (write(fd, buf, len) == len) {
            close(fd);
            if (rename(tmp_path, XWEBD_PERSIST_CONF) != 0) {
                XLOG_E(TAG, "重命名persist配置失败: %s", strerror(errno));
                unlink(tmp_path);
            } else {
                XLOG_I(TAG, "持久化配置已保存");
            }
        } else {
            close(fd);
            unlink(tmp_path);
            XLOG_E(TAG, "写入persist配置不完整");
        }
    }
}

static void load_persist_conf(void) {
    char buf[1024];
    int n = read_file_string(XWEBD_PERSIST_CONF, buf, sizeof(buf));
    if (n <= 0) return;
    char *p;
    p = strstr(buf, "usb_lun=");
    if (p) g_persist_usb_lun = atoi(p + 8);
    p = strstr(buf, "telnet=");
    if (p) g_persist_telnet = atoi(p + 7);
    p = strstr(buf, "led=");
    if (p) g_persist_led = atoi(p + 4);
    p = strstr(buf, "precache=");
    if (p) g_persist_precache = atoi(p + 9);
    /* transport_mode/bl_enable/bl_value 键已废弃(2026-08-30 移除MQTT路线 + 背光插件化),
     * 旧文件孤儿行忽略; 背光持久化由 xwplug-backlight 自有 conf 负责 */

    p = strstr(buf, "custom_ws_url=");
    if (p) {
        p += 14;
        int i = 0;
        while (*p && *p != '\n' && *p != '\r' && i < (int)sizeof(g_persist_custom_ws_url) - 1)
            g_persist_custom_ws_url[i++] = *p++;
        g_persist_custom_ws_url[i] = '\0';
    }
}

static void apply_persist_conf(void) {
    if (g_persist_telnet >= 0) {
        if (!g_persist_telnet) {
            system("killall telnetd 2>/dev/null");
            XLOG_I(TAG, "恢复Telnet: 关闭");
        } else if (find_pid_by_name("telnetd") <= 0) {
            /* 持久开启态开机自启(原版仅 test.sh 拉起, test.sh 缺失时开机无 telnet) */
            system("busybox telnetd -p 23 -l /bin/sh 2>/dev/null &");
            XLOG_I(TAG, "恢复Telnet: 开启(开机自启)");
        }
    }
    if (g_persist_led >= 0) {
        int retries = 3;
        while (retries > 0) {
            if (send_led_cmd(g_persist_led) == 0) break;
            retries--;
            if (retries > 0) {
                XLOG_I(TAG, "呼吸灯恢复: 等待mqtt_custom_server启动, 剩余重试%d次", retries);
                sleep(3);
            }
        }
        if (retries <= 0) {
            XLOG_W(TAG, "呼吸灯恢复: mqtt_custom_server未就绪, 将在主循环中延迟重试");
            g_led_pending_value = g_persist_led;
            g_led_retry_time = time(NULL) + 5;
        }
    }
}

static int send_sair_cmd(const char *cmd_json, int cmd_len);
static int signal_sair(const char *sig_name);

static int handle_post_service_toggle(int fd, const char *body, const char *query) {
    char service[32] = "";
    char action[16] = "";
    if (parse_json_str(body, "service", service, sizeof(service)) < 0)
        return send_error(fd, 400, "Missing 'service' field");
    if (parse_json_str(body, "action", action, sizeof(action)) < 0)
        return send_error(fd, 400, "Missing 'action' field");

    int enable = (strcmp(action, "enable") == 0);

    if (strcmp(service, "usb_lun") == 0) {
        g_persist_usb_lun = enable;
        save_persist_conf();
        if (is_usb_data_mode()) {
            if (enable) {
                system("echo /dev/mmcblk0p1 > /sys/class/android_usb/android0/f_mass_storage/lun0/file");
            } else {
                system("echo '' > /sys/class/android_usb/android0/f_mass_storage/lun0/file");
            }
            XLOG_I(TAG, "USB LUN: %s (数据传输模式, 已应用)", enable ? "enabled" : "disabled");
        } else {
            XLOG_I(TAG, "USB LUN: %s (非数据传输模式, 仅保存设置)", enable ? "enabled" : "disabled");
        }
    } else if (strcmp(service, "telnet") == 0) {
        if (enable) {
            system("busybox telnetd -p 23 -l /bin/sh 2>/dev/null");
        } else {
            system("killall telnetd 2>/dev/null");
        }
        g_persist_telnet = enable;
        save_persist_conf();
        XLOG_I(TAG, "Telnet: %s", enable ? "enabled" : "disabled");
    } else if (strcmp(service, "autostart") == 0) {
        char buf[2048];
        int n = read_file_string(XWEBD_TEST_SH, buf, sizeof(buf));
        if (n <= 0)
            return send_error(fd, 500, "Cannot read test.sh");
        char *p = strstr(buf, "xwebd");
        if (enable && !p) {
            char *ins_point = strstr(buf, "# 开机频率检测");
            if (ins_point) {
                char new_buf[3072];
                int prefix_len = (int)(ins_point - buf);
                snprintf(new_buf, sizeof(new_buf), "%.*sif [ -x /var/upgrade/xwebd ]; then cd /var/upgrade && ./xwebd -d; fi\n\n%s", prefix_len, buf, ins_point);
                int wf = open(XWEBD_TEST_SH, O_WRONLY | O_TRUNC);
                if (wf >= 0) { write(wf, new_buf, strlen(new_buf)); close(wf); }
            }
        } else if (!enable && p) {
            char *line_start = p;
            while (line_start > buf && *(line_start - 1) != '\n') line_start--;
            char *line_end = strchr(p, '\n');
            if (!line_end) line_end = buf + n;
            else line_end++;
            char new_buf[2048];
            int prefix_len = (int)(line_start - buf);
            int suffix_len = (int)(buf + n - line_end);
            snprintf(new_buf, sizeof(new_buf), "%.*s%.*s", prefix_len, buf, suffix_len, line_end);
            int wf = open(XWEBD_TEST_SH, O_WRONLY | O_TRUNC);
            if (wf >= 0) { write(wf, new_buf, strlen(new_buf)); close(wf); }
        }
        XLOG_I(TAG, "Autostart: %s", enable ? "enabled" : "disabled");
    } else if (strcmp(service, "boot_watchdog") == 0) {
        if (enable) {
            system(XWEBD_WATCHDOG_SH " &");
        } else {
            system("killall boot_watchdog.sh 2>/dev/null");
        }
        XLOG_I(TAG, "Boot watchdog: %s", enable ? "enabled" : "disabled");
    } else if (strcmp(service, "led") == 0) {
        if (send_led_cmd(enable) != 0) {
            XLOG_W(TAG, "呼吸灯: 发送控制消息失败");
        }
        g_persist_led = enable;
        g_led_pending_value = -1;
        save_persist_conf();
        XLOG_I(TAG, "呼吸灯: %s", enable ? "enabled" : "disabled");
    } else if (strcmp(service, "key_backlight") == 0) {
        int bl_fd = open("/sys/class/input/input2/bl_onoff", O_WRONLY);
        if (bl_fd >= 0) {
            write(bl_fd, enable ? "1" : "0", 1);
            close(bl_fd);
            XLOG_I(TAG, "按键背光: %s", enable ? "enabled" : "disabled");
        } else {
            XLOG_W(TAG, "按键背光: 打开sysfs失败");
            return send_error(fd, 500, "Cannot control key backlight");
        }
    } else if (strcmp(service, "audio_precache") == 0) {
        g_persist_precache = enable;
        save_persist_conf();
        XLOG_I(TAG, "音频预缓存: %s", enable ? "enabled" : "disabled");
    } else if (strcmp(service, "custom_ws_url") == 0) {
        char url[512] = {0};
        const char *url_key = strstr(body, "\"value\"");
        if (url_key) {
            url_key += 7;
            while (*url_key == ' ' || *url_key == ':' || *url_key == '\t') url_key++;
            if (*url_key == '"') {
                url_key++;
                int i = 0;
                while (*url_key && *url_key != '"' && i < (int)sizeof(url) - 1)
                    url[i++] = *url_key++;
                url[i] = '\0';
            }
        }
        strncpy(g_persist_custom_ws_url, url, sizeof(g_persist_custom_ws_url) - 1);
        save_persist_conf();
        XLOG_I(TAG, "自定义WS URL: %s", url);
        {
            char esc_url[1024] = "";
            if (url[0]) json_escape(url, esc_url, sizeof(esc_url));
            char cmd[1200];
            snprintf(cmd, sizeof(cmd), "{\"cmd\":\"set_config\",\"ws_url\":\"%s\"}", esc_url);
            send_sair_cmd(cmd, strlen(cmd));
            signal_sair("自定义WS URL变更");
        }
    } else {
        return send_error(fd, 400, "Unknown service");
    }

    return send_json(fd, 200, "{\"ok\":true}");
}

static int handle_get_diag(int fd, const char *body, const char *query) {
    char buf[2048];
    int pos = 0;
    int ok_count = 0, fail_count = 0;
    int first = 1;

    APPEND_PRINTF(buf, pos, (int)sizeof(buf), "{\"section\":\"xwebd\",\"items\":[");

    pos = diag_append(buf, pos, sizeof(buf), first,
        "/var/upgrade分区", access(XWEBD_BASE_DIR, W_OK) == 0,
        access(XWEBD_BASE_DIR, W_OK) == 0 ? "可读写" : "不可写",
        &ok_count, &fail_count);
    first = 0;

    {
        struct statvfs vfs;
        if (statvfs(XWEBD_BASE_DIR, &vfs) == 0) {
            long free_kb = (long)((long long)vfs.f_bavail * vfs.f_bsize / 1024);
            int ok = (free_kb >= 1024);
            char msg[64];
            if (ok) snprintf(msg, sizeof(msg), "%ldKB可用", free_kb);
            else snprintf(msg, sizeof(msg), "仅%ldKB可用", free_kb);
            pos = diag_append(buf, pos, sizeof(buf), first, "磁盘空间", ok, msg, &ok_count, &fail_count);
        } else {
            pos = diag_append(buf, pos, sizeof(buf), first, "磁盘空间", 0, "查询失败", &ok_count, &fail_count);
        }
    }

    pos = diag_append(buf, pos, sizeof(buf), first,
        "test.sh自启动", access(XWEBD_TEST_SH, X_OK) == 0,
        access(XWEBD_TEST_SH, X_OK) == 0 ? "自启动脚本已配置" : "自启动脚本未配置",
        &ok_count, &fail_count);

    {
        int wifi_ok = 0;
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock >= 0) {
            struct ifreq ifr;
            memset(&ifr, 0, sizeof(ifr));
            strncpy(ifr.ifr_name, "wlan0", IFNAMSIZ - 1);
            if (ioctl(sock, SIOCGIFFLAGS, &ifr) == 0 && (ifr.ifr_flags & IFF_UP))
                wifi_ok = 1;
            close(sock);
        }
        if (!wifi_ok) {
            FILE *fp = popen("ifconfig wlan0 2>/dev/null | grep -qE 'inet addr:|inet [0-9]' && echo yes || echo no", "r");
            if (fp) {
                char tmp[8];
                if (fgets(tmp, sizeof(tmp), fp) && strncmp(tmp, "yes", 3) == 0)
                    wifi_ok = 1;
                pclose(fp);
            }
        }
        pos = diag_append(buf, pos, sizeof(buf), first,
            "WiFi网络", wifi_ok,
            wifi_ok ? "WiFi已连接" : "WiFi未连接",
            &ok_count, &fail_count);
    }

    {
        int launcher_running = (find_pid_by_name("launcher") > 0);
        pos = diag_append(buf, pos, sizeof(buf), first,
            "Launcher进程", launcher_running,
            launcher_running ? "Launcher运行中" : "Launcher未运行",
            &ok_count, &fail_count);
    }

    APPEND_PRINTF(buf, pos, (int)sizeof(buf),
        "],\"ok_count\":%d,\"fail_count\":%d,\"total\":%d,"
        "\"summary\":\"%d项正常, %d项异常\"}",
        ok_count, fail_count, ok_count + fail_count, ok_count, fail_count);

    return send_response(fd, 200, "application/json", buf, pos);
}

static int handle_get_assistant_env(int fd, const char *body, const char *query) {
    char buf[4096];
    int pos = 0;
    int ok_count = 0, fail_count = 0;
    int first = 1;
    char msg[128];

    APPEND_PRINTF(buf, pos, (int)sizeof(buf), "{\"section\":\"assistant_env\",\"items\":[");

    {
        int mgr_running = (find_pid_by_name("manager") > 0);
        pos = diag_append(buf, pos, sizeof(buf), first,
            "Manager进程", mgr_running,
            mgr_running ? "Manager运行中" : "Manager未运行",
            &ok_count, &fail_count);
    }
    first = 0;

    {
        int audio_running = (find_pid_by_name("audio_service") > 0);
        pos = diag_append(buf, pos, sizeof(buf), first,
            "audio_service进程", audio_running,
            audio_running ? "音频服务运行中" : "音频服务未运行",
            &ok_count, &fail_count);
    }

    pos = diag_append(buf, pos, sizeof(buf), first,
        "触摸屏设备", access("/dev/input/event2", R_OK) == 0,
        access("/dev/input/event2", R_OK) == 0 ? "/dev/input/event2可访问" : "/dev/input/event2不可访问",
        &ok_count, &fail_count);

    pos = diag_append(buf, pos, sizeof(buf), first,
        "applib框架", access("/usr/lib/libapplib.so", R_OK) == 0,
        access("/usr/lib/libapplib.so", R_OK) == 0 ? "libapplib.so存在" : "libapplib.so不存在",
        &ok_count, &fail_count);

    pos = diag_append(buf, pos, sizeof(buf), first,
        "ASR引擎库", access("/usr/lib/libsair_asr.so", R_OK) == 0,
        access("/usr/lib/libsair_asr.so", R_OK) == 0 ? "libsair_asr.so存在" : "libsair_asr.so不存在",
        &ok_count, &fail_count);

    pos = diag_append(buf, pos, sizeof(buf), first,
        "音频服务库", access("/usr/lib/libaudio_service_api.so", R_OK) == 0,
        access("/usr/lib/libaudio_service_api.so", R_OK) == 0 ? "libaudio_service_api.so存在" : "libaudio_service_api.so不存在",
        &ok_count, &fail_count);

    pos = diag_append(buf, pos, sizeof(buf), first,
        "音频录制库", access("/usr/lib/libaudio_recorder.so", R_OK) == 0,
        access("/usr/lib/libaudio_recorder.so", R_OK) == 0 ? "libaudio_recorder.so存在" : "libaudio_recorder.so不存在",
        &ok_count, &fail_count);

    pos = diag_append(buf, pos, sizeof(buf), first,
        "DDS消息服务", access("/usr/lib/libdds.so", R_OK) == 0,
        access("/usr/lib/libdds.so", R_OK) == 0 ? "libdds.so存在" : "libdds.so不存在",
        &ok_count, &fail_count);

    pos = diag_append(buf, pos, sizeof(buf), first,
        "配置管理库", access("/usr/lib/libapconfig.so", R_OK) == 0,
        access("/usr/lib/libapconfig.so", R_OK) == 0 ? "libapconfig.so存在" : "libapconfig.so不存在",
        &ok_count, &fail_count);

    pos = diag_append(buf, pos, sizeof(buf), first,
        "配置分区库", access("/usr/lib/libconfigpart.so", R_OK) == 0,
        access("/usr/lib/libconfigpart.so", R_OK) == 0 ? "libconfigpart.so存在" : "libconfigpart.so不存在",
        &ok_count, &fail_count);

    pos = diag_append(buf, pos, sizeof(buf), first,
        "WiFi客户端库", access("/usr/lib/libwifi_client.so", R_OK) == 0,
        access("/usr/lib/libwifi_client.so", R_OK) == 0 ? "libwifi_client.so存在" : "libwifi_client.so不存在",
        &ok_count, &fail_count);

    pos = diag_append(buf, pos, sizeof(buf), first,
        "Duilite语音引擎", access("/usr/lib/libduilite_fespl.so", R_OK) == 0,
        access("/usr/lib/libduilite_fespl.so", R_OK) == 0 ? "libduilite_fespl.so存在" : "libduilite_fespl.so不存在",
        &ok_count, &fail_count);

    {
        int alooper_ok = (access("/usr/lib/libalooper.so", R_OK) == 0);
        int stream_ok = (access("/usr/lib/libstream_source.so", R_OK) == 0);
        snprintf(msg, sizeof(msg), "%s%s%s",
                 !alooper_ok ? "libalooper.so缺失" : "",
                 (!alooper_ok && !stream_ok) ? ", " : "",
                 !stream_ok ? "libstream_source.so缺失" : "");
        pos = diag_append(buf, pos, sizeof(buf), first,
            "ASR依赖库", (alooper_ok && stream_ok),
            (alooper_ok && stream_ok) ? "ASR依赖库均存在" : msg,
            &ok_count, &fail_count);
    }

    {
        int tls_ok = (access("/usr/lib/libmbedtls.so", R_OK) == 0);
        int crypto_ok = (access("/usr/lib/libmbedcrypto.so", R_OK) == 0);
        pos = diag_append(buf, pos, sizeof(buf), first,
            "TLS/SSL库", (tls_ok && crypto_ok),
            (tls_ok && crypto_ok) ? "mbedtls库均存在" : "mbedtls库缺失",
            &ok_count, &fail_count);
    }

    {
        int wakeup_found = (access("/usr/local/resource/duilite/wakeup.bin", R_OK) == 0 ||
                           access("/var/upgrade/wakeup.bin", R_OK) == 0 ||
                           access("/var/upgrade/wakeup_resource", F_OK) == 0 ||
                           access("/usr/share/sair_asr/resource", F_OK) == 0);
        pos = diag_append(buf, pos, sizeof(buf), first,
            "唤醒词模型", wakeup_found,
            wakeup_found ? "唤醒词模型资源存在" : "唤醒词模型资源未找到",
            &ok_count, &fail_count);
    }

    {
        int wifi_ok = 0;
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock >= 0) {
            struct ifreq ifr;
            memset(&ifr, 0, sizeof(ifr));
            strncpy(ifr.ifr_name, "wlan0", IFNAMSIZ - 1);
            if (ioctl(sock, SIOCGIFFLAGS, &ifr) == 0 && (ifr.ifr_flags & IFF_UP))
                wifi_ok = 1;
            close(sock);
        }
        if (!wifi_ok) {
            FILE *fp = popen("ifconfig wlan0 2>/dev/null | grep -qE 'inet addr:|inet [0-9]' && echo yes || echo no", "r");
            if (fp) {
                char tmp[8];
                if (fgets(tmp, sizeof(tmp), fp) && strncmp(tmp, "yes", 3) == 0)
                    wifi_ok = 1;
                pclose(fp);
            }
        }
        pos = diag_append(buf, pos, sizeof(buf), first,
            "WiFi联网", wifi_ok,
            wifi_ok ? "WiFi已连接" : "WiFi未连接",
            &ok_count, &fail_count);
    }

    APPEND_PRINTF(buf, pos, (int)sizeof(buf),
        "],\"ok_count\":%d,\"fail_count\":%d,\"total\":%d,"
        "\"summary\":\"%d项正常, %d项异常\"}",
        ok_count, fail_count, ok_count + fail_count, ok_count, fail_count);

    return send_response(fd, 200, "application/json", buf, pos);
}

static int handle_post_poweroff(int fd, const char *body, const char *query) {
    send_json(fd, 200, "{\"ok\":true}");
    fsync(fd);
    usleep(100000);
    sync();
    reboot(RB_POWER_OFF);
    return 0;
}

static int handle_post_reboot(int fd, const char *body, const char *query) {
    send_json(fd, 200, "{\"ok\":true}");
    fsync(fd);
    usleep(100000);
    sync();
    reboot(RB_AUTOBOOT);
    return 0;
}

/* ===== API处理函数: 文件 ===== */


static int handle_download_file(int fd, const char *body, const char *query) {
    char path_val[PATH_MAX] = "";
    if (query) {
        char *p = strstr(query, "path=");
        if (p) {
            p += 5;
            char *end = strchr(p, '&');
            int plen = end ? (int)(end - p) : (int)strlen(p);
            if (plen >= PATH_MAX) plen = PATH_MAX - 1;
            memcpy(path_val, p, plen);
            path_val[plen] = '\0';
        }
    }

    char resolved[PATH_MAX];
    if (validate_path(path_val, resolved, sizeof(resolved)) != 0)
        return send_error(fd, 400, "Invalid or unsafe path");

    struct stat st;
    if (stat(resolved, &st) != 0 || S_ISDIR(st.st_mode))
        return send_error(fd, 404, "File not found");

    int file_fd = open(resolved, O_RDONLY);
    if (file_fd < 0) return send_error(fd, 500, "Cannot open file");

    const char *fname = strrchr(resolved, '/');
    fname = fname ? fname + 1 : resolved;

    char safe_name[256];
    int si = 0;
    for (const char *p = fname; *p && si < (int)sizeof(safe_name) - 1; p++) {
        if (*p != '\r' && *p != '\n' && *p != '"') safe_name[si++] = *p;
    }
    safe_name[si] = '\0';

    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: %ld\r\n"
        "Content-Disposition: attachment; filename=\"%s\"\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n",
        (long)st.st_size, safe_name);
    send(fd, header, hlen, MSG_NOSIGNAL);

    char fbuf[XWEBD_FILE_BUF_SIZE];
    ssize_t total_sent = 0;
    while (total_sent < st.st_size) {
        ssize_t n = read(file_fd, fbuf, sizeof(fbuf));
        if (n <= 0) break;
        ssize_t off = 0;
        while (off < n) {
            ssize_t sent = send(fd, fbuf + off, n - off, MSG_NOSIGNAL);
            if (sent <= 0) goto download_done;
            off += sent;
        }
        total_sent += n;
    }
download_done:
    close(file_fd);
    return 0;
}




/* ===== API处理函数: 助手 ===== */

static int handle_get_assistant_status(int fd, const char *body, const char *query) {
    int installed = (access(XWEBD_SAIR_BIN, X_OK) == 0);
    int running = 0;
    int pid = 0;
    int native_running = 0;
    int activated = 0;
    int backup_exists = (access(XWEBD_SAIR_BACKUP, F_OK) == 0);
    char version[32] = "";
    char state[32] = "";
    char activation_code[64] = "";
    char ws_url[512] = "";
    char ws_token[512] = "";
    char log_level[16] = "";

    {
        pid = find_pid_by_name("sair_main");
        if (pid <= 0) pid = find_pid_by_name("sair");
        if (pid > 0) {
            char exe_path[256] = "";
            char link_path[64];
            snprintf(link_path, sizeof(link_path), "/proc/%d/exe", pid);
            ssize_t elen = readlink(link_path, exe_path, sizeof(exe_path) - 1);
            if (elen > 0) exe_path[elen] = '\0';
            if (strstr(exe_path, XWEBD_SAIR_BIN)) {
                running = 1;
            } else if (strstr(exe_path, "/usr/bin/sair")) {
                native_running = 1;
            } else if (exe_path[0]) {
                running = installed;
            }
        }
    }

    if (running) {
        char sbuf[512] = "";
        int sfd = open("/tmp/sair_status.json", O_RDONLY);
        if (sfd >= 0) {
            int n = read(sfd, sbuf, sizeof(sbuf) - 1);
            close(sfd);
            if (n > 0) {
                sbuf[n] = '\0';
                parse_json_str(sbuf, "state", state, sizeof(state));
                parse_json_str(sbuf, "version", version, sizeof(version));
                parse_json_str(sbuf, "activation_code", activation_code, sizeof(activation_code));
                {
                    int act_val = parse_json_bool(sbuf, "activated");
                    activated = (act_val == 1) ? 1 : 0;
                }
            }
        }

        char cbuf[1024] = "";
        int cfd = open("/tmp/sair_config.json", O_RDONLY);
        if (cfd >= 0) {
            int n = read(cfd, cbuf, sizeof(cbuf) - 1);
            close(cfd);
            if (n > 0) {
                cbuf[n] = '\0';
                parse_json_str(cbuf, "ws_url", ws_url, sizeof(ws_url));
                parse_json_str(cbuf, "ws_token", ws_token, sizeof(ws_token));
                parse_json_str(cbuf, "log_level", log_level, sizeof(log_level));
            }
        }
    }

    char esc_state[64], esc_version[64], esc_activation[128], esc_ws_url[1024], esc_ws_token[1024], esc_log_level[32];
    json_escape(state, esc_state, sizeof(esc_state));
    json_escape(version, esc_version, sizeof(esc_version));
    json_escape(activation_code, esc_activation, sizeof(esc_activation));
    json_escape(ws_url, esc_ws_url, sizeof(esc_ws_url));
    json_escape(ws_token, esc_ws_token, sizeof(esc_ws_token));
    json_escape(log_level, esc_log_level, sizeof(esc_log_level));

    char buf[4096];
    snprintf(buf, sizeof(buf),
        "{\"installed\":%s,\"running\":%s,\"native_running\":%s,\"pid\":%d,\"native_backup_exists\":%s,\"state\":\"%s\",\"version\":\"%s\",\"activation_code\":\"%s\",\"activated\":%s,\"ws_url\":\"%s\",\"ws_token\":\"%s\",\"log_level\":\"%s\"}",
        installed ? "true" : "false", running ? "true" : "false", native_running ? "true" : "false", pid,
        backup_exists ? "true" : "false", esc_state, esc_version, esc_activation, activated ? "true" : "false", esc_ws_url, esc_ws_token, esc_log_level);
    return send_json(fd, 200, buf);
}

static int handle_get_assistant_logs(int fd, const char *body, const char *query) {
    int lines = 50;
    if (query) {
        char *p = strstr(query, "lines=");
        if (p) lines = atoi(p + 6);
        if (lines <= 0) lines = 50;
        if (lines > 500) lines = 500;
    }

    char buf[XWEBD_RESP_BUF_SIZE];
    int fd_log = open(XWEBD_SAIR_LOG, O_RDONLY);
    if (fd_log < 0) return send_json(fd, 200, "{\"logs\":\"\"}");

    off_t file_size = lseek(fd_log, 0, SEEK_END);
    int line_count = 0;
    off_t pos = file_size;
    char rbuf[1024];

    while (pos > 0 && line_count < lines) {
        int chunk = (pos < (off_t)sizeof(rbuf)) ? (int)pos : (int)sizeof(rbuf);
        pos -= chunk;
        lseek(fd_log, pos, SEEK_SET);
        ssize_t n = read(fd_log, rbuf, chunk);
        if (n <= 0) break;
        for (ssize_t i = n - 1; i >= 0; i--) {
            if (rbuf[i] == '\n') { line_count++; if (line_count >= lines) { pos += i + 1; break; } }
        }
    }
    if (pos < 0) pos = 0;
    lseek(fd_log, pos, SEEK_SET);

    int total = 0;
    while (total < (int)sizeof(buf) - 1) {
        ssize_t n = read(fd_log, buf + total, (int)sizeof(buf) - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    close(fd_log);
    buf[total] = '\0';

    char resp[XWEBD_RESP_BUF_SIZE + 64];
    int rlen = snprintf(resp, sizeof(resp), "{\"logs\":");
    resp[rlen++] = '"';
    for (int i = 0; i < total && rlen < (int)sizeof(resp) - 4; i++) {
        if (buf[i] == '"') { resp[rlen++] = '\\'; resp[rlen++] = '"'; }
        else if (buf[i] == '\n') { resp[rlen++] = '\\'; resp[rlen++] = 'n'; }
        else if (buf[i] == '\r') { }
        else if (buf[i] == '\\') { resp[rlen++] = '\\'; resp[rlen++] = '\\'; }
        else { resp[rlen++] = buf[i]; }
    }
    resp[rlen++] = '"';
    resp[rlen++] = '}';
    resp[rlen] = '\0';

    return send_response(fd, 200, "application/json", resp, rlen);
}

static int handle_post_assistant_upgrade(int fd, const char *body, const char *query);

static int handle_post_assistant_deploy(int fd, const char *body, const char *query) {
    if (access(XWEBD_BASE_DIR, W_OK) != 0) {
        mkdir(XWEBD_BASE_DIR, 0755);
        if (access(XWEBD_BASE_DIR, W_OK) != 0)
            return send_error(fd, 500, XWEBD_BASE_DIR " not writable");
    }
    char sair_new_path[PATH_MAX] = XWEBD_BASE_DIR "/sair_new";
    if (body && *body) {
        char path_val[PATH_MAX] = "";
        if (parse_json_str(body, "path", path_val, sizeof(path_val)) == 0 && path_val[0]) {
            char resolved[PATH_MAX];
            if (validate_path(path_val, resolved, sizeof(resolved)) != 0)
                return send_error(fd, 400, "Invalid path");
            strncpy(sair_new_path, resolved, sizeof(sair_new_path) - 1);
        }
    }

    if (access(sair_new_path, R_OK) != 0)
        return send_error(fd, 404, "sair_new not found, upload first");

    if (access(XWEBD_SAIR_BIN, X_OK) == 0) {
        if (rename(XWEBD_SAIR_BIN, XWEBD_SAIR_BACKUP) != 0)
            XLOG_W(TAG, "备份助手程序失败: %s", strerror(errno));
    }
    if (rename(sair_new_path, XWEBD_SAIR_BIN) != 0)
    {
        XLOG_E(TAG, "重命名sair_new失败, 尝试恢复备份");
        if (access(XWEBD_SAIR_BACKUP, R_OK) == 0)
            rename(XWEBD_SAIR_BACKUP, XWEBD_SAIR_BIN);
        return send_error(fd, 500, "Failed to rename sair_new to sair");
    }
    chmod(XWEBD_SAIR_BIN, 0755);

    XLOG_I(TAG, "助手部署: 文件已放置到 %s, PATH优先级使Manager自动启动自定义sair, 重启设备", XWEBD_SAIR_BIN);
    send_json(fd, 200, "{\"ok\":true,\"method\":\"deploy\"}");
    fsync(fd);
    usleep(100000);
    sync();
    reboot(RB_AUTOBOOT);
    return 0;
}

static int handle_post_assistant_update(int fd, const char *body, const char *query) {
    return handle_post_assistant_upgrade(fd, "{\"method\":\"cold\"}", query);
}

static int handle_post_assistant_uninstall(int fd, const char *body, const char *query) {
    int sair_running = (find_pid_by_name("sair_main") > 0 || find_pid_by_name("sair") > 0);

    int sair_exists = (access(XWEBD_SAIR_BIN, F_OK) == 0);
    if (!sair_running && !sair_exists)
        return send_json(fd, 200, "{\"ok\":true,\"message\":\"not_installed\"}");

    send_json(fd, 200, "{\"ok\":true}");

    if (sair_running) {
        XLOG_I(TAG, "助手卸载: 正在停止sair...");
        system("killall sair 2>/dev/null");
        usleep(300000);
        system("killall -9 sair 2>/dev/null");
        usleep(300000);
    }

    if (sair_exists) {
        if (unlink(XWEBD_SAIR_BIN) == 0)
            XLOG_I(TAG, "助手卸载: 已删除 %s", XWEBD_SAIR_BIN);
    }

    if (access(XWEBD_SAIR_BACKUP, F_OK) == 0) {
        unlink(XWEBD_SAIR_BACKUP);
        XLOG_I(TAG, "助手卸载: 已删除 %s", XWEBD_SAIR_BACKUP);
    }

    {
        const char *cache_files[] = {
            "/var/upgrade/.mcp_endpoint",
            "/var/upgrade/.ws_config",
            "/var/upgrade/.client_id",
            "/var/upgrade/.xiaozhi_sair",
            NULL
        };
        for (int i = 0; cache_files[i]; i++) {
            if (access(cache_files[i], F_OK) == 0) {
                unlink(cache_files[i]);
                XLOG_I(TAG, "助手卸载: 已删除 %s", cache_files[i]);
            }
        }
    }

    save_persist_conf();
    XLOG_I(TAG, "助手卸载: 持久化配置已重置");

    return 0;
}

static int handle_post_assistant_logs_clear(int fd, const char *body, const char *query) {
    if (access(XWEBD_SAIR_LOG, F_OK) != 0)
        return send_json(fd, 200, "{\"ok\":true,\"message\":\"log file not found\"}");
    if (truncate(XWEBD_SAIR_LOG, 0) == 0)
        return send_json(fd, 200, "{\"ok\":true}");
    return send_error(fd, 500, "Failed to clear log file");
}

static int handle_post_logs_clean(int fd, const char *body, const char *query) {
    int source = 0;
    if (query) {
        const char *p = strstr(query, "source=");
        if (p) source = atoi(p + 7);
    }

    const char *paths[] = {
        NULL,
        XWEBD_SAIR_LOG,
        XWEBD_LOG_PATH
    };
    const char *names[] = {"", "sair", "xwebd"};

    if (source < 1 || source > 2)
        return send_error(fd, 400, "Invalid source (1=sair, 2=xwebd)");

    if (truncate(paths[source], 0) == 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"ok\":true,\"source\":\"%s\"}", names[source]);
        return send_json(fd, 200, buf);
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"source\":\"%s\",\"error\":\"truncate failed\"}", names[source]);
    return send_json(fd, 200, buf);
}

static int handle_post_assistant_upgrade(int fd, const char *body, const char *query) {
    if (access(XWEBD_SAIR_BIN, X_OK) != 0)
        return send_error(fd, 404, "sair not installed");

    char sair_new_path[PATH_MAX] = XWEBD_BASE_DIR "/sair_new";
    if (access(sair_new_path, R_OK) != 0)
        return send_error(fd, 404, "sair_new not found, upload first");

    int want_hot = 1;
    {
        char method[16] = "";
        if (body && parse_json_str(body, "method", method, sizeof(method)) == 0 && strcmp(method, "cold") == 0)
            want_hot = 0;
    }

    int sair_custom = 0;
    pid_t sair_pid = find_pid_by_name("sair_main");
    if (sair_pid <= 0) sair_pid = find_pid_by_name("sair");
    int sair_running = (sair_pid > 0);

    if (sair_running) {
        char exe_path[256] = "";
        char link[64];
        snprintf(link, sizeof(link), "/proc/%d/exe", sair_pid);
        int n = readlink(link, exe_path, sizeof(exe_path) - 1);
        if (n > 0) exe_path[n] = '\0';
        sair_custom = (strstr(exe_path, XWEBD_SAIR_BIN) != NULL);
    }

    if (rename(XWEBD_SAIR_BIN, XWEBD_SAIR_BACKUP) != 0)
        XLOG_W(TAG, "备份助手程序失败: %s", strerror(errno));
    if (rename(sair_new_path, XWEBD_SAIR_BIN) != 0)
    {
        XLOG_E(TAG, "重命名sair_new失败, 尝试恢复备份");
        if (access(XWEBD_SAIR_BACKUP, R_OK) == 0)
            rename(XWEBD_SAIR_BACKUP, XWEBD_SAIR_BIN);
        return send_error(fd, 500, "重命名sair_new失败");
    }
    chmod(XWEBD_SAIR_BIN, 0755);

    if (want_hot && sair_running && sair_custom && sair_pid > 0) {
        {
            char killcmd[128];
            snprintf(killcmd, sizeof(killcmd),
                "for p in $(pidof sair); do [ \"$p\" != \"%d\" ] && kill -9 $p 2>/dev/null; done",
                sair_pid);
            system(killcmd);
        }
        send_sair_cmd("{\"cmd\":\"upgrade\"}", 18);
        XLOG_I(TAG, "助手热更新: 已替换二进制, 通过cmd.json+SIGUSR1通知pid %d", sair_pid);
        kill(sair_pid, SIGUSR1);
        return send_json(fd, 200, "{\"ok\":true,\"method\":\"hot_update\"}");
    }

    XLOG_I(TAG, "助手冷更新: 已替换二进制, 重启设备");
    sync();
    reboot(RB_AUTOBOOT);
    return send_json(fd, 200, "{\"ok\":true,\"method\":\"cold_update\"}");
}

static int send_sair_cmd(const char *cmd_json, int cmd_len) {
    char tmp_path[] = "/tmp/sair_cmd.json.tmp";
    int tfd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (tfd < 0) return -1;
    if (write(tfd, cmd_json, cmd_len) != cmd_len) { close(tfd); unlink(tmp_path); return -1; }
    close(tfd);
    if (rename(tmp_path, "/tmp/sair_cmd.json") != 0) { unlink(tmp_path); return -1; }
    return 0;
}

static int signal_sair(const char *sig_name) {
    pid_t sair_pid = find_pid_by_name("sair_main");
    if (sair_pid <= 0) sair_pid = find_pid_by_name("sair");
    if (sair_pid <= 0) return -2;
    kill(sair_pid, SIGUSR1);
    XLOG_I(TAG, "%s: SIGUSR1 -> pid %d", sig_name, sair_pid);
    return 0;
}

static int handle_post_assistant_wakeup(int fd, const char *body, const char *query) {
    char cmd[] = "{\"cmd\":\"wakeup\"}";
    if (send_sair_cmd(cmd, sizeof(cmd) - 1) != 0)
        return send_error(fd, 502, "Failed to write wakeup command");
    int r = signal_sair("唤醒助手");
    if (r == -2) return send_error(fd, 404, "sair not running");
    if (r != 0) return send_error(fd, 500, "Cannot signal sair");
    return send_json(fd, 200, "{\"ok\":true}");
}

static int handle_post_assistant_abort(int fd, const char *body, const char *query) {
    char cmd[] = "{\"cmd\":\"abort\"}";
    if (send_sair_cmd(cmd, sizeof(cmd) - 1) != 0)
        return send_error(fd, 502, "Failed to write abort command");
    int r = signal_sair("中止对话");
    if (r == -2) return send_error(fd, 404, "sair not running");
    if (r != 0) return send_error(fd, 500, "Cannot signal sair");
    return send_json(fd, 200, "{\"ok\":true}");
}

static int handle_post_assistant_activate(int fd, const char *body, const char *query) {
    char cmd[] = "{\"cmd\":\"activate\"}";
    if (send_sair_cmd(cmd, sizeof(cmd) - 1) != 0)
        return send_error(fd, 502, "Failed to write activate command");
    int r = signal_sair("激活助手");
    if (r == -2) return send_error(fd, 404, "sair not running");
    if (r != 0) return send_error(fd, 500, "Cannot signal sair");
    return send_json(fd, 200, "{\"code\":0,\"msg\":\"ok\"}");
}

static int handle_get_assistant_config(int fd, const char *body, const char *query) {
    if (access(XWEBD_SAIR_BIN, F_OK) != 0)
        return send_json(fd, 200, "{\"ws_url\":\"\",\"ws_token\":\"\",\"log_level\":\"\",\"mcp_endpoint\":\"\",\"listening_mode\":\"\"}");

    /* sair_config.json 含 use_limit 全块(~700B)且随字段增加膨胀, 8192 防截断 */
    char buf[8192] = "";
    int cfd = open("/tmp/sair_config.json", O_RDONLY);
    if (cfd >= 0) {
        int n = read(cfd, buf, sizeof(buf) - 1);
        close(cfd);
        if (n > 0) {
            buf[n] = '\0';
            return send_json(fd, 200, buf);
        }
    }
    return send_json(fd, 200, "{\"ws_url\":\"\",\"ws_token\":\"\",\"log_level\":\"\",\"mcp_endpoint\":\"\",\"listening_mode\":\"\"}");
}

static int handle_put_assistant_config(int fd, const char *body, const char *query) {
    if (!body || !*body) return send_error(fd, 400, "Empty request body");

    int body_len = strlen(body);
    if (body_len < 2 || body[0] != '{' || body[body_len - 1] != '}')
        return send_error(fd, 400, "Invalid JSON body");

    char cmd_json[4096];
    int inner_len = body_len - 2;
    int cmd_len = snprintf(cmd_json, sizeof(cmd_json),
        "{\"cmd\":\"set_config\",%.*s}", inner_len, body + 1);
    if (cmd_len >= (int)sizeof(cmd_json))
        return send_error(fd, 400, "Config too large");

    {
        char ws_url[512] = {0};
        if (parse_json_str(body, "custom_ws_url", ws_url, sizeof(ws_url)) == 0) {
            strncpy(g_persist_custom_ws_url, ws_url, sizeof(g_persist_custom_ws_url) - 1);
            save_persist_conf();
            XLOG_I(TAG, "自定义WS URL(随助手配置): %s", ws_url);
        }
    }

    pid_t sair_pid = find_pid_by_name("sair_main");
    if (sair_pid <= 0) sair_pid = find_pid_by_name("sair");

    if (sair_pid <= 0) {
        XLOG_W(TAG, "配置变更: sair未运行，仅保存xwebd持久化配置");
        return send_error(fd, 404, "sair not running");
    }

    if (send_sair_cmd(cmd_json, cmd_len) != 0)
        return send_error(fd, 502, "Failed to write config command");

    kill(sair_pid, SIGUSR1);
    XLOG_I(TAG, "配置变更: SIGUSR1 -> pid %d", sair_pid);

    return send_json(fd, 200, "{\"ok\":true}");
}

static int handle_get_assistant_diag(int fd, const char *body, const char *query) {
    if (access(XWEBD_SAIR_BIN, F_OK) != 0)
        return send_json(fd, 200, "{\"ok_count\":0,\"fail_count\":1,\"total\":1,\"summary\":\"助手未安装\",\"items\":[{\"name\":\"助手状态\",\"ok\":false,\"message\":\"助手未安装\"}]}");

    pid_t sair_pid = find_pid_by_name("sair_main");
    if (sair_pid <= 0) sair_pid = find_pid_by_name("sair");
    if (sair_pid <= 0) return send_json(fd, 200, "{\"ok_count\":0,\"fail_count\":1,\"total\":1,\"summary\":\"助手未运行\",\"items\":[{\"name\":\"助手状态\",\"ok\":false,\"message\":\"助手未运行\"}]}");

    unlink("/tmp/sair_diag.json");

    int req_fd = open("/tmp/sair_diag_request", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (req_fd >= 0) close(req_fd);

    if (sair_pid > 0) kill(sair_pid, SIGUSR1);

    for (int retry = 0; retry < 10; retry++) {
        usleep(100000);
        int dfd = open("/tmp/sair_diag.json", O_RDONLY);
        if (dfd >= 0) {
            char buf[16384] = "";
            int n = read(dfd, buf, sizeof(buf) - 1);
            close(dfd);
            if (n > 0) {
                unlink("/tmp/sair_diag.json");
                buf[n] = '\0';
                return send_json(fd, 200, buf);
            }
        }
    }
    return send_json(fd, 200, "{\"ok_count\":0,\"fail_count\":1,\"total\":1,\"summary\":\"诊断信息不可用\",\"items\":[{\"name\":\"诊断\",\"ok\":false,\"message\":\"无法读取诊断信息\"}]}");
}

/* ===== 路由表 ===== */

static int handle_post_self_update(int fd, const char *body, const char *query) {
    if (access(XWEBD_BASE_DIR "/xwebd_new", F_OK) != 0)
        return send_error(fd, 404, "xwebd_new not found");

    char self_path[512] = {0};
    int n = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
    if (n <= 0 || n >= (int)sizeof(self_path) - 1) return send_error(fd, 500, "Cannot determine self path");
    self_path[n] = '\0';

    char old_path[PATH_MAX];
    snprintf(old_path, sizeof(old_path), "%s_old", self_path);
    unlink(old_path);
    if (rename(self_path, old_path) != 0)
        return send_error(fd, 500, "Cannot rename current binary");
    if (rename(XWEBD_BASE_DIR "/xwebd_new", self_path) != 0) {
        if (rename(old_path, self_path) != 0)
            XLOG_E(TAG, "自更新回滚失败! 手动恢复: mv %s %s", old_path, self_path);
        return send_error(fd, 500, "Cannot rename new binary");
    }
    chmod(self_path, 0755);

    XLOG_I(TAG, "自更新冷更新: 二进制已替换, 重启设备");
    send_json(fd, 200, "{\"ok\":true,\"method\":\"cold_update\"}");
    fsync(fd);
    usleep(100000);
    sync();
    reboot(RB_AUTOBOOT);
    return 0;
}

static const route_t g_routes[] = {
    {"GET",    "/api/ping",                handle_get_ping},
    {"GET",    "/api/version",             handle_get_version},
    {"GET",    "/api/config",              handle_get_config},
    {"PUT",    "/api/config",              handle_put_config},
    {"GET",    "/api/system",              handle_get_system},
    {"GET",    "/api/logs",                handle_get_logs},
    {"POST",   "/api/logs/clean",          handle_post_logs_clean},
    {"POST",   "/api/poweroff",            handle_post_poweroff},
    {"POST",   "/api/reboot",              handle_post_reboot},
    {"GET",    "/api/files/download",       handle_download_file},
    {"GET",    "/api/services",             handle_get_services},
    {"POST",   "/api/services/toggle",      handle_post_service_toggle},
    {"GET",    "/api/diag",                 handle_get_diag},
    {"GET",    "/api/assistant/env",        handle_get_assistant_env},
    {"GET",    "/api/assistant/status",      handle_get_assistant_status},
    {"GET",    "/api/assistant/config",      handle_get_assistant_config},
    {"PUT",    "/api/assistant/config",      handle_put_assistant_config},
    {"GET",    "/api/assistant/diag",        handle_get_assistant_diag},
    {"GET",    "/api/assistant/logs",        handle_get_assistant_logs},
    {"POST",   "/api/assistant/deploy",      handle_post_assistant_deploy},
    {"POST",   "/api/assistant/update",      handle_post_assistant_update},
    {"POST",   "/api/assistant/upgrade",     handle_post_assistant_upgrade},
    {"POST",   "/api/assistant/wakeup",      handle_post_assistant_wakeup},
    {"POST",   "/api/assistant/abort",       handle_post_assistant_abort},
    {"POST",   "/api/assistant/activate",     handle_post_assistant_activate},
    {"POST",   "/api/assistant/uninstall",   handle_post_assistant_uninstall},
    {"POST",   "/api/assistant/logs/clear",  handle_post_assistant_logs_clear},
    {"POST",   "/api/self-update",           handle_post_self_update},
    /* 插件管理(B0 框架, 见 plugin_gateway.c) */
    {"GET",    "/api/plugins",              xwplug_handle_list},
    {"POST",   "/api/plugins/restart",      xwplug_handle_restart},
    {"POST",   "/api/plugins/remove",       xwplug_handle_remove},
    {"POST",   "/api/plugins/install",      xwplug_handle_install},
    {NULL, NULL, NULL}
};

/* ===== 上传处理 ===== */

static void do_upload_child(int client_fd, const char *boundary, int content_length,
                           const char *body_read, int body_read_len) {
    xlog_close();

    char fbuf[XWEBD_FILE_BUF_SIZE];

    char tmp_path[PATH_MAX] = XWEBD_BASE_DIR "/.upload_XXXXXX";
    int tmp_fd = mkstemp(tmp_path);
    if (tmp_fd < 0) {
        send_error(client_fd, 500, "Cannot create temp file");
        _exit(1);
    }

    int boundary_len = strlen(boundary);
    char end_marker[140];
    int end_marker_len = snprintf(end_marker, sizeof(end_marker), "\r\n--%s--", boundary);
    (void)boundary_len;

    char filename[256] = "";
    int total_read = 0;
    int in_header = 1;
    int found_file = 0;
    int from_socket = 0;

    while (total_read < content_length) {
        int n;
        if (!from_socket && body_read_len > 0 && total_read < body_read_len) {
            int avail = body_read_len - total_read;
            int to_copy = avail < (int)sizeof(fbuf) ? avail : (int)sizeof(fbuf);
            memcpy(fbuf, body_read + total_read, to_copy);
            n = to_copy;
        } else {
            from_socket = 1;
            int to_read = sizeof(fbuf);
            if (content_length - total_read < to_read) to_read = content_length - total_read;
            n = recv(client_fd, fbuf, to_read, 0);
            if (n <= 0) break;
        }
        total_read += n;

        if (in_header) {
            char *hdr_end = strstr(fbuf, "\r\n\r\n");
            if (hdr_end) {
                char *fn_pos = strstr(fbuf, "filename=\"");
                if (fn_pos) {
                    fn_pos += 10;
                    char *fn_end = strchr(fn_pos, '"');
                    if (fn_end) {
                        int fn_len = (int)(fn_end - fn_pos);
                        if (fn_len >= (int)sizeof(filename)) fn_len = sizeof(filename) - 1;
                        memcpy(filename, fn_pos, fn_len);
                        filename[fn_len] = '\0';
                    }
                }
                in_header = 0;
                found_file = 1;
                int hdr_size = (int)(hdr_end - fbuf) + 4;
                int body_data = n - hdr_size;
                if (body_data > 0) {
                    ssize_t w = write(tmp_fd, fbuf + hdr_size, body_data);
                    if (w != body_data) { close(tmp_fd); remove(tmp_path); send_error(client_fd, 500, "Write failed"); _exit(1); }
                }
            }
        } else if (found_file) {
            ssize_t w = write(tmp_fd, fbuf, n);
            if (w != n) { close(tmp_fd); remove(tmp_path); send_error(client_fd, 500, "Write failed"); _exit(1); }
        }
    }

    if (found_file) {
        off_t file_size = lseek(tmp_fd, 0, SEEK_END);
        if (file_size > end_marker_len) {
            int search_len = end_marker_len + 4;
            if (search_len > (int)sizeof(fbuf)) search_len = (int)sizeof(fbuf);
            lseek(tmp_fd, file_size - search_len, SEEK_SET);
            int rn = read(tmp_fd, fbuf, search_len);
            char *ep = NULL;
            int i;
            for (i = 0; i <= rn - end_marker_len; i++) {
                if (memcmp(fbuf + i, end_marker, end_marker_len) == 0) {
                    ep = fbuf + i;
                    break;
                }
            }
            if (ep) {
                off_t cut_pos = file_size - search_len + (ep - fbuf);
                ftruncate(tmp_fd, cut_pos);
            }
        }
    }

    close(tmp_fd);

    if (!found_file || filename[0] == '\0') {
        remove(tmp_path);
        send_error(client_fd, 400, "No file found in upload");
        _exit(1);
    }

    char final_path[PATH_MAX];
    snprintf(final_path, sizeof(final_path), "%s/%s", XWEBD_BASE_DIR, filename);

    char resolved[PATH_MAX];
    if (validate_path(final_path, resolved, sizeof(resolved)) != 0) {
        remove(tmp_path);
        send_error(client_fd, 400, "Invalid destination path");
        _exit(1);
    }

    if (rename(tmp_path, resolved) != 0) {
        remove(tmp_path);
        send_error(client_fd, 500, "Failed to save file");
        _exit(1);
    }

    struct stat st;
    stat(resolved, &st);
    char resp[256];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"path\":\"%s\",\"size\":%ld}", resolved, (long)st.st_size);
    send_json(client_fd, 200, resp);
    _exit(0);
}

static void do_upload_raw_child(int client_fd, const char *filename, int content_length,
                                const char *body_read, int body_read_len) {
    xlog_close();

    char tmp_path[PATH_MAX] = XWEBD_BASE_DIR "/.upload_XXXXXX";
    int tmp_fd = mkstemp(tmp_path);
    if (tmp_fd < 0) {
        send_error(client_fd, 500, "Cannot create temp file");
        _exit(1);
    }

    char fbuf[XWEBD_FILE_BUF_SIZE];
    int total_read = 0;
    int from_socket = 0;

    while (total_read < content_length) {
        int n;
        if (!from_socket && body_read_len > 0 && total_read < body_read_len) {
            int avail = body_read_len - total_read;
            int to_copy = avail < (int)sizeof(fbuf) ? avail : (int)sizeof(fbuf);
            memcpy(fbuf, body_read + total_read, to_copy);
            n = to_copy;
        } else {
            from_socket = 1;
            int to_read = sizeof(fbuf);
            if (content_length - total_read < to_read) to_read = content_length - total_read;
            n = recv(client_fd, fbuf, to_read, 0);
            if (n <= 0) break;
        }
        if (n > 0) {
            ssize_t w = write(tmp_fd, fbuf, n);
            if (w != n) { close(tmp_fd); remove(tmp_path); send_error(client_fd, 500, "Write failed"); _exit(1); }
        }
        total_read += n;
    }

    close(tmp_fd);

    char final_path[PATH_MAX];
    snprintf(final_path, sizeof(final_path), "%s/%s", XWEBD_BASE_DIR, filename);

    char resolved[PATH_MAX];
    if (validate_path(final_path, resolved, sizeof(resolved)) != 0) {
        remove(tmp_path);
        send_error(client_fd, 400, "Invalid destination path");
        _exit(1);
    }

    if (rename(tmp_path, resolved) != 0) {
        remove(tmp_path);
        send_error(client_fd, 500, "Failed to save file");
        _exit(1);
    }

    struct stat st;
    stat(resolved, &st);
    char resp[256];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"path\":\"%s\",\"size\":%ld}", resolved, (long)st.st_size);
    send_json(client_fd, 200, resp);
    _exit(0);
}

static int handle_upload_raw(int client_fd, const char *content_type, int content_length,
                              const char *body_read, int body_read_len, const char *req_buf) {
    if (content_length <= 0) return send_error(client_fd, 400, "Missing Content-Length");

    int max_bytes = g_upload_max_mb * 1024 * 1024;
    if (content_length > max_bytes) {
        char err[64];
        snprintf(err, sizeof(err), "File too large (max %dMB)", g_upload_max_mb);
        return send_error(client_fd, 400, err);
    }

    pid_t existing = g_upload_pid;
    if (existing > 0) {
        char check[64];
        snprintf(check, sizeof(check), "/proc/%d/comm", existing);
        if (access(check, F_OK) == 0) return send_error(client_fd, 503, "Upload in progress");
    }

    char filename[256] = "";
    char *xfn = strcasestr(req_buf, "X-Filename:");
    if (xfn) {
        xfn += 11;
        while (*xfn == ' ') xfn++;
        int i = 0;
        while (*xfn && *xfn != '\r' && *xfn != '\n' && i < (int)sizeof(filename) - 1)
            filename[i++] = *xfn++;
        filename[i] = '\0';
    }
    if (filename[0] == '\0') return send_error(client_fd, 400, "Missing X-Filename header");

    pid_t pid_file = open(XWEBD_UPLOAD_PID_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (pid_file >= 0) { char pbuf[16]; snprintf(pbuf, sizeof(pbuf), "%d", getpid()); write(pid_file, pbuf, strlen(pbuf)); close(pid_file); }

    pid_t pid = fork();
    if (pid < 0) return send_error(client_fd, 500, "Fork failed");

    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        for (int fd_i = 3; fd_i < 1024; fd_i++) {
            if (fd_i != client_fd) close(fd_i);
        }
        do_upload_raw_child(client_fd, filename, content_length, body_read, body_read_len);
    }

    g_upload_pid = pid;
    XLOG_I(TAG, "原始上传开始: pid=%d, filename=%s, content_length=%d", pid, filename, content_length);
    return 1;
}

static int handle_upload(int client_fd, const char *content_type, int content_length,
                        const char *body_read, int body_read_len) {
    if (content_length <= 0) return send_error(client_fd, 400, "Missing Content-Length");

    int max_bytes = g_upload_max_mb * 1024 * 1024;
    if (content_length > max_bytes) {
        char err[64];
        snprintf(err, sizeof(err), "File too large (max %dMB)", g_upload_max_mb);
        return send_error(client_fd, 400, err);
    }

    pid_t existing = g_upload_pid;
    if (existing > 0) {
        char check[64];
        snprintf(check, sizeof(check), "/proc/%d/comm", existing);
        if (access(check, F_OK) == 0) return send_error(client_fd, 503, "Upload in progress");
    }

    const char *boundary = NULL;
    if (content_type) {
        const char *b = strstr(content_type, "boundary=");
        if (b) { b += 9; boundary = b; }
    }
    if (!boundary) return send_error(client_fd, 400, "Missing boundary in Content-Type");

    char boundary_copy[128];
    strncpy(boundary_copy, boundary, sizeof(boundary_copy) - 1);
    boundary_copy[sizeof(boundary_copy) - 1] = '\0';
    char *semi = strchr(boundary_copy, ';');
    if (semi) *semi = '\0';

    pid_t pid_file = open(XWEBD_UPLOAD_PID_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (pid_file >= 0) { char pbuf[16]; snprintf(pbuf, sizeof(pbuf), "%d", getpid()); write(pid_file, pbuf, strlen(pbuf)); close(pid_file); }

    pid_t pid = fork();
    if (pid < 0) return send_error(client_fd, 500, "Fork failed");

    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        for (int fd_i = 3; fd_i < 1024; fd_i++) {
            if (fd_i != client_fd) close(fd_i);
        }
        do_upload_child(client_fd, boundary_copy, content_length, body_read, body_read_len);
    }

    g_upload_pid = pid;
    XLOG_I(TAG, "上传开始: pid=%d, content_length=%d", pid, content_length);
    return 1;
}

/* ===== HTTP请求处理 ===== */

static int handle_request(int client_fd) {
    char req_buf[XWEBD_REQ_BUF_SIZE];
    int total = 0;
    time_t start_time = time(NULL);

    while (total < XWEBD_REQ_BUF_SIZE - 1) {
        if (time(NULL) - start_time >= XWEBD_REQUEST_TIMEOUT) return -1;
        struct pollfd pfd = {client_fd, POLLIN, 0};
        int pret = poll(&pfd, 1, 1000);
        if (pret < 0) return -1;
        if (pret == 0) continue;
        int n = recv(client_fd, req_buf + total, XWEBD_REQ_BUF_SIZE - 1 - total, 0);
        if (n <= 0) return -1;
        total += n;
        req_buf[total] = '\0';
        if (strstr(req_buf, "\r\n\r\n")) break;
    }

    if (total == 0) return -1;

    char method[16] = {0};
    char path[512] = {0};
    sscanf(req_buf, "%15s %511s", method, path);

    char *query = strchr(path, '?');
    if (query) *query++ = '\0';

    char *body_start = strstr(req_buf, "\r\n\r\n");
    int header_len = 0;
    if (body_start) {
        header_len = (int)(body_start - req_buf) + 4;
        body_start += 4;
    }

    int content_length = 0;
    {
        char *cl = strcasestr(req_buf, "Content-Length:");
        if (cl) content_length = atoi(cl + 15);
    }

    int body_received = total - header_len;
    if (body_received < 0) body_received = 0;

    while (body_received < content_length && total < XWEBD_REQ_BUF_SIZE - 1) {
        if (time(NULL) - start_time >= XWEBD_REQUEST_TIMEOUT) return -1;
        struct pollfd pfd = {client_fd, POLLIN, 0};
        int pret = poll(&pfd, 1, 1000);
        if (pret < 0) return -1;
        if (pret == 0) continue;
        int n = recv(client_fd, req_buf + total, XWEBD_REQ_BUF_SIZE - 1 - total, 0);
        if (n <= 0) break;
        total += n;
        body_received += n;
    }
    req_buf[total] = '\0';

    const char *body = body_start;
    if (body_received <= 0 || content_length == 0) body = NULL;

    char content_type[256] = "";
    {
        char *ct = strcasestr(req_buf, "Content-Type:");
        if (ct) {
            ct += 13;
            while (*ct == ' ') ct++;
            int i = 0;
            while (*ct && *ct != '\r' && *ct != '\n' && i < (int)sizeof(content_type) - 1)
                content_type[i++] = *ct++;
            content_type[i] = '\0';
        }
    }

    XLOG_D(TAG, "%s %s", method, path);

    {
        pid_t wp;
        while ((wp = waitpid(-1, NULL, WNOHANG)) > 0) {
            if (wp == g_upload_pid) g_upload_pid = 0;
        }
    }

    /* 上传路由(特殊处理，需要额外参数) */
    if (strcmp(method, "POST") == 0) {
        if (strcmp(path, "/api/files/upload") == 0)
            return handle_upload(client_fd, content_type, content_length, body, body_received);
        if (strcmp(path, "/api/upload") == 0)
            return handle_upload_raw(client_fd, content_type, content_length, body, body_received, req_buf);
    }

    /* 功能插件化改写(纯粹化: 四功能全量交插件, 未装=404/离线=503 由网关回复;
     * download/upload 大文件流式走核心) */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/files") == 0)
        return xwplug_forward(client_fd, "GET", "files/list", query, NULL, 0);
    if (strcmp(method, "DELETE") == 0 && strcmp(path, "/api/files") == 0)
        return xwplug_forward(client_fd, "GET", "files/delete", query, NULL, 0);
    if (strcmp(method, "DELETE") == 0 && strcmp(path, "/api/files/download") == 0)
        return xwplug_forward(client_fd, "GET", "files/delete", query, NULL, 0);
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/files/batch-delete") == 0)
        return xwplug_forward(client_fd, "POST", "files/batch-delete", query, body, body_received);
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/files/cleanup") == 0)
        return xwplug_forward(client_fd, "GET", "files/cleanup", query, NULL, 0);
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/processes") == 0)
        return xwplug_forward(client_fd, "GET", "procs/list", query, NULL, 0);
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/processes/control") == 0)
        return xwplug_forward(client_fd, "POST", "procs/control", query, body, body_received);
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/usb/mode") == 0)
        return xwplug_forward(client_fd, "GET", "usb/mode", query, NULL, 0);
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/usb/mode") == 0)
        return xwplug_forward(client_fd, "POST", "usb/mode", query, body, body_received);
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/backlight") == 0)
        return xwplug_forward(client_fd, "GET", "backlight/state", query, NULL, 0);
    if (strcmp(method, "PUT") == 0 && strcmp(path, "/api/backlight") == 0)
        return xwplug_forward(client_fd, "PUT", "backlight/state", query, body, body_received);

    /* 路由表查找 */
    for (int i = 0; g_routes[i].handler; i++) {
        if (strcmp(method, g_routes[i].method) == 0 && strcmp(path, g_routes[i].path) == 0)
            return g_routes[i].handler(client_fd, body, query);
    }

    /* 插件转发: /api/plugin/<name>/<sub...> (精确路由优先, 故插件不会遮蔽内置端点) */
    if (strncmp(path, "/api/plugin/", 12) == 0)
        return xwplug_forward(client_fd, method, path + 12, query, body, body_received);

    /* CORS预检 */
    if (strcmp(method, "OPTIONS") == 0)
        return send_json(client_fd, 200, "{}");

    return send_error(client_fd, 404, "Not found");
}

/* ===== 工作进程与看门狗 ===== */

static void cleanup_startup_residuals(void) {
    if (access(XWEBD_TEST_SH_NEW, F_OK) == 0) {
        XLOG_I(TAG, "清理残留文件 %s", XWEBD_TEST_SH_NEW);
        unlink(XWEBD_TEST_SH_NEW);
    }
}

static void signal_handler(int sig) {
    if (sig == SIGTERM || sig == SIGINT) {
        XLOG_I(TAG, "收到信号 %d, 设置g_running=0", sig);
        g_running = 0;
    }
}

static void worker_loop(void) {
    g_running = 1;
    prctl(PR_SET_PDEATHSIG, SIGTERM);

    xlog_init(NULL);
    XLOG_I(TAG, "xwebd工作进程已启动 (v%s, 端口 %d)", XWEBD_VERSION, g_port);

    cpu_sample_update();
    cleanup_startup_residuals();
    load_persist_conf();
    apply_persist_conf();
    xwplug_init();

    g_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server_fd < 0) {
        XLOG_E(TAG, "创建服务器套接字失败: %s", strerror(errno));
        _exit(2);
    }

    int opt = 1;
    setsockopt(g_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(g_port);

    if (bind(g_server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        XLOG_E(TAG, "端口 %d 已被占用: %s", g_port, strerror(errno));
        if (!g_daemon) fprintf(stderr, "端口 %d 已被占用\n", g_port);
        close(g_server_fd);
        _exit(2);
    }

    if (listen(g_server_fd, 4) != 0) {
        XLOG_E(TAG, "监听失败: %s", strerror(errno));
        close(g_server_fd);
        _exit(2);
    }

    XLOG_I(TAG, "xwebd正在监听端口 %d", g_port);

    while (g_running) {
        while (waitpid(-1, NULL, WNOHANG) > 0) {}
        xwplug_tick();

        if (g_led_pending_value >= 0 && time(NULL) >= g_led_retry_time) {
            if (send_led_cmd(g_led_pending_value) == 0) {
                XLOG_I(TAG, "呼吸灯延迟恢复: 成功 enable=%d", g_led_pending_value);
                g_led_pending_value = -1;
            } else {
                g_led_retry_time = time(NULL) + 10;
            }
        }

        struct pollfd pfd = {g_server_fd, POLLIN, 0};
        int pret = poll(&pfd, 1, 1000);
        if (pret <= 0) continue;

        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(g_server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) continue;

        struct timeval tv = {XWEBD_REQUEST_TIMEOUT, 0};
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        int ret = handle_request(client_fd);
        if (ret != 1) close(client_fd);
    }

    if (g_upload_pid > 0) {
        XLOG_I(TAG, "等待上传子进程结束 (pid=%d)...", g_upload_pid);
        int waited = 0;
        while (waited < 5) {
            if (waitpid(g_upload_pid, NULL, WNOHANG) > 0) break;
            usleep(1000000);
            waited++;
        }
        if (waited >= 5) kill(g_upload_pid, SIGTERM);
    }

    close(g_server_fd);
    xwplug_shutdown();
    xlog_close();
    XLOG_I(TAG, "xwebd工作进程退出 (g_running=%d)", g_running);
}

static void print_usage(const char *prog) {
    fprintf(stderr, "用法: %s [-p 端口] [-d]\n", prog);
    fprintf(stderr, "  -p 端口   监听端口 (默认 %d)\n", XWEBD_DEFAULT_PORT);
    fprintf(stderr, "  -d        以守护进程模式运行\n");
}

int main(int argc, char *argv[]) {
    int is_worker = 0;
    int new_argc = 1;
    char *new_argv[argc + 1];
    new_argv[0] = argv[0];
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--worker") == 0) { is_worker = 1; continue; }
        new_argv[new_argc++] = argv[i];
    }
    new_argv[new_argc] = NULL;

    int opt;
    while ((opt = getopt(new_argc, new_argv, "p:d")) != -1) {
        switch (opt) {
            case 'p': g_port = atoi(optarg); break;
            case 'd': g_daemon = 1; break;
            default: print_usage(argv[0]); return 1;
        }
    }

    if (is_worker) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = signal_handler;
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGINT, &sa, NULL);
        signal(SIGPIPE, SIG_IGN);
        worker_loop();
        return 0;
    }

    if (g_daemon) {
        pid_t pid = fork();
        if (pid < 0) return 1;
        if (pid > 0) _exit(0);
        setsid();
        close(0); close(1); close(2);
        open("/dev/null", O_RDONLY);
        open("/dev/null", O_WRONLY);
        open("/dev/null", O_WRONLY);
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    pid_t worker_pid = fork();
    if (worker_pid < 0) {
        XLOG_E(TAG, "fork失败: %s", strerror(errno));
        return 1;
    }

    if (worker_pid == 0) {
        worker_loop();
        _exit(0);
    }

    xlog_init(NULL);
    XLOG_I(TAG, "xwebd看门狗已启动, 工作进程pid=%d", worker_pid);

    time_t last_health_check = time(NULL);
    int health_fail_count = 0;
    int restart_delay = 2;

    while (1) {
        int status;

        pid_t ret = waitpid(worker_pid, &status, WNOHANG);
        if (ret < 0) {
            if (errno == EINTR) { sleep(1); continue; }
            break;
        }

        if (ret > 0) {
            if (WIFEXITED(status)) {
                int exit_code = WEXITSTATUS(status);
                if (exit_code == 0) {
                    XLOG_I(TAG, "工作进程正常退出, 看门狗退出");
                    break;
                } else if (exit_code == 2) {
                    XLOG_E(TAG, "工作进程遇到不可恢复错误退出 (代码 %d), 不再重启", exit_code);
                    break;
                } else {
                    XLOG_W(TAG, "工作进程崩溃 (代码 %d)", exit_code);
                }
            } else if (WIFSIGNALED(status)) {
                XLOG_W(TAG, "工作进程被信号 %d 终止", WTERMSIG(status));
            } else {
                break;
            }

            time_t now = time(NULL);
            if (g_watchdog_crash_start == 0 || now - g_watchdog_crash_start > XWEBD_WATCHDOG_CRASH_WINDOW) {
                g_watchdog_crash_count = 1;
                g_watchdog_crash_start = now;
            } else {
                g_watchdog_crash_count++;
            }

            if (g_watchdog_crash_count >= XWEBD_WATCHDOG_CRASH_LIMIT) {
                XLOG_E(TAG, "工作进程崩溃 %d 次(在 %d 秒内), 放弃重启",
                       g_watchdog_crash_count, XWEBD_WATCHDOG_CRASH_WINDOW);
                break;
            }

            XLOG_I(TAG, "%d秒后重启工作进程...", restart_delay);
            sleep(restart_delay);
            if (restart_delay < 30) restart_delay *= 2;
            if (restart_delay > 30) restart_delay = 30;

            worker_pid = fork();
            if (worker_pid < 0) {
                XLOG_E(TAG, "fork失败, 看门狗退出");
                break;
            }
            if (worker_pid == 0) {
                char self_path[512];
                int n = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
                if (n > 0) {
                    self_path[n] = '\0';
                    char *child_argv[] = {self_path, "--worker", NULL};
                    execvp(self_path, child_argv);
                }
                worker_loop();
                _exit(1);
            }
            XLOG_I(TAG, "工作进程已重启, 新pid=%d", worker_pid);
            last_health_check = time(NULL);
            health_fail_count = 0;
            restart_delay = 2;
            continue;
        }

        time_t now = time(NULL);
        if (now - last_health_check >= XWEBD_HEALTH_CHECK_INTERVAL) {
            last_health_check = now;

            int need_restart = 0;

            if (worker_pid > 0 && kill(worker_pid, 0) != 0) {
                health_fail_count++;
                XLOG_W(TAG, "健康检查: 工作进程不存在 (%d/%d)", health_fail_count, XWEBD_HEALTH_FAIL_LIMIT);
            } else if (worker_pid > 0) {
                int sock = socket(AF_INET, SOCK_STREAM, 0);
                if (sock >= 0) {
                    struct timeval tv = {5, 0};
                    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
                    struct sockaddr_in addr;
                    memset(&addr, 0, sizeof(addr));
                    addr.sin_family = AF_INET;
                    addr.sin_port = htons(g_port);
                    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                        const char *req = "GET /api/ping HTTP/1.0\r\n\r\n";
                        send(sock, req, strlen(req), 0);
                        char rbuf[256];
                        int n = recv(sock, rbuf, sizeof(rbuf) - 1, 0);
                        close(sock);
                        if (n <= 0 || !memmem(rbuf, n, "200", 3)) {
                            health_fail_count++;
                            XLOG_W(TAG, "健康检查: HTTP ping失败 (%d/%d)", health_fail_count, XWEBD_HEALTH_FAIL_LIMIT);
                        } else {
                            health_fail_count = 0;
                        }
                    } else {
                        close(sock);
                        health_fail_count++;
                        XLOG_W(TAG, "健康检查: 连接失败 (%d/%d)", health_fail_count, XWEBD_HEALTH_FAIL_LIMIT);
                    }
                }
            }

            if (health_fail_count >= XWEBD_HEALTH_FAIL_LIMIT) {
                XLOG_E(TAG, "工作进程连续%d次健康检查失败, 强制重启", health_fail_count);
                need_restart = 1;
            }

            if (!need_restart && worker_pid > 0) {
                char stat_path[64];
                snprintf(stat_path, sizeof(stat_path), "/proc/%d/status", worker_pid);
                FILE *sf = fopen(stat_path, "r");
                if (sf) {
                    char line[256];
                    while (fgets(line, sizeof(line), sf)) {
                        if (strncmp(line, "VmRSS:", 6) == 0) {
                            long rss = atol(line + 6);
                            if (rss > XWEBD_MAX_RSS_KB) {
                                XLOG_E(TAG, "工作进程内存超限: %ldKB > %dKB, 强制重启", rss, XWEBD_MAX_RSS_KB);
                                need_restart = 1;
                            }
                            break;
                        }
                    }
                    fclose(sf);
                }
            }

            if (need_restart) {
                kill(worker_pid, SIGKILL);
                sleep(1);
                waitpid(worker_pid, &status, 0);
                XLOG_I(TAG, "工作进程已被强制终止, %d秒后重启...", restart_delay);
                sleep(restart_delay);
                if (restart_delay < 30) restart_delay *= 2;
                if (restart_delay > 30) restart_delay = 30;

                worker_pid = fork();
                if (worker_pid < 0) {
                    XLOG_E(TAG, "fork失败, 看门狗退出");
                    break;
                }
                if (worker_pid == 0) {
                    worker_loop();
                    _exit(0);
                }
                XLOG_I(TAG, "工作进程已重启, 新pid=%d", worker_pid);
                health_fail_count = 0;
            }
        }

        sleep(1);
    }

    xlog_close();
    return 0;
}
