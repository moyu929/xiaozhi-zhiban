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
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <poll.h>
#include <ctype.h>
#include <dirent.h>
#include <limits.h>

#define TAG "XWD"

static volatile int g_running = 1;       /* 服务器运行标志，收到SIGTERM时置0 */
static int g_server_fd = -1;             /* 服务器监听套接字 */
static int g_port = XWEBD_DEFAULT_PORT;  /* 监听端口 */
static int g_daemon = 0;                 /* 是否以守护进程模式运行 */

static int g_upload_max_mb = XWEBD_UPLOAD_MAX_MB_DEFAULT; /* 当前上传文件大小限制(MB) */
static volatile pid_t g_upload_pid = 0;  /* 当前上传子进程PID */

static int g_plog_fd = -1;              /* 日志文件描述符 */
static int g_plog_level = 2;            /* 日志级别(0=ERROR, 1=WARN, 2=INFO, 3=DEBUG) */
static pthread_mutex_t g_plog_mutex = PTHREAD_MUTEX_INITIALIZER; /* 日志写入互斥锁 */
static off_t g_plog_size = 0;           /* 当前日志文件大小 */
static char g_plog_path[256] = XWEBD_LOG_PATH; /* 日志文件路径 */

static int g_watchdog_crash_count = 0;   /* 看门狗: 当前时间窗口内崩溃计数 */
static time_t g_watchdog_crash_start = 0; /* 看门狗: 当前崩溃统计窗口起始时间 */

/* 日志级别宏: E=错误, W=警告, I=信息, D=调试 */
#define XLOG_E(tag, fmt, ...) xlog_write(0, tag, fmt, ##__VA_ARGS__)
#define XLOG_W(tag, fmt, ...) xlog_write(1, tag, fmt, ##__VA_ARGS__)
#define XLOG_I(tag, fmt, ...) xlog_write(2, tag, fmt, ##__VA_ARGS__)
#define XLOG_D(tag, fmt, ...) xlog_write(3, tag, fmt, ##__VA_ARGS__)

static const char *level_names[] = {"E", "W", "I", "D"};

/**
 * xlog_rotate - 日志文件轮转
 *
 * 当日志文件超过XWEBD_LOG_MAX_SIZE时，保留文件尾部XWEBD_LOG_KEEP_SIZE的内容，
 * 删除前半部分，避免日志文件无限增长。
 * 轮转时会跳过第一行不完整的日志（从换行符处开始保留）。
 */
static void xlog_rotate(void) {
    if (g_plog_size < XWEBD_LOG_MAX_SIZE) return;
    close(g_plog_fd);
    g_plog_fd = -1;

    int src_fd = open(g_plog_path, O_RDONLY);
    if (src_fd < 0) {
        g_plog_fd = open(g_plog_path, O_WRONLY | O_CREAT | O_TRUNC | O_SYNC, 0644);
        g_plog_size = 0;
        return;
    }

    off_t file_size = lseek(src_fd, 0, SEEK_END);
    off_t keep_offset = 0;
    if (file_size > XWEBD_LOG_KEEP_SIZE) keep_offset = file_size - XWEBD_LOG_KEEP_SIZE;

    char *buf = malloc(XWEBD_LOG_KEEP_SIZE);
    if (!buf) {
        close(src_fd);
        g_plog_fd = open(g_plog_path, O_WRONLY | O_CREAT | O_TRUNC | O_SYNC, 0644);
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

    /* 跳过第一行不完整的日志，从换行符后开始保留 */
    char *start = buf;
    if (total_read > 0 && keep_offset > 0) {
        char *nl = memchr(buf, '\n', total_read);
        if (nl) { start = nl + 1; total_read -= (start - buf); }
    }

    g_plog_fd = open(g_plog_path, O_WRONLY | O_CREAT | O_TRUNC | O_SYNC, 0644);
    if (g_plog_fd >= 0 && total_read > 0) {
        write(g_plog_fd, start, total_read);
        g_plog_size = total_read;
    } else {
        g_plog_size = 0;
    }
    free(buf);
}

/**
 * xlog_init - 初始化日志系统
 * @path: 日志文件路径，为NULL时使用默认路径g_plog_path
 *
 * 打开日志文件并获取当前文件大小，用于后续轮转判断。
 * 线程安全，通过g_plog_mutex保护。
 */
static void xlog_init(const char *path) {
    pthread_mutex_lock(&g_plog_mutex);
    if (g_plog_fd >= 0) close(g_plog_fd);
    if (path) { strncpy(g_plog_path, path, sizeof(g_plog_path) - 1); g_plog_path[sizeof(g_plog_path) - 1] = '\0'; }
    g_plog_fd = open(g_plog_path, O_WRONLY | O_CREAT | O_APPEND | O_SYNC, 0644);
    if (g_plog_fd >= 0) {
        struct stat st;
        if (fstat(g_plog_fd, &st) == 0) g_plog_size = st.st_size;
        else g_plog_size = 0;
    }
    pthread_mutex_unlock(&g_plog_mutex);
}

/**
 * xlog_close - 关闭日志系统
 *
 * 同步并关闭日志文件描述符，线程安全。
 */
static void xlog_close(void) {
    pthread_mutex_lock(&g_plog_mutex);
    if (g_plog_fd >= 0) { fsync(g_plog_fd); close(g_plog_fd); g_plog_fd = -1; }
    pthread_mutex_unlock(&g_plog_mutex);
}

/**
 * xlog_vwrite - 写入日志（可变参数列表版本）
 * @level: 日志级别(0=ERROR, 1=WARN, 2=INFO, 3=DEBUG)
 * @tag:   日志标签
 * @fmt:   格式化字符串
 * @ap:    可变参数列表
 *
 * 日志格式: [HH:MM:SS][级别][标签] 消息内容
 * 超过当前日志级别的消息会被忽略，写入前检查是否需要轮转。
 */
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
    }
    pthread_mutex_unlock(&g_plog_mutex);
}

/**
 * xlog_write - 写入日志（包装函数）
 * @level: 日志级别(0=ERROR, 1=WARN, 2=INFO, 3=DEBUG)
 * @tag:   日志标签
 * @fmt:   格式化字符串
 *
 * 将可变参数转换为va_list后调用xlog_vwrite。
 */
static void xlog_write(int level, const char *tag, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    xlog_vwrite(level, tag, fmt, ap);
    va_end(ap);
}

/**
 * send_response - 发送HTTP响应
 * @fd:           客户端套接字描述符
 * @status:       HTTP状态码(200, 400, 404, 500, 503)
 * @content_type: 响应Content-Type头
 * @body:         响应体数据
 * @body_len:     响应体长度
 *
 * 返回: 成功返回0，发送失败返回-1
 * 503状态码会自动添加Retry-After: 5头
 */
