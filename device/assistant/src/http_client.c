/**
 * @file http_client.c
 * @brief HTTP客户端模块
 *
 * 本模块基于TLS传输层实现HTTP/HTTPS客户端功能，主要功能包括：
 * - URL解析（支持http和https协议）
 * - HTTP GET请求
 * - HTTP POST请求（支持JSON请求体）
 * - HTTP响应接收与解析（状态码、头部、消息体）
 * - 响应超时控制
 */

#include "http_client.h"
#include "xiaozhi_config.h"
#include "plog.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>

/* HTTP响应总超时时间（毫秒） */
#define HTTP_RESPONSE_TIMEOUT_MS 20000
/* 单次poll等待超时时间（毫秒） */
#define HTTP_POLL_TIMEOUT_MS 5000

/**
 * @brief 获取当前时间戳（毫秒）
 *
 * @return 当前时间距epoch的毫秒数
 */
static int64_t http_get_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/**
 * @brief 解析URL为结构化信息
 *
 * 支持http://和https://协议，提取主机名、端口、路径等信息。
 * 若URL中未指定端口，https默认443，http默认80。
 * 若URL中未指定路径，默认为"/"。
 *
 * @param url     待解析的URL字符串
 * @param parsed  输出的解析结果结构体
 * @return 0=成功，-1=URL格式无效
 */
int http_parse_url(const char *url, http_url_t *parsed)
{
    if (!url || !parsed)
        return -1;

    memset(parsed, 0, sizeof(http_url_t));

    const char *p = url;

    /* 识别协议类型 */
    if (strncmp(p, "https://", 8) == 0)
    {
        parsed->use_ssl = true;
        parsed->port = 443;
        p += 8;
    }
    else if (strncmp(p, "http://", 7) == 0)
    {
        parsed->use_ssl = false;
        parsed->port = 80;
        p += 7;
    }
    else
    {
        return -1;
    }

    /* 查找路径分隔符和端口号分隔符 */
    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');

    /* URL中包含端口号 */
    if (colon && (!slash || colon < slash))
    {
        size_t host_len = colon - p;
        if (host_len >= HTTP_MAX_HOST_LEN)
            host_len = HTTP_MAX_HOST_LEN - 1;
        memcpy(parsed->host, p, host_len);
        parsed->port = atoi(colon + 1);
        p = slash ? slash : p + strlen(p);
    }
    else if (slash)
    {
        /* 无端口号，有路径 */
        size_t host_len = slash - p;
        if (host_len >= HTTP_MAX_HOST_LEN)
            host_len = HTTP_MAX_HOST_LEN - 1;
        memcpy(parsed->host, p, host_len);
        p = slash;
    }
    else
    {
        /* 无端口号也无路径 */
        strncpy(parsed->host, p, HTTP_MAX_HOST_LEN - 1);
        p = p + strlen(p);
    }

    /* 提取路径部分 */
    if (*p)
    {
        strncpy(parsed->path, p, HTTP_MAX_PATH_LEN - 1);
    }
    else
    {
        strcpy(parsed->path, "/");
    }

    return 0;
}

/**
 * @brief 接收并解析HTTP响应
 *
 * 从TLS连接读取HTTP响应数据，解析状态行、头部和消息体。
 * 支持Content-Length和连接关闭两种方式确定消息体结束。
 * 总超时时间为HTTP_RESPONSE_TIMEOUT_MS。
 *
 * @param tls      TLS传输层实例
 * @param response 输出的HTTP响应结构体
 * @return 0=成功，-1=失败
 */
