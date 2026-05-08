/**
 * @file api_server.c
 * @brief HTTP API服务器实现
 *
 * 实现本地HTTP API服务器，提供设备控制和管理接口，包括：
 * - 状态查询（GET /api/status）
 * - 配置查询和修改（GET/PUT /api/config）
 * - 唤醒触发（POST /api/wakeup）
 * - 中止操作（POST /api/abort）
 * - 热更新触发（POST /api/upgrade）
 * - CORS跨域支持
 * - 信号屏蔽（避免API线程被SIGUSR1/SIGUSR2中断）
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
#include <signal.h>
#include <pthread.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <ctype.h>
#include <sys/prctl.h>

#define API_PORT 8081                /* API服务器监听端口 */
#define API_REQ_BUF_SIZE 4096        /* 请求缓冲区大小 */
#define API_RESP_BUF_SIZE 16384      /* 响应缓冲区大小 */
#define API_REQUEST_TIMEOUT_SEC 5    /* 请求超时时间（秒） */

#define TAG "API"

extern app_context_t g_app;

static volatile int g_api_running = 0;   /* 服务器运行标志 */
static int g_server_fd = -1;             /* 服务器监听套接字 */
static volatile int g_upgrading = 0;     /* 热更新进行中标志 */

/**
 * @brief 将状态枚举转换为字符串
 * @param state 状态枚举值
 * @return 状态字符串
 */
static const char* state_to_string(xiaozhi_state_t state) {
    switch (state) {
        case kStateStarting:    return "Starting";
        case kStateActivating:  return "Activating";
        case kStateIdle:        return "Idle";
        case kStateConnecting:  return "Connecting";
        case kStateListening:   return "Listening";
        case kStateSpeaking:    return "Speaking";
        case kStateCleaning:    return "Cleaning";
        default:                return "Unknown";
    }
}

/**
 * @brief 发送HTTP响应
 * @param fd 客户端套接字
 * @param status HTTP状态码
 * @param content_type 内容类型
 * @param body 响应体
 * @param body_len 响应体长度
 * @return 0成功，-1失败
 */
static int send_response(int fd, int status, const char* content_type, const char* body, int body_len) {
    const char* status_text;
    switch (status) {
        case 200: status_text = "OK"; break;
        case 400: status_text = "Bad Request"; break;
        case 404: status_text = "Not Found"; break;
        case 500: status_text = "Internal Server Error"; break;
        case 503: status_text = "Service Unavailable"; break;
        default: status_text = "Unknown"; break;
    }
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n",
        status, status_text, content_type, body_len);
    if (send(fd, header, hlen, MSG_NOSIGNAL) < 0) return -1;
    if (body_len > 0 && send(fd, body, body_len, MSG_NOSIGNAL) < 0) return -1;
    return 0;
}

/**
 * @brief 发送JSON响应
 * @param fd 客户端套接字
 * @param status HTTP状态码
 * @param json JSON字符串
 * @return 0成功，-1失败
 */
static int send_json(int fd, int status, const char* json) {
    return send_response(fd, status, "application/json", json, strlen(json));
}

/**
 * @brief 发送错误响应
 * @param fd 客户端套接字
 * @param code HTTP状态码
 * @param message 错误消息
 * @return 0成功，-1失败
 */
static int send_error(int fd, int code, const char* message) {
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", message);
    return send_json(fd, code, buf);
}

/**
 * @brief 从JSON字符串中解析指定键的字符串值
 * @param json JSON字符串
 * @param key 键名
 * @param out 输出缓冲区
 * @param out_size 输出缓冲区大小
 * @return 0成功，-1未找到
 */
static int parse_json_str(const char* json, const char* key, char* out, int out_size) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char* p = strstr(json, search);
    if (!p) return -1;
    p += strlen(search);
    while (*p && (*p == ' ' || *p == ':' || *p == '\t')) p++;
    if (*p != '"') return -1;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < out_size - 1) {
        if (*p == '\\' && *(p + 1)) { p++; out[i++] = *p++; }
        else { out[i++] = *p++; }
    }
    out[i] = '\0';
    return 0;
}

/**
 * @brief 从JSON字符串中解析指定键的布尔值
 * @param json JSON字符串
 * @param key 键名
 * @param out 输出整数值（1=true, 0=false）
 * @return 0成功，-1未找到
 */