static int send_response(int fd, int status, const char *content_type, const char *body, int body_len) {
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

/**
 * send_json - 发送JSON格式的HTTP响应
 * @fd:     客户端套接字描述符
 * @status: HTTP状态码
 * @json:   JSON字符串
 *
 * 返回: 成功返回0，失败返回-1
 */
static int send_json(int fd, int status, const char *json) {
    return send_response(fd, status, "application/json", json, strlen(json));
}

/**
 * send_error - 发送错误响应
 * @fd:      客户端套接字描述符
 * @code:    HTTP错误状态码
 * @message: 错误消息字符串
 *
 * 返回: 成功返回0，失败返回-1
 * 响应格式: {"error":"消息"}
 */
static int send_error(int fd, int code, const char *message) {
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", message);
    return send_json(fd, code, buf);
}

/**
 * parse_json_int - 从JSON字符串中解析整数值
 * @json: JSON字符串
 * @key:  要查找的键名
 * @out:  输出整数值的指针
 *
 * 返回: 成功返回0，未找到键返回-1
 * 简易解析，仅支持顶层字段
 */
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

/**
 * parse_json_str - 从JSON字符串中解析字符串值
 * @json:     JSON字符串
 * @key:      要查找的键名
 * @out:      输出字符串缓冲区
 * @out_size: 输出缓冲区大小
 *
 * 返回: 成功返回0，未找到键或格式错误返回-1
 * 支持转义字符(如\")
 */
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

/**
 * hex_to_int - 十六进制字符转整数
 * @c: 十六进制字符(0-9, a-f, A-F)
 *
 * 返回: 对应的整数值(0-15)，非法字符返回-1
 */
static int hex_to_int(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/**
 * url_decode - URL解码
 * @src:      待解码的URL编码字符串
 * @dst:      解码结果输出缓冲区
 * @dst_size: 输出缓冲区大小
 *
 * 返回: 解码后的字符串长度
 * 支持%XX编码和+转空格
 */
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

/**
 * validate_path - 校验文件路径安全性
 * @raw_path:      原始请求路径
 * @resolved:      解析后的安全路径输出缓冲区
 * @resolved_size: 输出缓冲区大小
 *
 * 返回: 路径合法返回0，不合法返回-1
 * 安全检查:
 * 1. 禁止路径中包含".."防止目录遍历
 * 2. 解析后的路径必须以XWEBD_BASE_DIR为前缀
 * 3. 前缀后必须紧跟'/'或字符串结束
 */
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

/**
 * read_file_string - 读取文件内容为字符串
 * @path:     文件路径
 * @buf:      输出缓冲区
 * @buf_size: 缓冲区大小
 *
 * 返回: 读取的字符串长度(去除尾部空白)，失败返回-1
 * 自动去除尾部换行符、回车符和空格
 */
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

/**
 * get_volume - 获取当前音量
 *
 * 通过tinymix工具读取硬件音量值，并转换为0-XWEBD_VOLUME_MAX的范围。
 * 返回: 音量值(0-80)，获取失败返回-1
 */
static int get_volume(void) {
    char buf[128];
    if (access(XWEBD_TINYMIX_PATH, X_OK) == 0) {
        char cmd[64];
        snprintf(cmd, sizeof(cmd), XWEBD_TINYMIX_PATH " %d 2>/dev/null", XWEBD_VOLUME_TINYMIX_ID);
        FILE *f = popen(cmd, "r");
        if (f) {
            if (fgets(buf, sizeof(buf), f)) {
                pclose(f);
                char *colon = strchr(buf, ':');
                if (colon) {
                    int hw_vol = atoi(colon + 1);
                    if (hw_vol >= 0) {
                        if (hw_vol > XWEBD_VOLUME_HW_MAX) hw_vol = XWEBD_VOLUME_HW_MAX;
                        return hw_vol * XWEBD_VOLUME_MAX / XWEBD_VOLUME_HW_MAX;
                    }
                }
            } else { pclose(f); }
        }
    }

    return -1;
}

/**
 * set_volume - 设置音量
 * @vol: 目标音量值(0-80)，超出范围会被自动截断
 *
 * 将逻辑音量转换为硬件音量后通过tinymix设置。
 * 返回: 成功返回0，失败返回-1
 */
static int set_volume(int vol) {
    if (vol < XWEBD_VOLUME_MIN) vol = XWEBD_VOLUME_MIN;
    if (vol > XWEBD_VOLUME_MAX) vol = XWEBD_VOLUME_MAX;

    if (access(XWEBD_TINYMIX_PATH, X_OK) == 0) {
        int hw_vol = vol * XWEBD_VOLUME_HW_MAX / XWEBD_VOLUME_MAX;
        char cmd[128];
        snprintf(cmd, sizeof(cmd), XWEBD_TINYMIX_PATH " %d %d", XWEBD_VOLUME_TINYMIX_ID, hw_vol);
        int ret = system(cmd);
        if (ret == 0) return 0;
        XLOG_W(TAG, "tinymix设置音量失败, ret=%d", ret);
    }

    return -1;
}

/**
 * get_brightness - 获取当前屏幕亮度
 *
 * 从sysfs背光节点读取亮度值。
 * 返回: 亮度值(0-900)，读取失败返回-1
 */
static int get_brightness(void) {
    char buf[64];
    int n = read_file_string(XWEBD_BACKLIGHT_PATH, buf, sizeof(buf));
    if (n <= 0) return -1;
    return atoi(buf);
}

/**
 * set_brightness - 设置屏幕亮度
 * @val: 目标亮度值(0-900)，超出范围会被自动截断
 *
 * 通过sysfs背光节点写入亮度值。
 * 返回: 成功返回0，失败返回-1
 */
static int set_brightness(int val) {
    if (val < XWEBD_BRIGHTNESS_MIN) val = XWEBD_BRIGHTNESS_MIN;
    if (val > XWEBD_BRIGHTNESS_MAX) val = XWEBD_BRIGHTNESS_MAX;
    int fd = open(XWEBD_BACKLIGHT_PATH, O_WRONLY);
    if (fd < 0) return -1;
    char buf[16];
    int len = snprintf(buf, sizeof(buf), "%d", val);
    int w = write(fd, buf, len);
    close(fd);
    return (w == len) ? 0 : -1;
}

/**
 * get_mute - 获取当前静音状态
 *
 * 通过tinymix读取静音控制值，Off表示已静音，On表示未静音。
 * 返回: 1=已静音, 0=未静音, 获取失败返回-1
 */
static int get_mute(void) {
    if (access(XWEBD_TINYMIX_PATH, X_OK) == 0) {
        char cmd[64];
        snprintf(cmd, sizeof(cmd), XWEBD_TINYMIX_PATH " %d 2>/dev/null", XWEBD_MUTE_TINYMIX_ID);
        FILE *f = popen(cmd, "r");
        if (f) {
            char buf[128];
            if (fgets(buf, sizeof(buf), f)) {
                pclose(f);
                if (strstr(buf, "Off")) return 1;
                if (strstr(buf, "On")) return 0;
            } else { pclose(f); }
        }
    }
    return -1;
}

/**
 * set_mute - 设置静音状态
 * @muted: 1=静音, 0=取消静音
 *
 * 通过tinymix设置静音控制值，0=静音, 1=取消静音(tinymix参数语义)。
 * 返回: 成功返回0，失败返回-1
 */
static int set_mute(int muted) {
    if (access(XWEBD_TINYMIX_PATH, X_OK) == 0) {
        char cmd[128];
        snprintf(cmd, sizeof(cmd), XWEBD_TINYMIX_PATH " %d %s", XWEBD_MUTE_TINYMIX_ID, muted ? "0" : "1");
        int ret = system(cmd);
        if (ret == 0) return 0;
        XLOG_W(TAG, "tinymix设置静音失败, ret=%d", ret);
    }
    return -1;
}

/**
 * is_protected_file - 检查文件是否为受保护文件
 * @name: 文件名(不含路径)
 *
 * 返回: 受保护文件返回1，否则返回0
 * 受保护文件禁止通过API删除，列表由XWEBD_PROTECT_FILES宏定义
 */
static int is_protected_file(const char *name) {
    const char *p = XWEBD_PROTECT_FILES;
    while (*p) {
        if (strcmp(name, p) == 0) return 1;
        p += strlen(p) + 1;
    }
    return 0;
}

/**
 * is_truncate_file - 检查文件是否为截断清理文件
 * @name: 文件名(不含路径)
 *
 * 返回: 截断文件返回1，否则返回0
 * 截断文件在清理时只清空内容而非删除，列表由XWEBD_TRUNCATE_FILES宏定义
 */
static int is_truncate_file(const char *name) {
    const char *p = XWEBD_TRUNCATE_FILES;
    while (*p) {
        if (strcmp(name, p) == 0) return 1;
        p += strlen(p) + 1;
    }
    return 0;
}

/**
 * handle_get_ping - 处理GET /api/ping请求
 *
 * 心跳检测接口，返回{"ok":true}
 */
static int handle_get_ping(int fd) {
    return send_json(fd, 200, "{\"ok\":true}");
}

/**
 * handle_get_version - 处理GET /api/version请求
 *
 * 返回服务器版本号
 */
static int handle_get_version(int fd) {
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"version\":\"%s\"}", XWEBD_VERSION);
    return send_json(fd, 200, buf);
}

/**
 * handle_get_config - 处理GET /api/config请求
 *
 * 返回当前运行配置，包括上传大小限制和日志级别
 */
static int handle_get_config(int fd) {
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"upload_max_mb\":%d,\"log_level\":\"%s\"}",
             g_upload_max_mb,
             g_plog_level == 3 ? "DEBUG" : g_plog_level == 2 ? "INFO" : g_plog_level == 1 ? "WARN" : "ERROR");
    return send_json(fd, 200, buf);
}

/**
 * handle_put_config - 处理PUT /api/config请求
 * @fd:   客户端套接字描述符
 * @body: 请求体JSON字符串
 *
 * 修改运行时配置，支持字段:
 * - upload_max_mb: 上传文件大小限制(1-100 MB)
 * - log_level: 日志级别(DEBUG/INFO/WARN/ERROR)
 *
 * 返回: 成功返回0，参数错误返回负数
 */
