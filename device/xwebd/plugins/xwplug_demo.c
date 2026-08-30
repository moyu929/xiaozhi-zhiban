/**
 * @file xwplug_demo.c
 * @brief 框架验证插件(B0): 回显请求要素, 供网关全链联调
 *
 * 端点(经 /api/plugin/demo/<sub> 访问):
 *   GET  /ping       -> {"ok":true,"plugin":"demo"}
 *   POST /echo       -> 回显 body
 *   GET  /crash      -> 主动段错误, 验证隔离与退避重启
 */

#include "xwplug_common.h"

int plug_handle(const xwplug_req_t *req, char *resp, int resp_size)
{
    if (strcmp(req->path, "/ping") == 0) {
        snprintf(resp, resp_size, "{\"ok\":true,\"plugin\":\"demo\"}");
        return 200;
    }
    if (strcmp(req->path, "/echo") == 0) {
        /* body 中的 " 转义后回显 */
        char esc[XWPLUG_RESP_MAX];
        int j = 0;
        for (int i = 0; req->body[i] && j < (int)sizeof(esc) - 2; i++) {
            if (req->body[i] == '"' || req->body[i] == '\\') esc[j++] = '\\';
            esc[j++] = req->body[i];
        }
        esc[j] = '\0';
        snprintf(resp, resp_size,
                 "{\"plugin\":\"demo\",\"method\":\"%s\",\"path\":\"%s\","
                 "\"query\":\"%s\",\"body\":\"%s\"}",
                 req->method, req->path, req->query, esc);
        return 200;
    }
    if (strcmp(req->path, "/crash") == 0) {
        /* 崩溃注入: 验证 xwebd 探测/退避/隔离 */
        volatile int *p = (int *)0;
        *p = 1;
        return 200; /* 不可达 */
    }
    snprintf(resp, resp_size, "{\"error\":\"not found\",\"path\":\"%s\"}", req->path);
    return 404;
}

XWPLUG_MAIN("demo")