static int parse_json_bool(const char* json, const char* key, int* out) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char* p = strstr(json, search);
    if (!p) return -1;
    p += strlen(search);
    while (*p && (*p == ' ' || *p == ':' || *p == '\t')) p++;
    if (strncmp(p, "true", 4) == 0) *out = 1;
    else if (strncmp(p, "false", 5) == 0) *out = 0;
    else *out = atoi(p);
    return 0;
}

/**
 * @brief 处理GET /api/status请求，返回设备当前状态
 * @param fd 客户端套接字
 * @return 0成功，-1失败
 */
static int handle_get_status(int fd) {
    xiaozhi_state_t state = state_machine_get_state(&g_app.sm);
    char buf[512];
    int len = snprintf(buf, sizeof(buf),
        "{\"state\":\"%s\",\"version\":\"%s\",\"realtime_mode\":%s,\"aec_enabled\":true}",
        state_to_string(state),
        XIAOZHI_VERSION,
        g_app.realtime_mode ? "true" : "false");
    return send_response(fd, 200, "application/json", buf, len);
}

/**
 * @brief 处理GET /api/config请求，返回当前配置
 * @param fd 客户端套接字
 * @return 0成功，-1失败
 */
static int handle_get_config(int fd) {
    int plog_lvl = plog_get_level();
    char buf[API_RESP_BUF_SIZE];
    int len = snprintf(buf, sizeof(buf),
        "{\"ws_url\":\"%s\",\"ws_token\":\"***\",\"realtime_mode\":%s,\"aec_enabled\":true,\"log_level\":\"%s\"}",
        g_app.config.ws_url,
        g_app.realtime_mode ? "true" : "false",
        plog_lvl == PLOG_LEVEL_DEBUG ? "DEBUG" :
        plog_lvl == PLOG_LEVEL_INFO ? "INFO" :
        plog_lvl == PLOG_LEVEL_WARN ? "WARN" : "ERROR"
    );
    return send_response(fd, 200, "application/json", buf, len);
}

/**
 * @brief 处理PUT /api/config请求，修改配置
 * @param fd 客户端套接字
 * @param body 请求体JSON
 * @return 0成功，-1失败
 *
 * 支持修改：ws_url、realtime_mode、log_level
 * 修改ws_url和realtime_mode后通过SIGUSR1通知主线程生效
 */
static int handle_put_config(int fd, const char* body) {
    if (!body || !*body) return send_error(fd, 400, "Empty request body");

    char ws_url[512] = {0};
    int realtime_mode = -1;
    char log_level[16] = {0};
    int has_changes = 0;

    /* 解析并验证ws_url */
    if (parse_json_str(body, "ws_url", ws_url, sizeof(ws_url)) == 0) {
        if (ws_url[0] != '\0' && strncmp(ws_url, "wss://", 6) != 0 && strncmp(ws_url, "ws://", 5) != 0)
            return send_error(fd, 400, "Invalid ws_url format");
        if (ws_url[0] != '\0' && strlen(ws_url) < 10)
            return send_error(fd, 400, "ws_url too short");
        has_changes = 1;
    }
    if (parse_json_bool(body, "realtime_mode", &realtime_mode) == 0) has_changes = 1;
    /* 解析并验证log_level */
    if (parse_json_str(body, "log_level", log_level, sizeof(log_level)) == 0) {
        if (strcmp(log_level, "DEBUG") != 0 && strcmp(log_level, "INFO") != 0 &&
            strcmp(log_level, "WARN") != 0 && strcmp(log_level, "ERROR") != 0)
            return send_error(fd, 400, "Invalid log_level");
        has_changes = 1;
    }

    if (!has_changes) return send_error(fd, 400, "No valid config fields provided");

    /* 将配置变更写入待处理缓冲区，通过信号通知主线程 */
    if (ws_url[0] || realtime_mode >= 0) {
        memset(g_app.pending_config_buf, 0, sizeof(g_app.pending_config_buf));
        int off = 0;
        if (ws_url[0]) off += snprintf(g_app.pending_config_buf + off, sizeof(g_app.pending_config_buf) - off, "ws_url=%s;", ws_url);
        if (realtime_mode >= 0) off += snprintf(g_app.pending_config_buf + off, sizeof(g_app.pending_config_buf) - off, "realtime_mode=%d;", realtime_mode);
        __sync_synchronize();
        g_app.pending_api_config = 1;
        kill(getpid(), SIGUSR1);
    }

    /* 立即生效日志级别 */
    if (log_level[0]) {
        if (strcmp(log_level, "DEBUG") == 0) plog_set_level(PLOG_LEVEL_DEBUG);
        else if (strcmp(log_level, "INFO") == 0) plog_set_level(PLOG_LEVEL_INFO);
        else if (strcmp(log_level, "WARN") == 0) plog_set_level(PLOG_LEVEL_WARN);
        else if (strcmp(log_level, "ERROR") == 0) plog_set_level(PLOG_LEVEL_ERROR);
    }

    return send_json(fd, 200, "{\"ok\":true}");
}