static int handle_put_config(int fd, const char *body) {
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

/**
 * handle_get_system - 处理GET /api/system请求
 *
 * 采集并返回设备系统信息，包括:
 * - 设备型号、内核版本、CPU型号
 * - 内存总量/空闲/缓存
 * - 系统运行时间、电池电量
 * - WiFi连接状态和IP地址
 * - 磁盘总量/已用/可用
 * - 音量、亮度、静音状态
 * - 助手程序安装和运行状态
 */
static int handle_get_system(int fd) {
    char buf[XWEBD_RESP_BUF_SIZE];
    int len = 0;

    char model[64] = "Unknown", kernel[64] = "Unknown", cpu[128] = "";
    long mem_total = 0, mem_free = 0, mem_cached = 0;
    double uptime = 0;
    int battery = -1;
    int wifi_connected = 0;
    char wifi_ip[64] = "";
    long disk_total = 0, disk_used = 0, disk_free = 0;
    int volume = -1, brightness = -1, muted = -1;
    int assistant_installed = 0, assistant_running = 0;
    char assistant_version[32] = "";

    /* 读取设备型号 */
    {
        FILE *f = fopen("/proc/device-tree/model", "r");
        if (f) { if (fgets(model, sizeof(model), f)) { char *nl = strchr(model, '\n'); if (nl) *nl = '\0'; } fclose(f); }
    }
    /* 读取内核版本 */
    {
        FILE *f = fopen("/proc/version", "r");
        if (f) { if (fgets(kernel, sizeof(kernel), f)) { char *nl = strchr(kernel, '\n'); if (nl) *nl = '\0'; } fclose(f); }
    }
    /* 读取CPU型号 */
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
    /* 读取内存信息 */
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
    /* 读取系统运行时间 */
    {
        char buf2[64];
        if (read_file_string("/proc/uptime", buf2, sizeof(buf2)) > 0) uptime = atof(buf2);
    }
    /* 读取电池电量 */
    {
        char buf2[16];
        if (read_file_string("/sys/class/power_supply/battery/capacity", buf2, sizeof(buf2)) > 0) battery = atoi(buf2);
    }
    /* 检测WiFi连接状态 */
    {
        FILE *f = fopen("/proc/net/wireless", "r");
        if (f) {
            char line[256];
            if (fgets(line, sizeof(line), f) && fgets(line, sizeof(line), f)) {
                wifi_connected = 1;
            }
            fclose(f);
        }
    }
    /* 获取wlan0的IP地址 */
    {
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock >= 0) {
            struct ifreq ifr;
            memset(&ifr, 0, sizeof(ifr));
            strncpy(ifr.ifr_name, "wlan0", IFNAMSIZ - 1);
            if (ioctl(sock, SIOCGIFADDR, &ifr) == 0) {
                struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
                strncpy(wifi_ip, inet_ntoa(sin->sin_addr), sizeof(wifi_ip) - 1);
            }
            close(sock);
        }
    }
    /* 获取磁盘使用情况 */
    {
        struct statfs st;
        if (statfs(XWEBD_BASE_DIR, &st) == 0) {
            disk_total = (long)((long long)st.f_blocks * st.f_bsize / 1024);
            disk_free = (long)((long long)st.f_bfree * st.f_bsize / 1024);
            disk_used = disk_total - disk_free;
        }
    }

    volume = get_volume();
    brightness = get_brightness();
    muted = get_mute();

    /* 检查助手程序(sair)安装和运行状态 */
    if (access(XWEBD_SAIR_BIN, X_OK) == 0) {
        assistant_installed = 1;
        char pid_buf[16];
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "pidof sair 2>/dev/null");
        FILE *pf = popen(cmd, "r");
        if (pf) {
            if (fgets(pid_buf, sizeof(pid_buf), pf)) assistant_running = 1;
            pclose(pf);
        }
    }

    len = snprintf(buf, sizeof(buf),
        "{\"model\":\"%s\",\"kernel\":\"%s\",\"cpu\":\"%s\","
        "\"mem_total_kb\":%ld,\"mem_free_kb\":%ld,\"mem_cached_kb\":%ld,"
        "\"uptime_s\":%.0f,\"battery_cap\":%d,"
        "\"wifi_connected\":%s,\"wifi_ip\":\"%s\","
        "\"disk_total_kb\":%ld,\"disk_used_kb\":%ld,\"disk_free_kb\":%ld,"
        "\"volume\":%d,\"brightness\":%d,\"muted\":%s,"
        "\"assistant_installed\":%s,\"assistant_running\":%s,\"assistant_version\":\"%s\"}",
        model, kernel, cpu,
        mem_total, mem_free, mem_cached,
        uptime, battery,
        wifi_connected ? "true" : "false", wifi_ip,
        disk_total, disk_used, disk_free,
        volume, brightness, muted < 0 ? "null" : (muted ? "true" : "false"),
        assistant_installed ? "true" : "false", assistant_running ? "true" : "false", assistant_version);

    return send_response(fd, 200, "application/json", buf, len);
}

/**
 * handle_get_logs - 处理GET /api/logs请求
 * @fd:    客户端套接字描述符
 * @query: URL查询字符串
 *
 * 查询参数:
 * - lines: 返回的日志行数(默认100, 最大500)
 * - level: 日志级别过滤(E/W/I/D)
 * - source: 日志来源(1=sair, 2=xwebd, 不指定=全部)
 *
 * 使用环形缓冲区保留最新的指定行数日志
 */
static int handle_get_logs(int fd, const char *query) {
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
    buf_pos += snprintf(buf + buf_pos, XWEBD_RESP_BUF_SIZE * 4 - buf_pos, "{\"logs\":[");

    int total_lines = 0;
    int written = 0;

    for (int li = 0; li < log_count && total_lines < lines; li++) {
        int idx = start_idx + li;
        FILE *f = fopen(log_paths[idx], "r");
        if (!f) continue;

        /* 对于大文件，只读取尾部64KB以提高效率 */
        fseek(f, 0, SEEK_END);
        long file_size = ftell(f);

        int want_lines = lines - total_lines;
        long read_start = 0;
        if (file_size > 65536) {
            read_start = file_size - 65536;
        }
        fseek(f, read_start, SEEK_SET);

        char line[512];
        /* 环形缓冲区，保留最新的want_lines行 */
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

            /* 从日志格式[HH:MM:SS][级别][TAG]中提取级别字符 */
            char line_level = 0;
            char *lb = strchr(line, '[');
            if (lb) {
                char *rb = strchr(lb + 1, ']');
                if (rb && rb > lb + 1) {
                    char *lb2 = strchr(rb + 1, '[');
                    if (lb2) {
                        char *rb2 = strchr(lb2 + 1, ']');
                        if (rb2 && rb2 == lb2 + 2) {
                            line_level = *(lb2 + 1);
                        }
                    }
                }
            }

            /* 按日志级别过滤 */
            if (level_filter) {
                char effective_level = line_level ? line_level : 'I';
                if (effective_level != level_filter) continue;
            }

            /* 环形缓冲区: 满时淘汰最旧的行 */
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

        /* 将环形缓冲区中的日志行写入响应JSON */
        for (int i = 0; i < ring_count && total_lines < lines; i++) {
            if (written > 0) buf_pos += snprintf(buf + buf_pos, XWEBD_RESP_BUF_SIZE * 4 - buf_pos, ",");
            const char *lvl = "INFO";
            switch (ring_level[i]) {
                case 'E': lvl = "ERROR"; break;
                case 'W': lvl = "WARN"; break;
                case 'I': lvl = "INFO"; break;
                case 'D': lvl = "DEBUG"; break;
            }
            /* 转义JSON特殊字符 */
            char escaped[1024];
            int ep = 0;
            for (const char *s = ring[i]; *s && ep < (int)sizeof(escaped) - 2; s++) {
                if (*s == '"' || *s == '\\') escaped[ep++] = '\\';
                escaped[ep++] = *s;
            }
            escaped[ep] = '\0';
            buf_pos += snprintf(buf + buf_pos, XWEBD_RESP_BUF_SIZE * 4 - buf_pos,
                "{\"source\":\"%s\",\"level\":\"%s\",\"text\":\"%s\"}",
                log_names[idx], lvl, escaped);
            written++;
            total_lines++;
            free(ring[i]);
        }
        free(ring);
        free(ring_level);
    }

    buf_pos += snprintf(buf + buf_pos, XWEBD_RESP_BUF_SIZE * 4 - buf_pos, "],\"count\":%d}", total_lines);
    int ret = send_json(fd, 200, buf);
    free(buf);
    return ret;
}

