/**
 * @file xwplug_common.h
 * @brief xwebd 插件公共骨架（《09-插件化架构设计方案》B1 沉淀）
 *
 * 单头文件三件套: unix sock 监听建立 / 请求解析 / HTTP 响应。
 * 插件只需实现 plug_handle() 并用 XWPLUG_MAIN(<name>) 生成 main。
 *
 * 协议(与 plugin_gateway.c 对应):
 *   入: "<M> /<sub> HTTP/1.0\r\nContent-Length: N\r\n\r\n<json>"
 *   出: "HTTP/1.0 <code> <msg>\r\nContent-Length: M\r\n\r\n<json>"
 *
 * 防卡死: 单请求 10s 超时自杀(SIGKILL 由 xwebd 兜底重启),
 * 避免占着 sock 不响应拖垮面板请求。
 */

#ifndef XWPLUG_COMMON_H
#define XWPLUG_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/prctl.h>
#include <poll.h>

#define XWPLUG_REQ_MAX   8192
#define XWPLUG_RESP_MAX  16384
#define XWPLUG_REQ_TIMEOUT_SEC 10

/* 追加式 snprintf(超长截断不越界) */
#define APPEND_PRINTF(buf, pos, size, ...) do { \
    if ((pos) < (size) - 1) { \
        (pos) += snprintf((buf) + (pos), (size) - (pos), __VA_ARGS__); \
        if ((pos) >= (size)) (pos) = (size) - 1; \
    } \
} while (0)

typedef struct {
    char method[8];
    char path[256];     /* 不含 query 的子路径, 如 "/list" */
    char query[256];    /* 原样 query 串(不含'?'), 空为"" */
    char body[XWPLUG_REQ_MAX];
    int  body_len;
} xwplug_req_t;

/* ---- 插件需实现的唯一入口: 返回 HTTP 状态码, resp 填 JSON 体 ---- */
extern int plug_handle(const xwplug_req_t *req, char *resp, int resp_size);

/* ---- 工具: 从请求 body/query 中取 JSON 字符串/整数值 ---- */
__attribute__((unused)) static int xwplug_json_str(const char *hay, const char *key, char *out, int out_size)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(hay, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p != '"') return -1;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < out_size - 1) out[i++] = *p++;
    out[i] = '\0';
    return 0;
}

__attribute__((unused)) static int xwplug_json_int(const char *hay, const char *key, int def)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(hay, pat);
    if (!p) return def;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    return atoi(p);
}

/* ---- main 生成器 ---- */

#define XWPLUG_MAIN(name)                                                        \
    static void plug_req_timeout(int sig)                                         \
    {                                                                             \
        (void)sig;                                                                \
        _exit(2); /* 单请求超时自杀, xwebd 会按退避重启 */                         \
    }                                                                             \
    int main(void)                                                                \
    {                                                                             \
        prctl(PR_SET_NAME, "xwplug-" name);                                       \
        signal(SIGPIPE, SIG_IGN);                                                 \
        signal(SIGALRM, plug_req_timeout);                                        \
                                                                                  \
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);                                 \
        if (fd < 0) return 1;                                                     \
        struct sockaddr_un addr;                                                  \
        memset(&addr, 0, sizeof(addr));                                           \
        addr.sun_family = AF_UNIX;                                                \
        snprintf(addr.sun_path, sizeof(addr.sun_path), "/tmp/xwplug/%s.sock", name);\
        unlink(addr.sun_path);                                                    \
        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) return 1;      \
        if (listen(fd, 2) != 0) return 1;                                         \
                                                                                  \
        while (1) {                                                               \
            int cfd = accept(fd, NULL, NULL);                                     \
            if (cfd < 0) continue;                                                \
                                                                                  \
            alarm(XWPLUG_REQ_TIMEOUT_SEC); /* 请求处理限时 */                       \
            char buf[XWPLUG_REQ_MAX + 64];                                        \
            int total = 0;                                                        \
            while (total < (int)sizeof(buf) - 1) {                                \
                struct pollfd pfd = {cfd, POLLIN, 0};                             \
                if (poll(&pfd, 1, 2000) <= 0) break;                              \
                int n = read(cfd, buf + total, sizeof(buf) - 1 - total);          \
                if (n <= 0) break;                                                \
                total += n;                                                       \
                buf[total] = '\0';                                                \
                char *sep = strstr(buf, "\r\n\r\n");                              \
                if (sep) {                                                        \
                    char *cl = strcasestr_(buf, "Content-Length:");               \
                    int want = cl ? atoi(cl + 16) : 0;                            \
                    int got = total - (int)(sep - buf) - 4;                       \
                    if (got >= want) break; /* 头体收齐 */                         \
                }                                                                 \
            }                                                                     \
                                                                                  \
            xwplug_req_t req;                                                     \
            memset(&req, 0, sizeof(req));                                         \
            sscanf(buf, "%7s %255s", req.method, req.path);                       \
            char *q = strchr(req.path, '?');                                      \
            if (q) { *q++ = '\0'; snprintf(req.query, sizeof(req.query), "%s", q); }\
            char *sep = strstr(buf, "\r\n\r\n");                                  \
            if (sep) {                                                            \
                int n = total - (int)(sep - buf) - 4;                             \
                if (n < 0) n = 0;                                                 \
                if (n > (int)sizeof(req.body) - 1) n = sizeof(req.body) - 1;      \
                memcpy(req.body, sep + 4, n);                                     \
                req.body[n] = '\0';                                               \
                req.body_len = n;                                                 \
            }                                                                     \
                                                                                  \
            char resp[XWPLUG_RESP_MAX];                                           \
            int code = plug_handle(&req, resp, sizeof(resp));                     \
            if (code <= 0) { code = 500; snprintf(resp, sizeof(resp),             \
                "{\"error\":\"plugin internal\"}"); }                             \
                                                                                  \
            char head[128];                                                       \
            int hl = snprintf(head, sizeof(head),                                 \
                "HTTP/1.0 %d %s\r\nContent-Length: %d\r\n\r\n",                   \
                code, code == 200 ? "OK" : "ERR", (int)strlen(resp));             \
            write(cfd, head, hl);                                                 \
            write(cfd, resp, strlen(resp));                                       \
            close(cfd);                                                           \
            alarm(0);                                                             \
        }                                                                         \
        return 0;                                                                 \
    }

/* strcasestr 兼容(部分 uClibc 无导出) */
__attribute__((unused)) static char *strcasestr_(const char *hay, const char *needle)
{
    if (!hay || !needle) return NULL;
    size_t nl = strlen(needle);
    for (; *hay; hay++) {
        size_t i;
        for (i = 0; i < nl; i++) {
            char a = hay[i], b = needle[i];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (!a || a != b) break;
        }
        if (i == nl) return (char *)hay;
    }
    return NULL;
}

#endif /* XWPLUG_COMMON_H */