/**
 * @brief 处理POST /api/wakeup请求，触发设备唤醒
 * @param fd 客户端套接字
 * @return 0成功，-1失败
 * @note 仅在Idle状态下允许唤醒
 */
static int handle_post_wakeup(int fd) {
    xiaozhi_state_t state = state_machine_get_state(&g_app.sm);
    if (state != kStateIdle) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Cannot wakeup in state %s", state_to_string(state));
        return send_error(fd, 503, msg);
    }
    __sync_synchronize();
    g_app.pending_api_wakeup = 1;
    kill(getpid(), SIGUSR1);
    return send_json(fd, 200, "{\"ok\":true}");
}

/**
 * @brief 处理POST /api/abort请求，中止当前操作
 * @param fd 客户端套接字
 * @return 0成功，-1失败
 */
static int handle_post_abort(int fd) {
    __sync_synchronize();
    g_app.pending_api_abort = 1;
    kill(getpid(), SIGUSR1);
    return send_json(fd, 200, "{\"ok\":true}");
}

/**
 * @brief 处理GET /api/diag请求，执行自检并返回诊断结果
 * @param fd 客户端套接字
 * @return 0成功，-1失败
 */
static int handle_get_diag(int fd) {
    diag_result_t result = diag_run_all();
    char buf[2048];
    int len = diag_result_to_json(&result, buf, sizeof(buf));
    return send_response(fd, 200, "application/json", buf, len);
}

/**
 * @brief 处理POST /api/upgrade请求，触发热更新
 * @param fd 客户端套接字
 * @return 0成功，-1失败
 *
 * 2秒后发送SIGUSR2信号触发主程序热更新
 */
static int handle_post_upgrade(int fd) {
    if (g_upgrading) return send_json(fd, 200, "{\"ok\":true,\"status\":\"already_upgrading\"}");

    g_upgrading = 1;
    send_json(fd, 200, "{\"ok\":true,\"status\":\"upgrading\"}");

    PLOG_I(TAG, "热更新: 2秒后发送SIGUSR2信号");
    sleep(2);
    kill(getpid(), SIGUSR2);
    return 0;
}

/**
 * @brief 处理一个HTTP请求
 * @param client_fd 客户端套接字
 * @return 0成功，-1失败
 *
 * 读取请求、解析方法和路径、路由到对应的处理函数
 */
static int handle_request(int client_fd) {
    char req_buf[API_REQ_BUF_SIZE];
    int total = 0;
    time_t start_time = time(NULL);

    /* 读取完整的HTTP请求头 */
    while (total < API_REQ_BUF_SIZE - 1) {
        if (time(NULL) - start_time >= API_REQUEST_TIMEOUT_SEC) return -1;
        struct pollfd pfd = {client_fd, POLLIN, 0};
        int pret = poll(&pfd, 1, 1000);
        if (pret < 0) return -1;
        if (pret == 0) continue;
        int n = recv(client_fd, req_buf + total, API_REQ_BUF_SIZE - 1 - total, 0);
        if (n <= 0) return -1;
        total += n;
        req_buf[total] = '\0';
        if (strstr(req_buf, "\r\n\r\n")) break;
    }

    if (total == 0) return -1;

    /* 解析HTTP方法和路径 */
    char method[16] = {0};
    char path[256] = {0};
    sscanf(req_buf, "%15s %255s", method, path);
    char* query = strchr(path, '?');
    if (query) *query++ = '\0';
    const char* body = strstr(req_buf, "\r\n\r\n");
    if (body) body += 4;

    PLOG_D(TAG, "%s %s", method, path);

    /* 路由请求到对应的处理函数 */
    if (strcmp(method, "GET") == 0) {
        if (strcmp(path, "/api/status") == 0) return handle_get_status(client_fd);
        if (strcmp(path, "/api/config") == 0) return handle_get_config(client_fd);
        if (strcmp(path, "/api/diag") == 0) return handle_get_diag(client_fd);
    } else if (strcmp(method, "POST") == 0) {
        if (strcmp(path, "/api/wakeup") == 0) return handle_post_wakeup(client_fd);
        if (strcmp(path, "/api/abort") == 0) return handle_post_abort(client_fd);
        if (strcmp(path, "/api/upgrade") == 0) return handle_post_upgrade(client_fd);
    } else if (strcmp(method, "PUT") == 0) {
        if (strcmp(path, "/api/config") == 0) return handle_put_config(client_fd, body);
    } else if (strcmp(method, "OPTIONS") == 0) {
        /* CORS预检请求 */
        return send_json(client_fd, 200, "{}");
    }

    return send_error(client_fd, 404, "Not found");
}