/**
 * handle_get_volume - 处理GET /api/volume请求
 *
 * 返回当前音量值
 */
static int handle_get_volume(int fd) {
    int vol = get_volume();
    if (vol < 0) return send_error(fd, 500, "Volume control not available");
    char buf[32];
    snprintf(buf, sizeof(buf), "{\"volume\":%d}", vol);
    return send_json(fd, 200, buf);
}

/**
 * handle_post_volume - 处理POST /api/volume请求
 * @fd:   客户端套接字描述符
 * @body: 请求体JSON，需包含"volume"字段(0-80)
 *
 * 设置音量值
 */
static int handle_post_volume(int fd, const char *body) {
    if (!body) return send_error(fd, 400, "Empty request body");
    int vol;
    if (parse_json_int(body, "volume", &vol) != 0) return send_error(fd, 400, "Missing 'volume' field");
    if (vol < XWEBD_VOLUME_MIN || vol > XWEBD_VOLUME_MAX)
        return send_error(fd, 400, "Volume must be 0-80");
    if (set_volume(vol) != 0) return send_error(fd, 500, "Volume control not available");
    char buf[32];
    snprintf(buf, sizeof(buf), "{\"volume\":%d}", vol);
    return send_json(fd, 200, buf);
}

/**
 * handle_get_brightness - 处理GET /api/brightness请求
 *
 * 返回当前亮度值
 */
static int handle_get_brightness(int fd) {
    int br = get_brightness();
    if (br < 0) return send_error(fd, 500, "Brightness control not available");
    char buf[32];
    snprintf(buf, sizeof(buf), "{\"brightness\":%d}", br);
    return send_json(fd, 200, buf);
}

/**
 * handle_post_brightness - 处理POST /api/brightness请求
 * @fd:   客户端套接字描述符
 * @body: 请求体JSON，需包含"brightness"字段(0-900)
 *
 * 设置屏幕亮度值
 */
static int handle_post_brightness(int fd, const char *body) {
    if (!body) return send_error(fd, 400, "Empty request body");
    int val;
    if (parse_json_int(body, "brightness", &val) != 0) return send_error(fd, 400, "Missing 'brightness' field");
    if (val < XWEBD_BRIGHTNESS_MIN || val > XWEBD_BRIGHTNESS_MAX)
        return send_error(fd, 400, "Brightness must be 0-900");
    if (set_brightness(val) != 0) return send_error(fd, 500, "Brightness control not available");
    char buf[32];
    snprintf(buf, sizeof(buf), "{\"brightness\":%d}", val);
    return send_json(fd, 200, buf);
}

/**
 * handle_post_poweroff - 处理POST /api/poweroff请求
 *
 * 先发送响应再执行关机命令，避免客户端收不到响应
 */
static int handle_post_poweroff(int fd) {
    send_json(fd, 200, "{\"ok\":true}");
    system("poweroff");
    return 0;
}

/**
 * handle_post_reboot - 处理POST /api/reboot请求
 *
 * 先发送响应再执行重启命令，避免客户端收不到响应
 */
static int handle_post_reboot(int fd) {
    send_json(fd, 200, "{\"ok\":true}");
    system("reboot");
    return 0;
}

/**
 * handle_get_files - 处理GET /api/files请求
 * @fd:    客户端套接字描述符
 * @query: URL查询字符串，需包含"path"参数
 *
 * 列出指定目录下的文件和子目录信息(名称、大小、是否目录、修改时间)
 */
static int handle_get_files(int fd, const char *query) {
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

    DIR *dir = opendir(resolved);
    if (!dir) return send_error(fd, 404, "Directory not found");

    char buf[XWEBD_RESP_BUF_SIZE];
    int len = snprintf(buf, sizeof(buf), "{\"path\":\"%s\",\"files\":[", resolved);

    struct dirent *ent;
    int first = 1;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", resolved, ent->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0) continue;

        if (!first) len += snprintf(buf + len, sizeof(buf) - len, ",");
        first = 0;

        len += snprintf(buf + len, sizeof(buf) - len,
            "{\"name\":\"%s\",\"size\":%ld,\"is_dir\":%s,\"mtime\":%ld}",
            ent->d_name, (long)st.st_size, S_ISDIR(st.st_mode) ? "true" : "false", (long)st.st_mtime);

        /* 缓冲区快满时停止，避免溢出 */
        if ((size_t)len > sizeof(buf) - 256) break;
    }
    closedir(dir);

    len += snprintf(buf + len, sizeof(buf) - len, "]}");
    return send_response(fd, 200, "application/json", buf, len);
}

/**
 * handle_download_file - 处理GET /api/files/download请求
 * @fd:    客户端套接字描述符
 * @query: URL查询字符串，需包含"path"参数
 *
 * 以附件形式下载指定文件，流式传输避免大文件占用过多内存
 */
static int handle_download_file(int fd, const char *query) {
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

    /* 发送文件下载响应头 */
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: %ld\r\n"
        "Content-Disposition: attachment; filename=\"%s\"\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n",
        (long)st.st_size, fname);
    send(fd, header, hlen, MSG_NOSIGNAL);

    /* 流式传输文件内容 */
    char fbuf[XWEBD_FILE_BUF_SIZE];
    ssize_t total_sent = 0;
    while (total_sent < st.st_size) {
        ssize_t n = read(file_fd, fbuf, sizeof(fbuf));
        if (n <= 0) break;
        ssize_t sent = send(fd, fbuf, n, MSG_NOSIGNAL);
        if (sent <= 0) break;
        total_sent += sent;
    }
    close(file_fd);
    return 0;
}

/**
 * handle_delete_file - 处理DELETE /api/files请求
 * @fd:    客户端套接字描述符
 * @query: URL查询字符串，需包含"path"参数
 *
 * 删除指定文件，受保护文件禁止删除
 */
static int handle_delete_file(int fd, const char *query) {
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

    const char *fname = strrchr(resolved, '/');
    fname = fname ? fname + 1 : resolved;
    if (is_protected_file(fname)) return send_error(fd, 400, "Cannot delete protected file");

    if (remove(resolved) != 0) {
        if (errno == ENOENT) return send_error(fd, 404, "File not found");
        return send_error(fd, 500, "Delete failed");
    }
    return send_json(fd, 200, "{\"ok\":true}");
}

/**
 * handle_batch_delete - 处理POST /api/files/batch-delete请求
 * @fd:   客户端套接字描述符
 * @body: 请求体JSON，需包含"paths"数组字段
 *
 * 批量删除文件，受保护文件会被跳过，返回实际删除数量
 */