static int recv_response(tls_transport_t *tls, http_response_t *response)
{
    memset(response, 0, sizeof(http_response_t));
    response->body_capacity = HTTP_MAX_RESPONSE_LEN;
    response->body = malloc(response->body_capacity);
    if (!response->body)
    {
        return -1;
    }

    char buffer[4096];
    bool headers_complete = false;
    char *header_end = NULL;
    size_t header_len = 0;
    int content_length = -1;

    int64_t start_time = http_get_time_ms();

    while (true)
    {
        /* 检查总超时 */
        int64_t elapsed = http_get_time_ms() - start_time;
        if (elapsed > HTTP_RESPONSE_TIMEOUT_MS)
        {
            PLOG_W("HTTP", "响应超时 (%lld ms)", (long long)elapsed);
            break;
        }

        /* 轮询等待数据 */
        int ret = tls_transport_poll(tls, HTTP_POLL_TIMEOUT_MS);
        if (ret < 0)
        {
            PLOG_W("HTTP", "轮询错误");
            break;
        }
        if (ret == 0)
        {
            continue;
        }

        /* 读取数据 */
        ret = tls_transport_read(tls, (uint8_t *)buffer, sizeof(buffer) - 1);
        if (ret < 0)
        {
            if (ret == -2)
            {
                continue;
            }
            PLOG_W("HTTP", "读取错误");
            break;
        }
        if (ret == 0)
        {
            /* 对端关闭连接 */
            if (headers_complete && content_length < 0)
            {
                break;
            }
            if (!headers_complete && header_len > 0)
            {
                break;
            }
            continue;
        }
        buffer[ret] = '\0';

        /* 头部尚未接收完成 */
        if (!headers_complete)
        {
            size_t copy_len = HTTP_MAX_HEADER_LEN - header_len - 1;
            if (copy_len == 0)
            {
                /* 头部缓冲区已满，强制标记完成 */
                headers_complete = true;
                header_end = strstr(response->headers, "\r\n\r\n");
                if (header_end)
                {
                    *header_end = '\0';
                }
            }
            else
            {
                if (copy_len > (size_t)ret)
                    copy_len = ret;
                memcpy(response->headers + header_len, buffer, copy_len);
                header_len += copy_len;
                response->headers[header_len] = '\0';

                header_end = strstr(response->headers, "\r\n\r\n");
            }

            /* 检测到头部结束标记 */
            if (header_end && !headers_complete)
            {
                headers_complete = true;
                *header_end = '\0';

                /* 解析Content-Length */
                char *cl = strcasestr(response->headers, "Content-Length:");
                if (cl)
                {
                    content_length = atoi(cl + 15);
                }

                /* 提取头部之后的消息体数据 */
                size_t body_offset = (header_end - response->headers) + 4;
                size_t body_in_buffer = header_len - body_offset;
                if (body_in_buffer > 0)
                {
                    if (body_in_buffer > response->body_capacity - 1)
                    {
                        body_in_buffer = response->body_capacity - 1;
                    }
                    memcpy(response->body, response->headers + body_offset, body_in_buffer);
                    response->body_len = body_in_buffer;
                }

                /* 已收到完整消息体 */
                if (content_length >= 0 && response->body_len >= (size_t)content_length)
                {
                    break;
                }
            }
        }
        else
        {
            /* 头部已完成，继续接收消息体 */
            size_t copy_len = response->body_capacity - response->body_len - 1;
            if (copy_len > (size_t)ret)
                copy_len = ret;
            if (copy_len > 0)
            {
                memcpy(response->body + response->body_len, buffer, copy_len);
                response->body_len += copy_len;
            }

            /* 按Content-Length判断是否接收完成 */
            if (content_length >= 0 && response->body_len >= (size_t)content_length)
            {
                break;
            }
            /* 无Content-Length时，收到任意数据即结束 */
            if (content_length < 0 && response->body_len > 0)
            {
                break;
            }
        }
    }

    /* 确保消息体以null结尾 */
    if (response->body_len > 0)
    {
        response->body[response->body_len] = '\0';
    }

    /* 解析HTTP状态行，提取状态码和状态文本 */
    if (response->headers[0])
    {
        if (sscanf(response->headers, "HTTP/%*d.%*d %d %63[^\r\n]",
                   &response->status_code, response->status_text) != 2)
        {
            response->status_code = 0;
        }
    }

    PLOG_I("HTTP", "响应: %d %s, body_len=%zu",
           response->status_code, response->status_text, response->body_len);

    return 0;
}

/**
 * @brief 发送HTTP GET请求
 *
 * 解析URL，建立TLS连接，发送GET请求并接收响应。
 * 请求完成后自动断开连接并释放TLS资源。
 *
 * @param url      请求URL
 * @param headers  附加请求头（可为NULL）
 * @param response 输出的HTTP响应结构体
 * @return 0=成功，-1=失败
 */