/**
 * @brief API服务器线程函数
 * @param arg 线程参数（未使用）
 * @return 线程返回值
 *
 * 循环接受客户端连接并处理请求，
 * 屏蔽SIGUSR1/SIGUSR2/SIGTERM信号避免被中断
 */
static void* api_server_thread(void* arg) {
    (void)arg;

    /* 屏蔽信号，避免API线程被用户信号中断 */
    sigset_t block_set;
    sigemptyset(&block_set);
    sigaddset(&block_set, SIGUSR1);
    sigaddset(&block_set, SIGUSR2);
    sigaddset(&block_set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &block_set, NULL);

    prctl(PR_SET_NAME, "api_server");

    PLOG_I(TAG, "API服务器线程已启动，监听端口 %d", API_PORT);

    while (g_api_running) {
        struct pollfd pfd = {g_server_fd, POLLIN, 0};
        int pret = poll(&pfd, 1, 1000);
        if (pret <= 0) continue;

        /* 接受客户端连接 */
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(g_server_fd, (struct sockaddr*)&client_addr, &addr_len);
        if (client_fd < 0) continue;

        /* 设置收发超时 */
        struct timeval tv = {API_REQUEST_TIMEOUT_SEC, 0};
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        handle_request(client_fd);
        close(client_fd);
    }

    PLOG_I(TAG, "API服务器线程已退出");
    return NULL;
}

/**
 * @brief 启动API服务器
 * @return 0成功，-1失败
 *
 * 创建TCP套接字、绑定端口、监听并启动服务线程
 */
int api_server_start(void) {
    if (g_api_running) return 0;

    /* 创建TCP套接字 */
    g_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server_fd < 0) {
        PLOG_E(TAG, "创建服务器套接字失败: %s", strerror(errno));
        return -1;
    }

    /* 设置端口复用 */
    int opt = 1;
    setsockopt(g_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* 绑定地址和端口 */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(API_PORT);

    if (bind(g_server_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        PLOG_E(TAG, "绑定端口 %d 失败: %s", API_PORT, strerror(errno));
        close(g_server_fd);
        g_server_fd = -1;
        return -1;
    }

    /* 开始监听 */
    if (listen(g_server_fd, 4) != 0) {
        PLOG_E(TAG, "监听失败: %s", strerror(errno));
        close(g_server_fd);
        g_server_fd = -1;
        return -1;
    }

    g_api_running = 1;

    /* 创建API服务器线程 */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 32768);
    int ret = pthread_create(&g_app.api_server_tid, &attr, api_server_thread, NULL);
    pthread_attr_destroy(&attr);

    if (ret != 0) {
        PLOG_E(TAG, "创建API服务器线程失败: %s", strerror(ret));
        g_api_running = 0;
        close(g_server_fd);
        g_server_fd = -1;
        return -1;
    }

    PLOG_I(TAG, "API服务器已启动，端口 %d", API_PORT);
    return 0;
}

/**
 * @brief 停止API服务器
 */
void api_server_stop(void) {
    g_api_running = 0;
    if (g_server_fd >= 0) {
        close(g_server_fd);
        g_server_fd = -1;
    }
}