static int handle_batch_delete(int fd, const char *body) {
    if (!body) return send_error(fd, 400, "Empty request body");
    int deleted = 0;
    const char *p = strstr(body, "\"paths\"");
    if (!p) return send_error(fd, 400, "Missing 'paths' field");
    p = strchr(p, '[');
    if (!p) return send_error(fd, 400, "Invalid paths format");
    p++;

    /* 逐个解析paths数组中的路径并删除 */
    while (*p && *p != ']') {
        while (*p && (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
        if (*p != '"') { p++; continue; }
        p++;
        char path_val[PATH_MAX];
        int i = 0;
        while (*p && *p != '"' && i < PATH_MAX - 1) {
            if (*p == '\\' && *(p + 1)) { p++; path_val[i++] = *p++; }
            else path_val[i++] = *p++;
        }
        path_val[i] = '\0';
        if (*p == '"') p++;

        char resolved[PATH_MAX];
        if (validate_path(path_val, resolved, sizeof(resolved)) == 0) {
            const char *fname = strrchr(resolved, '/');
            fname = fname ? fname + 1 : resolved;
            if (!is_protected_file(fname) && remove(resolved) == 0) deleted++;
        }
    }

    char buf[32];
    snprintf(buf, sizeof(buf), "{\"deleted\":%d}", deleted);
    return send_json(fd, 200, buf);
}

/**
 * handle_cleanup - 处理POST /api/files/cleanup请求
 *
 * 清理XWEBD_BASE_DIR目录下的临时和可清理文件:
 * 1. 截断类文件(如xwebd.log): 清空内容但不删除
 * 2. .upload_pid文件: 仅在对应进程不存在时删除
 * 3. 匹配清理模式的文件: 直接删除
 * 4. .tmp后缀文件: 直接删除
 * 受保护文件不会被清理
 */
static int handle_cleanup(int fd) {
    long cleaned_bytes = 0;
    int cleaned_files = 0;

    DIR *dir = opendir(XWEBD_BASE_DIR);
    if (!dir) return send_error(fd, 500, "Cannot open base directory");

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", XWEBD_BASE_DIR, ent->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0 || S_ISDIR(st.st_mode)) continue;

        /* 跳过受保护文件 */
        if (is_protected_file(ent->d_name)) continue;

        /* 截断类文件: 清空内容但不删除 */
        if (is_truncate_file(ent->d_name)) {
            long old_size = st.st_size;
            int tfd = open(full_path, O_WRONLY | O_TRUNC);
            if (tfd >= 0) {
                close(tfd);
                cleaned_bytes += old_size;
                cleaned_files++;
            }
            continue;
        }

        /* .upload_pid文件: 仅在对应上传进程已不存在时删除 */
        if (strcmp(ent->d_name, ".upload_pid") == 0) {
            char pid_buf[16];
            int n = read_file_string(full_path, pid_buf, sizeof(pid_buf));
            if (n > 0) {
                int pid = atoi(pid_buf);
                char comm_path[64];
                snprintf(comm_path, sizeof(comm_path), "/proc/%d/comm", pid);
                char comm[64];
                if (read_file_string(comm_path, comm, sizeof(comm)) <= 0 || strstr(comm, "xwebd") == NULL) {
                    if (remove(full_path) == 0) { cleaned_bytes += st.st_size; cleaned_files++; }
                }
            } else {
                if (remove(full_path) == 0) { cleaned_bytes += st.st_size; cleaned_files++; }
            }
            continue;
        }

        /* 匹配清理模式列表的文件: 直接删除 */
        const char *cp = XWEBD_CLEANUP_PATTERNS;
        while (*cp) {
            if (strcmp(ent->d_name, cp) == 0) {
                if (remove(full_path) == 0) { cleaned_bytes += st.st_size; cleaned_files++; }
                break;
            }
            cp += strlen(cp) + 1;
        }

        /* .tmp后缀文件: 直接删除 */
        size_t namelen = strlen(ent->d_name);
        if (namelen >= 4 && strcmp(ent->d_name + namelen - 4, ".tmp") == 0) {
            if (remove(full_path) == 0) { cleaned_bytes += st.st_size; cleaned_files++; }
        }
    }
    closedir(dir);

    char buf[64];
    snprintf(buf, sizeof(buf), "{\"cleaned_bytes\":%ld,\"cleaned_files\":%d}", cleaned_bytes, cleaned_files);
    return send_json(fd, 200, buf);
}

/**
 * do_upload_child - multipart/form-data上传子进程函数
 * @client_fd:     客户端套接字描述符
 * @boundary:      multipart边界字符串
 * @content_length: 请求体总长度
 * @body_read:     已读取的请求体数据
 * @body_read_len: 已读取的请求体数据长度
 *
 * 在子进程中执行文件上传:
 * 1. 创建临时文件
 * 2. 解析multipart头部获取文件名
 * 3. 写入文件数据并去除结尾边界标记
 * 4. 校验目标路径安全性
 * 5. 重命名临时文件为最终文件名
 * 成功或失败后子进程直接_exit退出
 */
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
        /* 优先使用已读取的body数据，不足部分从socket继续读取 */
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
            /* 解析multipart头部，提取filename */
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
                if (body_data > 0) write(tmp_fd, fbuf + hdr_size, body_data);
            }
        } else if (found_file) {
            write(tmp_fd, fbuf, n);
        }
    }

    /* 去除文件末尾的multipart结束边界标记 */
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

/**
 * do_upload_raw_child - 原始二进制流上传子进程函数
 * @client_fd:      客户端套接字描述符
 * @filename:       目标文件名(从X-Filename请求头获取)
 * @content_length: 请求体总长度
 * @body_read:      已读取的请求体数据
 * @body_read_len:  已读取的请求体数据长度
 *
 * 直接将请求体数据写入文件，无需解析multipart格式。
 * 文件名通过X-Filename请求头指定。
 * 成功或失败后子进程直接_exit退出
 */
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
        if (n > 0) write(tmp_fd, fbuf, n);
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

/**
 * handle_upload_raw - 处理POST /api/upload请求(原始二进制流上传)
 * @client_fd:      客户端套接字描述符
 * @content_type:   Content-Type请求头值
 * @content_length: Content-Length值
 * @body_read:      已读取的请求体数据
 * @body_read_len:  已读取的请求体数据长度
 * @req_buf:        完整请求缓冲区(用于提取X-Filename头)
 *
 * 文件名通过X-Filename请求头指定，请求体直接作为文件内容。
 * 上传在子进程中执行，避免阻塞主进程。
 * 返回: 1表示上传在子进程中进行(连接暂不关闭)，<=0表示已完成或出错
 */
static int handle_upload_raw(int client_fd, const char *content_type, int content_length,
                              const char *body_read, int body_read_len, const char *req_buf) {
    if (content_length <= 0) return send_error(client_fd, 400, "Missing Content-Length");

    int max_bytes = g_upload_max_mb * 1024 * 1024;
    if (content_length > max_bytes) {
        char err[64];
        snprintf(err, sizeof(err), "File too large (max %dMB)", g_upload_max_mb);
        return send_error(client_fd, 400, err);
    }

    /* 检查是否有正在进行的上传 */
    pid_t existing = g_upload_pid;
    if (existing > 0) {
        char check[64];
        snprintf(check, sizeof(check), "/proc/%d/comm", existing);
        if (access(check, F_OK) == 0) return send_error(client_fd, 503, "Upload in progress");
    }

    /* 从X-Filename请求头提取文件名 */
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
        /* 子进程关闭除客户端连接外的所有文件描述符 */
        for (int fd_i = 3; fd_i < 1024; fd_i++) {
            if (fd_i != client_fd) close(fd_i);
        }
        do_upload_raw_child(client_fd, filename, content_length, body_read, body_read_len);
    }

    g_upload_pid = pid;
    XLOG_I(TAG, "原始上传开始: pid=%d, filename=%s, content_length=%d", pid, filename, content_length);
    return 1;
}

/**
 * handle_upload - 处理POST /api/files/upload请求(multipart/form-data上传)
 * @client_fd:      客户端套接字描述符
 * @content_type:   Content-Type请求头值(需包含boundary)
 * @content_length: Content-Length值
 * @body_read:      已读取的请求体数据
 * @body_read_len:  已读取的请求体数据长度
 *
 * 解析multipart/form-data格式上传文件，在子进程中执行。
 * 返回: 1表示上传在子进程中进行(连接暂不关闭)，<=0表示已完成或出错
 */