int http_get(const char *url, const char *headers, http_response_t *response)
{
    http_url_t parsed;
    if (http_parse_url(url, &parsed) != 0)
    {
        PLOG_E("HTTP", "URL解析失败: %s", url);
        return -1;
    }

    PLOG_I("HTTP", "GET %s%s", parsed.host, parsed.path);

    tls_transport_t tls;
    if (tls_transport_init(&tls) != 0)
    {
        return -1;
    }

    /* 建立TLS连接 */
    if (tls_transport_connect(&tls, parsed.host, parsed.port) != 0)
    {
        PLOG_E("HTTP", "连接失败: %s", tls_transport_get_error(&tls));
        tls_transport_destroy(&tls);
        return -1;
    }

    /* 构建GET请求 */
    char request[8192];
    int request_len = snprintf(request, sizeof(request),
                               "GET %s HTTP/1.1\r\n"
                               "Host: %s\r\n"
                               "User-Agent: " XIAOZHI_CHIP_MODEL "/1.0.0\r\n"
                               "Accept: application/json\r\n"
                               "Connection: close\r\n"
                               "%s"
                               "\r\n",
                               parsed.path, parsed.host, headers ? headers : "");

    /* 发送请求 */
    if (tls_transport_write(&tls, (const uint8_t *)request, request_len) < 0)
    {
        PLOG_E("HTTP", "发送请求失败");
        tls_transport_disconnect(&tls);
        tls_transport_destroy(&tls);
        return -1;
    }

    /* 接收响应 */
    int ret = recv_response(&tls, response);

    tls_transport_disconnect(&tls);
    tls_transport_destroy(&tls);

    return ret;
}

/**
 * @brief 发送HTTP POST请求
 *
 * 解析URL，建立TLS连接，发送POST请求（含JSON请求体）并接收响应。
 * 请求完成后自动断开连接并释放TLS资源。
 *
 * @param url      请求URL
 * @param headers  附加请求头（可为NULL）
 * @param body     请求体（JSON字符串，可为NULL）
 * @param response 输出的HTTP响应结构体
 * @return 0=成功，-1=失败
 */
int http_post(const char *url, const char *headers, const char *body, http_response_t *response)
{
    http_url_t parsed;
    if (http_parse_url(url, &parsed) != 0)
    {
        PLOG_E("HTTP", "URL解析失败: %s", url);
        return -1;
    }

    PLOG_I("HTTP", "POST %s%s", parsed.host, parsed.path);

    tls_transport_t tls;
    if (tls_transport_init(&tls) != 0)
    {
        return -1;
    }

    /* 建立TLS连接 */
    if (tls_transport_connect(&tls, parsed.host, parsed.port) != 0)
    {
        PLOG_E("HTTP", "连接失败: %s", tls_transport_get_error(&tls));
        tls_transport_destroy(&tls);
        return -1;
    }

    size_t body_len = body ? strlen(body) : 0;

    /* 构建POST请求 */
    char request[8192];
    int request_len = snprintf(request, sizeof(request),
                               "POST %s HTTP/1.1\r\n"
                               "Host: %s\r\n"
                               "User-Agent: " XIAOZHI_CHIP_MODEL "/1.0.0\r\n"
                               "Accept: application/json\r\n"
                               "Content-Type: application/json\r\n"
                               "Content-Length: %zu\r\n"
                               "Connection: close\r\n"
                               "%s"
                               "\r\n"
                               "%s",
                               parsed.path, parsed.host, body_len, headers ? headers : "", body ? body : "");

    /* 发送请求 */
    if (tls_transport_write(&tls, (const uint8_t *)request, request_len) < 0)
    {
        PLOG_E("HTTP", "发送请求失败");
        tls_transport_disconnect(&tls);
        tls_transport_destroy(&tls);
        return -1;
    }

    /* 接收响应 */
    int ret = recv_response(&tls, response);

    tls_transport_disconnect(&tls);
    tls_transport_destroy(&tls);

    return ret;
}

/**
 * @brief 释放HTTP响应资源
 *
 * 释放响应结构体中动态分配的消息体内存。
 *
 * @param response HTTP响应结构体
 */
void http_response_free(http_response_t *response)
{
    if (response && response->body)
    {
        free(response->body);
        response->body = NULL;
        response->body_len = 0;
    }
}