static int handle_upload(int client_fd, const char *content_type, int content_length,
                        const char *body_read, int body_read_len) {
    if (content_length <= 0) return send_error(client_fd, 400, "Missing Content-Length");

    int max_bytes = g_upload_max_mb * 1024 * 1024;
    if (content_length > max_bytes) {
        char err[64];
        snprintf(err, sizeof(err), "File too large (max %dMB)", g_upload_max_mb);
        return send_error(client_fd, 400, err);
    }

    /* 检查是否有正在进行的上传 */
    pid_t existing = g_upload_pid;
    if (existing > 0) {
        char check[64];
        snprintf(check, sizeof(check), "/proc/%d/comm", existing);
        if (access(check, F_OK) == 0) return send_error(client_fd, 503, "Upload in progress");
    }

    /* 从Content-Type中提取boundary */
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

/**
 * handle_get_diag - 处理GET /api/diag请求
 *
 * 设备环境自检（诊断）接口，检查xwebd各项功能所依赖的环境是否就绪:
 * 1. HTTP服务 — 服务是否正常运行
 * 2. 文件系统 — XWEBD_BASE_DIR是否可读写
 * 3. 系统信息 — /proc/version和/proc/meminfo是否可读
 * 4. 音量控制 — get_volume()是否可用
 * 5. 亮度控制 — get_brightness()是否可用
 * 6. Assistant状态 — sair二进制文件是否存在
 * 7. 看门狗脚本 — boot_watchdog.sh是否存在
 * 8. 磁盘空间 — /var/upgrade剩余空间是否充足
 */
static int handle_get_diag(int fd) {
    char buf[2048];
    int pos = 0;
    int ok_count = 0, fail_count = 0;
    int first = 1;

    pos += snprintf(buf + pos, sizeof(buf) - pos, "{\"items\":[");

    /* 1. HTTP服务 — 能调用此API说明服务正常运行 */
    {
        if (!first) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
        first = 0;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "{\"name\":\"HTTP服务\",\"ok\":true,\"message\":\"HTTP服务运行正常\"}");
        ok_count++;
    }

    /* 2. 文件系统 — 检查XWEBD_BASE_DIR是否可读写 */
    {
        if (!first) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
        first = 0;
        const char *test_path = XWEBD_BASE_DIR "/.diag_test";
        int fs_ok = 0;
        const char *fs_msg = "";
        char err_buf[128] = "";
        int tfd = open(test_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (tfd >= 0) {
            const char *test_data = "diag_ok";
            int wlen = strlen(test_data);
            if (write(tfd, test_data, wlen) == wlen) {
                close(tfd);
                char read_buf[16];
                int n = read_file_string(test_path, read_buf, sizeof(read_buf));
                if (n == wlen && memcmp(read_buf, test_data, wlen) == 0) {
                    fs_ok = 1;
                    fs_msg = "文件系统读写正常";
                } else {
                    snprintf(err_buf, sizeof(err_buf), "文件系统读验证失败");
                    fs_msg = err_buf;
                }
            } else {
                snprintf(err_buf, sizeof(err_buf), "文件系统写入失败: %s", strerror(errno));
                fs_msg = err_buf;
                close(tfd);
            }
            unlink(test_path);
        } else {
            snprintf(err_buf, sizeof(err_buf), "文件系统读写失败: %s", strerror(errno));
            fs_msg = err_buf;
        }
        if (fs_ok) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "{\"name\":\"文件系统\",\"ok\":true,\"message\":\"%s\"}", fs_msg);
            ok_count++;
        } else {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "{\"name\":\"文件系统\",\"ok\":false,\"message\":\"%s\"}", fs_msg);
            fail_count++;
        }
    }

    /* 3. 系统信息 — 检查/proc/version和/proc/meminfo是否可读 */
    {
        if (!first) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
        first = 0;
        char tmp[64];
        int v_ok = (read_file_string("/proc/version", tmp, sizeof(tmp)) >= 0);
        int m_ok = (read_file_string("/proc/meminfo", tmp, sizeof(tmp)) >= 0);
        if (v_ok && m_ok) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "{\"name\":\"系统信息\",\"ok\":true,\"message\":\"系统信息读取正常\"}");
            ok_count++;
        } else {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "{\"name\":\"系统信息\",\"ok\":false,\"message\":\"系统信息读取失败\"}");
            fail_count++;
        }
    }

    /* 4. 音量控制 — 检查get_volume()是否可用 */
    {
        if (!first) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
        first = 0;
        int vol = get_volume();
        if (vol >= 0) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "{\"name\":\"音量控制\",\"ok\":true,\"message\":\"音量控制接口正常（当前音量: %d）\"}", vol);
            ok_count++;
        } else {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "{\"name\":\"音量控制\",\"ok\":false,\"message\":\"音量控制接口不可用\"}");
            fail_count++;
        }
    }

    /* 5. 亮度控制 — 检查get_brightness()是否可用 */
    {
        if (!first) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
        first = 0;
        int br = get_brightness();
        if (br >= 0) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "{\"name\":\"亮度控制\",\"ok\":true,\"message\":\"亮度控制接口正常（当前亮度: %d）\"}", br);
            ok_count++;
        } else {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "{\"name\":\"亮度控制\",\"ok\":false,\"message\":\"亮度控制接口不可用\"}");
            fail_count++;
        }
    }

    /* 6. Assistant状态 — 检查sair二进制文件是否存在 */
    {
        if (!first) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
        first = 0;
        if (access(XWEBD_SAIR_BIN, F_OK) == 0) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "{\"name\":\"Assistant状态\",\"ok\":true,\"message\":\"助手程序已部署\"}");
            ok_count++;
        } else {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "{\"name\":\"Assistant状态\",\"ok\":false,\"message\":\"助手程序未部署\"}");
            fail_count++;
        }
    }

    /* 7. 看门狗脚本 — 检查boot_watchdog.sh是否存在 */
    {
        if (!first) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
        first = 0;
        if (access(XWEBD_WATCHDOG_SH, F_OK) == 0) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "{\"name\":\"看门狗脚本\",\"ok\":true,\"message\":\"看门狗脚本已部署\"}");
            ok_count++;
        } else {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "{\"name\":\"看门狗脚本\",\"ok\":false,\"message\":\"看门狗脚本未部署\"}");
            fail_count++;
        }
    }

    /* 8. 磁盘空间 — 检查/var/upgrade剩余空间 */
    {
        if (!first) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
        first = 0;
        struct statvfs vfs;
        if (statvfs(XWEBD_BASE_DIR, &vfs) == 0) {
            long free_kb = (long)((long long)vfs.f_bfree * vfs.f_bsize / 1024);
            if (free_kb >= 1024) {
                pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "{\"name\":\"磁盘空间\",\"ok\":true,\"message\":\"磁盘空间充足（%ldKB可用）\"}", free_kb);
                ok_count++;
            } else {
                pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "{\"name\":\"磁盘空间\",\"ok\":false,\"message\":\"磁盘空间不足（仅%ldKB可用）\"}", free_kb);
                fail_count++;
            }
        } else {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "{\"name\":\"磁盘空间\",\"ok\":false,\"message\":\"磁盘空间查询失败\"}");
            fail_count++;
        }
    }

    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "],\"ok_count\":%d,\"fail_count\":%d,\"total\":8,"
        "\"summary\":\"%d项正常, %d项异常\"}",
        ok_count, fail_count, ok_count, fail_count);

    return send_response(fd, 200, "application/json", buf, pos);
}

/**
 * handle_get_services - 处理GET /api/services请求
 *
 * 查询系统服务状态:
 * - telnet: 是否运行中
 * - boot_watchdog: 看门狗脚本是否已部署
 * - xwebd_autostart: 自启动脚本中是否包含xwebd
 */
static int handle_get_services(int fd) {
    char buf[512];
    int telnet_running = 0;
    {
        FILE *pf = popen("pidof telnetd 2>/dev/null", "r");
        if (pf) { char pbuf[16]; if (fgets(pbuf, sizeof(pbuf), pf)) telnet_running = 1; pclose(pf); }
    }
    int watchdog_exists = (access(XWEBD_WATCHDOG_SH, X_OK) == 0);
    int xwebd_autostart = 0;
    {
        char buf2[1024];
        int n = read_file_string(XWEBD_TEST_SH, buf2, sizeof(buf2));
        if (n > 0 && strstr(buf2, "xwebd")) xwebd_autostart = 1;
    }

    int len = snprintf(buf, sizeof(buf),
        "{\"telnet\":{\"running\":%s},"
        "\"boot_watchdog\":{\"deployed\":%s},"
        "\"xwebd_autostart\":{\"enabled\":%s}}",
        telnet_running ? "true" : "false",
        watchdog_exists ? "true" : "false",
        xwebd_autostart ? "true" : "false");
    return send_response(fd, 200, "application/json", buf, len);
}

/**
 * handle_post_assistant_deploy - 处理POST /api/assistant/deploy请求
 * @fd:   客户端套接字描述符
 * @body: 请求体JSON，可选"path"字段指定sair_new文件路径
 *
 * 部署助手程序(sair)，支持两种模式:
 * 1. 热更新: sair正在运行时，替换二进制文件后发送SIGUSR2信号通知其重新加载
 * 2. 冷部署: sair未运行时，直接放置二进制文件并设置可执行权限
 *
 * 部署流程:
 * 1. 查找sair_new文件(默认XWEBD_BASE_DIR/sair_new，可通过path参数指定)
 * 2. 备份当前sair为sair_backup
 * 3. 将sair_new重命名为sair
 * 4. 若sair正在运行则发送SIGUSR2信号热更新，否则仅放置文件
 */
static int handle_post_assistant_deploy(int fd, const char *body) {
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

    /* 检查sair(助手程序)是否正在运行 */
    int sair_running = 0;
    pid_t sair_pid = 0;
    {
        char pbuf[16];
        FILE *pf = popen("pidof sair 2>/dev/null", "r");
        if (pf) {
            if (fgets(pbuf, sizeof(pbuf), pf)) { sair_running = 1; sair_pid = atoi(pbuf); }
            pclose(pf);
        }
    }

    /* 热更新: sair正在运行 */
    if (sair_running && sair_pid > 0) {
        if (access(XWEBD_SAIR_BIN, X_OK) == 0) {
            if (rename(XWEBD_SAIR_BIN, XWEBD_SAIR_BACKUP) != 0)
                XLOG_W(TAG, "备份助手程序失败: %s", strerror(errno));
        }
        if (rename(sair_new_path, XWEBD_SAIR_BIN) != 0)
            return send_error(fd, 500, "Failed to rename sair_new to sair");
        XLOG_I(TAG, "助手部署: 发送SIGUSR2信号到pid %d进行热更新", sair_pid);
        kill(sair_pid, SIGUSR2);
        return send_json(fd, 200, "{\"ok\":true,\"method\":\"hot_update\",\"pid\":0}");
    }

    /* 冷部署: sair未运行 */
    if (access(XWEBD_SAIR_BIN, X_OK) == 0) {
        if (rename(XWEBD_SAIR_BIN, XWEBD_SAIR_BACKUP) != 0)
            XLOG_W(TAG, "备份助手程序失败: %s", strerror(errno));
    }
    if (rename(sair_new_path, XWEBD_SAIR_BIN) != 0)
        return send_error(fd, 500, "Failed to rename sair_new to sair");

    chmod(XWEBD_SAIR_BIN, 0755);

    XLOG_I(TAG, "助手部署: sair未运行, 二进制文件已放置到 %s", XWEBD_SAIR_BIN);
    return send_json(fd, 200, "{\"ok\":true,\"method\":\"cold_deploy\"}");
}

/**
 * handle_post_assistant_update - 处理POST /api/assistant/update请求
 * @fd:   客户端套接字描述符
 * @body: 请求体JSON
 *
 * 助手程序更新接口，实际逻辑与部署接口相同
 */
static int handle_post_assistant_update(int fd, const char *body) {
    return handle_post_assistant_deploy(fd, body);
}

/**
 * handle_post_assistant_uninstall - 处理POST /api/assistant/uninstall请求
 *
 * 卸载助手程序(sair):
 * 1. 若sair正在运行，先尝试正常终止，再强制终止
 * 2. 删除sair二进制文件
 * 3. 删除sair_backup备份文件
 */
static int handle_post_assistant_uninstall(int fd) {
    int sair_running = 0;
    {
        char pbuf[16];
        FILE *pf = popen("pidof sair 2>/dev/null", "r");
        if (pf) {
            if (fgets(pbuf, sizeof(pbuf), pf)) sair_running = 1;
            pclose(pf);
        }
    }

    /* 先尝试正常终止sair进程，失败则强制终止 */
    if (sair_running) {
        XLOG_I(TAG, "助手卸载: 正在停止sair...");
        system("killall sair 2>/dev/null");
        usleep(500000);
        system("killall -9 sair 2>/dev/null");
        usleep(500000);
    }

    /* 删除sair二进制文件 */
    if (access(XWEBD_SAIR_BIN, F_OK) == 0) {
        if (unlink(XWEBD_SAIR_BIN) != 0)
            return send_error(fd, 500, "Failed to remove sair");
        XLOG_I(TAG, "助手卸载: 已删除 %s", XWEBD_SAIR_BIN);
    }

    /* 删除sair备份文件 */
    if (access(XWEBD_SAIR_BACKUP, F_OK) == 0) {
        unlink(XWEBD_SAIR_BACKUP);
        XLOG_I(TAG, "助手卸载: 已删除 %s", XWEBD_SAIR_BACKUP);
    }

    return send_json(fd, 200, "{\"ok\":true}");
}

/**
 * handle_get_assistant_status - 处理GET /api/assistant/status请求
 *
 * 查询助手程序(sair)状态:
 * - installed: 是否已安装(sair二进制文件是否存在且可执行)
 * - running: 是否正在运行(通过pidof检测)
 * - pid: 运行时的进程ID
 * - native_backup_exists: 备份文件是否存在
 * - version: 运行时通过API获取的版本号
 */
static int handle_get_assistant_status(int fd) {
    int installed = (access(XWEBD_SAIR_BIN, X_OK) == 0);
    int running = 0;
    int pid = 0;
    int backup_exists = (access(XWEBD_SAIR_BACKUP, F_OK) == 0);
    char version[32] = "";

    if (installed) {
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "pidof sair 2>/dev/null");
        FILE *pf = popen(cmd, "r");
        if (pf) {
            char pbuf[16];
            if (fgets(pbuf, sizeof(pbuf), pf)) { running = 1; pid = atoi(pbuf); }
            pclose(pf);
        }

        /* 通过sair内部API获取版本号 */
        if (running) {
            char api_resp[256] = "";
            snprintf(cmd, sizeof(cmd),
                "wget -q -O - -T 2 http://127.0.0.1:8081/api/status 2>/dev/null");
            FILE *wf = popen(cmd, "r");
            if (wf) {
                if (fgets(api_resp, sizeof(api_resp), wf)) {
                    char vbuf[32] = "";
                    if (parse_json_str(api_resp, "version", vbuf, sizeof(vbuf)) == 0)
                        strncpy(version, vbuf, sizeof(version) - 1);
                }
                pclose(wf);
            }
        }
    }

    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"installed\":%s,\"running\":%s,\"pid\":%d,\"native_backup_exists\":%s,\"api_port\":8081,\"version\":\"%s\"}",
        installed ? "true" : "false", running ? "true" : "false", pid,
        backup_exists ? "true" : "false", version);
    return send_json(fd, 200, buf);
}

/**
 * handle_get_assistant_logs - 处理GET /api/assistant/logs请求
 * @fd:    客户端套接字描述符
 * @query: URL查询字符串，支持"lines"参数(默认50, 最大500)
 *
 * 从sair日志文件尾部读取指定行数的日志
 */
static int handle_get_assistant_logs(int fd, const char *query) {
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

    /* 从文件末尾向前搜索换行符，定位到指定行数的起始位置 */
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

    /* 读取定位位置到文件末尾的内容 */
    int total = 0;
    while (total < (int)sizeof(buf) - 1) {
        ssize_t n = read(fd_log, buf + total, (int)sizeof(buf) - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    close(fd_log);
    buf[total] = '\0';

    /* 将日志内容转义为JSON字符串 */
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

/**
 * handle_post_assistant_logs_clear - 处理POST /api/assistant/logs/clear请求
 *
 * 清空sair日志文件内容(截断为0字节)
 */
static int handle_post_assistant_logs_clear(int fd) {
    if (truncate(XWEBD_SAIR_LOG, 0) == 0)
        return send_json(fd, 200, "{\"ok\":true}");
    return send_error(fd, 500, "Failed to clear log file");
}

/**
 * handle_request - 处理单个HTTP请求
 * @client_fd: 客户端套接字描述符
 *
 * 完整的HTTP请求处理流程:
 * 1. 读取请求头(直到\r\n\r\n)
 * 2. 解析HTTP方法、路径、查询参数
 * 3. 读取请求体(根据Content-Length)
 * 4. 回收已结束的上传子进程
 * 5. 根据方法和路径分发到对应的处理函数
 *
 * 返回: 1表示上传在子进程中进行(连接暂不关闭)，0表示正常完成，-1表示错误
 */
static int handle_request(int client_fd) {
    char req_buf[XWEBD_REQ_BUF_SIZE];
    int total = 0;
    time_t start_time = time(NULL);

    /* 读取HTTP请求头 */
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

    /* 解析HTTP方法、路径和查询参数 */
    char method[16] = {0};
    char path[512] = {0};
    sscanf(req_buf, "%15s %511s", method, path);

    char *query = strchr(path, '?');
    if (query) *query++ = '\0';

    /* 定位请求体起始位置 */
    char *body_start = strstr(req_buf, "\r\n\r\n");
    int header_len = 0;
    if (body_start) {
        header_len = (int)(body_start - req_buf) + 4;
        body_start += 4;
    }

    /* 解析Content-Length */
    int content_length = 0;
    {
        char *cl = strcasestr(req_buf, "Content-Length:");
        if (cl) content_length = atoi(cl + 15);
    }

    /* 继续读取请求体数据 */
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

    /* 解析Content-Type */
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

    /* 回收已结束的上传子进程 */
    {
        pid_t wp;
        while ((wp = waitpid(-1, NULL, WNOHANG)) > 0) {
            if (wp == g_upload_pid) g_upload_pid = 0;
        }
    }

    /* GET请求路由 */
    if (strcmp(method, "GET") == 0) {
        if (strcmp(path, "/api/ping") == 0) return handle_get_ping(client_fd);
        if (strcmp(path, "/api/version") == 0) return handle_get_version(client_fd);
        if (strcmp(path, "/api/config") == 0) return handle_get_config(client_fd);
        if (strcmp(path, "/api/system") == 0) return handle_get_system(client_fd);
        if (strcmp(path, "/api/logs") == 0) return handle_get_logs(client_fd, query);
        if (strcmp(path, "/api/volume") == 0) return handle_get_volume(client_fd);
        if (strcmp(path, "/api/brightness") == 0) return handle_get_brightness(client_fd);
        if (strcmp(path, "/api/mute") == 0) {
            int m = get_mute();
            char buf[32];
            snprintf(buf, sizeof(buf), "{\"muted\":%s}", m < 0 ? "null" : (m ? "true" : "false"));
            return send_json(client_fd, 200, buf);
        }
        if (strcmp(path, "/api/services") == 0) return handle_get_services(client_fd);
        if (strcmp(path, "/api/diag") == 0) return handle_get_diag(client_fd);
        if (strcmp(path, "/api/assistant/status") == 0) return handle_get_assistant_status(client_fd);
        if (strcmp(path, "/api/assistant/logs") == 0) return handle_get_assistant_logs(client_fd, query);
        if (strcmp(path, "/api/files") == 0) return handle_get_files(client_fd, query);
        if (strcmp(path, "/api/files/download") == 0) return handle_download_file(client_fd, query);
    } else if (strcmp(method, "POST") == 0) {
        /* POST请求路由 */
        if (strcmp(path, "/api/volume") == 0) return handle_post_volume(client_fd, body);
        if (strcmp(path, "/api/brightness") == 0) return handle_post_brightness(client_fd, body);
        if (strcmp(path, "/api/mute") == 0) {
            int muted = 0;
            if (body) parse_json_int(body, "muted", &muted);
            int ret = set_mute(muted);
            if (ret != 0) return send_error(client_fd, 500, "Failed to set mute");
            char buf[32];
            snprintf(buf, sizeof(buf), "{\"muted\":%s}", muted ? "true" : "false");
            return send_json(client_fd, 200, buf);
        }
        if (strcmp(path, "/api/poweroff") == 0) return handle_post_poweroff(client_fd);
        if (strcmp(path, "/api/reboot") == 0) return handle_post_reboot(client_fd);
        if (strcmp(path, "/api/files/upload") == 0) return handle_upload(client_fd, content_type, content_length, body, body_received);
        if (strcmp(path, "/api/upload") == 0) return handle_upload_raw(client_fd, content_type, content_length, body, body_received, req_buf);
        if (strcmp(path, "/api/files/batch-delete") == 0) return handle_batch_delete(client_fd, body);
        if (strcmp(path, "/api/files/cleanup") == 0) return handle_cleanup(client_fd);
        if (strcmp(path, "/api/assistant/deploy") == 0) return handle_post_assistant_deploy(client_fd, body);
        if (strcmp(path, "/api/assistant/update") == 0) return handle_post_assistant_update(client_fd, body);
        if (strcmp(path, "/api/assistant/uninstall") == 0) return handle_post_assistant_uninstall(client_fd);
        if (strcmp(path, "/api/assistant/logs/clear") == 0) return handle_post_assistant_logs_clear(client_fd);
    } else if (strcmp(method, "PUT") == 0) {
        /* PUT请求路由 */
        if (strcmp(path, "/api/config") == 0) return handle_put_config(client_fd, body);
    } else if (strcmp(method, "DELETE") == 0) {
        /* DELETE请求路由 */
        if (strcmp(path, "/api/files") == 0 || strcmp(path, "/api/files/download") == 0)
            return handle_delete_file(client_fd, query);
    } else if (strcmp(method, "OPTIONS") == 0) {
        /* CORS预检请求 */
        return send_json(client_fd, 200, "{}");
    }

    return send_error(client_fd, 404, "Not found");
}

/**
 * cleanup_startup_residuals - 清理启动残留文件
 *
 * 删除test.sh.new临时文件（自启动脚本更新过程中的残留）
 */
static void cleanup_startup_residuals(void) {
    if (access(XWEBD_TEST_SH_NEW, F_OK) == 0) {
        XLOG_I(TAG, "清理残留文件 %s", XWEBD_TEST_SH_NEW);
        unlink(XWEBD_TEST_SH_NEW);
    }
}

/**
 * signal_handler - 信号处理函数
 * @sig: 接收到的信号编号
 *
 * 收到SIGTERM信号时设置g_running=0，使工作循环退出
 */
static void signal_handler(int sig) {
    if (sig == SIGTERM) {
        g_running = 0;
    }
}

/**
 * worker_loop - 工作进程主循环
 *
 * 工作进程的完整生命周期:
 * 1. 初始化日志系统
 * 2. 创建TCP服务器套接字并绑定端口
 * 3. 循环接受客户端连接并处理请求
 * 4. 收到SIGTERM时优雅退出，等待上传子进程结束
 *
 * 退出码: 0=正常退出, 2=不可恢复错误(端口占用等)
 */
static void worker_loop(void) {
    prctl(PR_SET_PDEATHSIG, SIGTERM);

    xlog_init(NULL);
    XLOG_I(TAG, "xwebd工作进程已启动 (v%s, 端口 %d)", XWEBD_VERSION, g_port);

    cleanup_startup_residuals();

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
        /* 回收已结束的子进程 */
        while (waitpid(-1, NULL, WNOHANG) > 0) {}

        struct pollfd pfd = {g_server_fd, POLLIN, 0};
        int pret = poll(&pfd, 1, 1000);
        if (pret <= 0) continue;

        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(g_server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) continue;

        /* 设置客户端连接超时 */
        struct timeval tv = {XWEBD_REQUEST_TIMEOUT, 0};
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        int ret = handle_request(client_fd);
        if (ret != 1) close(client_fd);
    }

    /* 优雅退出: 等待上传子进程结束 */
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
    xlog_close();
    XLOG_I(TAG, "xwebd工作进程正常退出");
}

/**
 * print_usage - 打印命令行用法
 * @prog: 程序名称
 */
static void print_usage(const char *prog) {
    fprintf(stderr, "用法: %s [-p 端口] [-d]\n", prog);
    fprintf(stderr, "  -p 端口   监听端口 (默认 %d)\n", XWEBD_DEFAULT_PORT);
    fprintf(stderr, "  -d        以守护进程模式运行\n");
}

/**
 * main - 程序入口
 *
 * 主进程作为看门狗运行:
 * 1. 解析命令行参数(-p端口, -d守护进程)
 * 2. 若指定-d则以守护进程模式运行
 * 3. 设置信号处理(SIGTERM, SIGINT, SIGPIPE)
 * 4. fork出工作进程运行HTTP服务器
 * 5. 监控工作进程:
 *    - 正常退出(退出码0): 看门狗也退出
 *    - 不可恢复错误(退出码2): 不重启，看门狗退出
 *    - 崩溃/被信号终止: 自动重启工作进程
 *    - 在时间窗口内崩溃次数超过限制: 放弃重启
 */
int main(int argc, char *argv[]) {
    int opt;
    while ((opt = getopt(argc, argv, "p:d")) != -1) {
        switch (opt) {
            case 'p': g_port = atoi(optarg); break;
            case 'd': g_daemon = 1; break;
            default: print_usage(argv[0]); return 1;
        }
    }

    /* 守护进程模式: fork后父进程退出，子进程脱离终端 */
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

    /* 设置信号处理 */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* fork工作进程 */
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

    /* 看门狗主循环: 监控工作进程状态 */
    while (1) {
        int status;
        pid_t ret = waitpid(worker_pid, &status, 0);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (WIFEXITED(status)) {
            int exit_code = WEXITSTATUS(status);
            if (exit_code == 0) {
                /* 工作进程正常退出 */
                XLOG_I(TAG, "工作进程正常退出, 看门狗退出");
                break;
            } else if (exit_code == 2) {
                /* 不可恢复错误(如端口占用)，不再重启 */
                XLOG_E(TAG, "工作进程遇到不可恢复错误退出 (代码 %d), 不再重启", exit_code);
                break;
            } else {
                /* 其他退出码，视为崩溃 */
                XLOG_W(TAG, "工作进程崩溃 (代码 %d)", exit_code);
            }
        } else if (WIFSIGNALED(status)) {
            /* 被信号终止 */
            XLOG_W(TAG, "工作进程被信号 %d 终止", WTERMSIG(status));
        } else {
            break;
        }

        /* 崩溃频率统计: 在时间窗口内崩溃次数超过限制则放弃 */
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

        XLOG_I(TAG, "2秒后重启工作进程...");
        sleep(2);

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
    }

    xlog_close();
    return 0;
}
