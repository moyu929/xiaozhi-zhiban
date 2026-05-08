/**
 * @file tls_transport.c
 * @brief TLS安全传输层模块
 *
 * 本模块基于mbedTLS库实现TLS加密通信传输，主要功能包括：
 * - TLS上下文初始化与销毁
 * - 带超时的TCP连接和TLS握手
 * - 域名解析（含备用IP回退机制）
 * - TLS数据读写（带超时和重试）
 * - 连接重试状态管理（指数退避策略）
 * - 连接状态轮询
 */

#include "tls_transport.h"
#include "plog.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>
#include <arpa/inet.h>
#include <poll.h>

#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"

/* TCP连接超时时间（毫秒） */
#define TCP_CONNECT_TIMEOUT_MS 10000
/* TLS握手超时时间（毫秒） */
#define TLS_HANDSHAKE_TIMEOUT_MS 15000

/* DNS解析失败时的备用IP地址列表 */
static const char *g_fallback_ips[] = {
    "112.74.84.224",
    NULL
};

/**
 * @brief 获取当前时间戳（毫秒）
 *
 * @return 当前时间距epoch的毫秒数
 */
static int64_t tls_get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/**
 * @brief 初始化TLS传输层
 *
 * 分配并初始化mbedTLS各组件（SSL上下文、配置、熵源、DRBG、网络上下文），
 * 设置为客户端模式，配置可选证书验证和随机数生成器。
 *
 * @param tls TLS传输层实例
 * @return 0=成功，-1=失败（内存分配或mbedTLS初始化错误）
 */
int tls_transport_init(tls_transport_t *tls) {
    if (!tls) return -1;

    memset(tls, 0, sizeof(tls_transport_t));
    tls->sockfd = -1;

    /* 分配mbedTLS各组件内存 */
    tls->ssl_ctx = calloc(1, sizeof(mbedtls_ssl_context));
    tls->ssl_conf = calloc(1, sizeof(mbedtls_ssl_config));
    tls->entropy = calloc(1, sizeof(mbedtls_entropy_context));
    tls->ctr_drbg = calloc(1, sizeof(mbedtls_ctr_drbg_context));
    tls->net_ctx = calloc(1, sizeof(mbedtls_net_context));

    if (!tls->ssl_ctx || !tls->ssl_conf || !tls->entropy || !tls->ctr_drbg || !tls->net_ctx) {
        snprintf(tls->error, sizeof(tls->error), "内存分配失败");
        tls_transport_destroy(tls);
        return -1;
    }

    /* 初始化mbedTLS各组件 */
    mbedtls_net_init((mbedtls_net_context*)tls->net_ctx);
    mbedtls_ssl_init((mbedtls_ssl_context*)tls->ssl_ctx);
    mbedtls_ssl_config_init((mbedtls_ssl_config*)tls->ssl_conf);
    mbedtls_entropy_init((mbedtls_entropy_context*)tls->entropy);
    mbedtls_ctr_drbg_init((mbedtls_ctr_drbg_context*)tls->ctr_drbg);

    /* 使用熵源初始化DRBG随机数生成器 */
    const char *pers = "xiaozhi-assistant";
    int ret = mbedtls_ctr_drbg_seed((mbedtls_ctr_drbg_context*)tls->ctr_drbg,
                                    mbedtls_entropy_func,
                                    (mbedtls_entropy_context*)tls->entropy,
                                    (const unsigned char *)pers, strlen(pers));
    if (ret != 0) {
        snprintf(tls->error, sizeof(tls->error), "随机数种子初始化失败: -0x%x", -ret);
        tls_transport_destroy(tls);
        return -1;
    }

    /* 配置SSL为客户端模式 */
    ret = mbedtls_ssl_config_defaults((mbedtls_ssl_config*)tls->ssl_conf,
                                      MBEDTLS_SSL_IS_CLIENT,
                                      MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        snprintf(tls->error, sizeof(tls->error), "SSL默认配置设置失败: -0x%x", -ret);
        tls_transport_destroy(tls);
        return -1;
    }

    /* 设置可选证书验证模式（不强制验证服务器证书） */
    mbedtls_ssl_conf_authmode((mbedtls_ssl_config*)tls->ssl_conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_rng((mbedtls_ssl_config*)tls->ssl_conf, mbedtls_ctr_drbg_random,
                         (mbedtls_ctr_drbg_context*)tls->ctr_drbg);

    /* 将SSL配置绑定到SSL上下文 */
    ret = mbedtls_ssl_setup((mbedtls_ssl_context*)tls->ssl_ctx,
                            (mbedtls_ssl_config*)tls->ssl_conf);
    if (ret != 0) {
        snprintf(tls->error, sizeof(tls->error), "SSL上下文绑定失败: -0x%x", -ret);
        tls_transport_destroy(tls);
        return -1;
    }

    tls->ssl_initialized = true;
    PLOG_I("TLS", "传输层初始化完成");
    return 0;
}

/**
 * @brief 销毁TLS传输层
 *
 * 断开连接，释放mbedTLS各组件资源，释放动态分配的内存，
 * 将结构体清零并重置文件描述符。
 *
 * @param tls TLS传输层实例
 */
void tls_transport_destroy(tls_transport_t *tls) {
    if (!tls) return;

    tls_transport_disconnect(tls);

    /* 释放mbedTLS各组件 */
    if (tls->ssl_initialized) {
        mbedtls_ssl_free((mbedtls_ssl_context*)tls->ssl_ctx);
        mbedtls_ssl_config_free((mbedtls_ssl_config*)tls->ssl_conf);
        mbedtls_ctr_drbg_free((mbedtls_ctr_drbg_context*)tls->ctr_drbg);
        mbedtls_entropy_free((mbedtls_entropy_context*)tls->entropy);
        mbedtls_net_free((mbedtls_net_context*)tls->net_ctx);
        tls->ssl_initialized = false;
    }

    /* 释放动态分配的内存 */
    free(tls->ssl_ctx);
    free(tls->ssl_conf);
    free(tls->entropy);
    free(tls->ctr_drbg);
    free(tls->net_ctx);

    memset(tls, 0, sizeof(tls_transport_t));
    tls->sockfd = -1;
}

/**
 * @brief 初始化重试状态
 *
 * 设置最大重试次数，初始退避延迟为1秒，采用指数退避策略。
 *
 * @param state       重试状态实例
 * @param max_retries 最大重试次数（<=0时使用默认值）
 */
void tls_retry_state_init(tls_retry_state_t *state, int max_retries) {
    if (!state) return;
    memset(state, 0, sizeof(tls_retry_state_t));
    state->max_retries = (max_retries <= 0) ? TLS_DEFAULT_MAX_RETRIES : max_retries;
    state->delay_seconds = 1;
    state->next_retry_ms = 0;
    state->in_progress = false;
}

/**
 * @brief 判断是否还可以重试
 *
 * @param state 重试状态实例
 * @return true=还可以重试，false=已达最大重试次数
 */
bool tls_retry_should_retry(tls_retry_state_t *state) {
    if (!state) return false;
    return state->attempt < state->max_retries;
}

/**
 * @brief 判断是否到了重试时间
 *
 * @param state 重试状态实例
 * @return true=可以重试，false=还未到重试时间
 */
bool tls_retry_is_time_to_retry(tls_retry_state_t *state) {
    if (!state) return false;
    if (state->next_retry_ms == 0) return true;
    return tls_get_time_ms() >= state->next_retry_ms;
}

/**
 * @brief 记录一次重试失败
 *
 * 增加重试计数，计算下次重试时间（指数退避，最大16秒）。
 *
 * @param state 重试状态实例
 */
void tls_retry_record_failure(tls_retry_state_t *state) {
    if (!state) return;
    state->attempt++;
    state->in_progress = true;
    if (state->attempt < state->max_retries) {
        state->next_retry_ms = tls_get_time_ms() + (int64_t)state->delay_seconds * 1000;
        /* 指数退避，延迟翻倍，上限16秒 */
        state->delay_seconds = (state->delay_seconds * 2 > 16) ? 16 : state->delay_seconds * 2;
    }
}

/**
 * @brief 重置重试状态
 *
 * 保留最大重试次数，重置其他所有状态。
 *
 * @param state 重试状态实例
 */
void tls_retry_reset(tls_retry_state_t *state) {
    if (!state) return;
    tls_retry_state_init(state, state->max_retries);
}

/**
 * @brief 获取距离下次重试的等待时间（毫秒）
 *
 * @param state 重试状态实例
 * @return 需要等待的毫秒数，0=无需等待或已到重试时间
 */
int tls_retry_get_wait_ms(tls_retry_state_t *state) {
    if (!state) return 0;
    if (state->next_retry_ms == 0) return 0;
    int64_t now = tls_get_time_ms();
    if (now >= state->next_retry_ms) return 0;
    return (int)(state->next_retry_ms - now);
}

/**
 * @brief 解析域名为IP地址
 *
 * 首先尝试通过getaddrinfo进行DNS解析，若失败则使用备用IP列表。
 *
 * @param hostname 主机名/域名
 * @param addr     输出的IPv4地址结构
 * @return 0=成功，-1=解析失败
 */
static int resolve_hostname(const char *hostname, struct in_addr *addr) {
    PLOG_I("TLS", "正在解析域名: %s", hostname);

    struct addrinfo hints, *result = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int ret = getaddrinfo(hostname, NULL, &hints, &result);
    if (ret == 0 && result != NULL) {
        struct sockaddr_in *ipv4 = (struct sockaddr_in *)result->ai_addr;
        memcpy(addr, &ipv4->sin_addr, sizeof(struct in_addr));
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, addr, ip_str, sizeof(ip_str));
        PLOG_I("TLS", "域名解析成功 %s -> %s", hostname, ip_str);
        freeaddrinfo(result);
        return 0;
    }

    if (result) freeaddrinfo(result);

    /* DNS解析失败，尝试备用IP */
    PLOG_W("TLS", "DNS解析失败: %s, 尝试备用IP", ret != 0 ? gai_strerror(ret) : "无结果");
    for (int i = 0; g_fallback_ips[i] != NULL; i++) {
        if (inet_aton(g_fallback_ips[i], addr) != 0) {
            PLOG_I("TLS", "使用备用IP: %s", g_fallback_ips[i]);
            return 0;
        }
    }
    return -1;
}

/**
 * @brief 带超时的TCP连接
 *
 * 将socket设为非阻塞模式发起连接，使用poll等待连接完成，
 * 超时后恢复socket为阻塞模式。
 *
 * @param sockfd     套接字文件描述符
 * @param addr       目标地址
 * @param addrlen    地址长度
 * @param timeout_ms 连接超时时间（毫秒）
 * @return 0=成功，-1=失败或超时
 */
static int tcp_connect_with_timeout(int sockfd, const struct sockaddr *addr, socklen_t addrlen, int timeout_ms) {
    /* 设置非阻塞模式 */
    int flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

    int ret = connect(sockfd, addr, addrlen);
    if (ret < 0 && errno != EINPROGRESS) {
        return -1;
    }

    /* 连接立即成功 */
    if (ret == 0) {
        fcntl(sockfd, F_SETFL, flags);
        return 0;
    }

    /* 使用poll等待连接完成 */
    struct pollfd pfd;
    pfd.fd = sockfd;
    pfd.events = POLLOUT;
    ret = poll(&pfd, 1, timeout_ms);

    if (ret <= 0) {
        return -1;
    }

    /* 检查连接是否真正成功 */
    int sock_err = 0;
    socklen_t err_len = sizeof(sock_err);
    getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &sock_err, &err_len);
    if (sock_err != 0) {
        errno = sock_err;
        return -1;
    }

    /* 恢复阻塞模式 */
    fcntl(sockfd, F_SETFL, flags);
    return 0;
}

/**
 * @brief 建立TLS连接
 *
 * 完成域名解析、TCP连接（带超时）、TLS握手（带超时）的全过程。
 * 设置socket收发超时，绑定mbedTLS网络IO。
 *
 * @param tls  TLS传输层实例
 * @param host 目标主机名
 * @param port 目标端口
 * @return 0=成功，-1=失败
 */
int tls_transport_connect(tls_transport_t *tls, const char *host, int port) {
    if (!tls || !host) return -1;

    strncpy(tls->host, host, sizeof(tls->host) - 1);
    tls->port = port;

    PLOG_I("TLS", "正在连接 %s:%d", host, port);

    /* 域名解析 */
    struct in_addr addr;
    if (resolve_hostname(host, &addr) != 0) {
        snprintf(tls->error, sizeof(tls->error), "域名解析失败: %s", host);
        return -1;
    }

    /* 创建TCP套接字 */
    tls->sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (tls->sockfd < 0) {
        snprintf(tls->error, sizeof(tls->error), "创建套接字失败: %s", strerror(errno));
        return -1;
    }

    /* 构建服务器地址 */
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    memcpy(&serv_addr.sin_addr, &addr, sizeof(addr));

    /* 带超时的TCP连接 */
    if (tcp_connect_with_timeout(tls->sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr), TCP_CONNECT_TIMEOUT_MS) != 0) {
        snprintf(tls->error, sizeof(tls->error), "TCP连接失败: %s", strerror(errno));
        close(tls->sockfd);
        tls->sockfd = -1;
        return -1;
    }

    PLOG_I("TLS", "TCP连接成功");

    /* 设置socket收发超时 */
    struct timeval snd_timeout = {3, 0};
    setsockopt(tls->sockfd, SOL_SOCKET, SO_SNDTIMEO, &snd_timeout, sizeof(snd_timeout));
    struct timeval rcv_timeout = {5, 0};
    setsockopt(tls->sockfd, SOL_SOCKET, SO_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout));

    /* 绑定mbedTLS网络IO和SSL上下文 */
    ((mbedtls_net_context*)tls->net_ctx)->fd = tls->sockfd;
    mbedtls_ssl_set_bio((mbedtls_ssl_context*)tls->ssl_ctx,
                        tls->net_ctx,
                        mbedtls_net_send,
                        mbedtls_net_recv,
                        NULL);

    /* 设置SNI主机名 */
    mbedtls_ssl_set_hostname((mbedtls_ssl_context*)tls->ssl_ctx, host);

    /* 执行TLS握手，带超时检测 */
    PLOG_I("TLS", "开始TLS握手");
    int64_t handshake_start = tls_get_time_ms();
    int hs_ret;

    while ((hs_ret = mbedtls_ssl_handshake((mbedtls_ssl_context*)tls->ssl_ctx)) != 0) {
        /* 非WANT_READ/WANT_WRITE错误，握手失败 */
        if (hs_ret != MBEDTLS_ERR_SSL_WANT_READ && hs_ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            snprintf(tls->error, sizeof(tls->error), "TLS握手失败: -0x%x", -hs_ret);
            close(tls->sockfd);
            tls->sockfd = -1;
            return -1;
        }

        /* 检查握手是否超时 */
        int64_t elapsed = tls_get_time_ms() - handshake_start;
        if (elapsed > TLS_HANDSHAKE_TIMEOUT_MS) {
            snprintf(tls->error, sizeof(tls->error), "TLS握手超时 (%lld ms)", (long long)elapsed);
            close(tls->sockfd);
            tls->sockfd = -1;
            return -1;
        }

        /* 计算poll等待时间，最小100ms */
        int poll_timeout = TLS_HANDSHAKE_TIMEOUT_MS - (int)elapsed;
        if (poll_timeout < 100) poll_timeout = 100;

        struct pollfd pfd;
        pfd.fd = tls->sockfd;
        pfd.events = (hs_ret == MBEDTLS_ERR_SSL_WANT_READ) ? POLLIN : POLLOUT;
        poll(&pfd, 1, poll_timeout);
    }

    PLOG_I("TLS", "TLS握手完成, 耗时%lld ms", (long long)(tls_get_time_ms() - handshake_start));
    tls->connected = true;
    return 0;
}

/**
 * @brief 断开TLS连接
 *
 * 发送TLS关闭通知，关闭TCP套接字。
 *
 * @param tls TLS传输层实例
 */
void tls_transport_disconnect(tls_transport_t *tls) {
    if (!tls) return;

    /* 发送TLS关闭通知 */
    if (tls->connected && tls->ssl_ctx) {
        mbedtls_ssl_close_notify((mbedtls_ssl_context*)tls->ssl_ctx);
        tls->connected = false;
    }

    /* 关闭TCP套接字 */
    if (tls->sockfd >= 0) {
        close(tls->sockfd);
        tls->sockfd = -1;
    }
}

/**
 * @brief 通过TLS写入数据
 *
 * 循环写入直到所有数据发送完毕，处理WANT_READ/WANT_WRITE情况，
 * 总超时时间为5秒。
 *
 * @param tls TLS传输层实例
 * @param data 要发送的数据
 * @param len  数据长度
 * @return 实际写入的字节数，-1=失败或超时
 */
int tls_transport_write(tls_transport_t *tls, const uint8_t *data, size_t len) {
    if (!tls || !data || !tls->connected) return -1;

    size_t total_sent = 0;
    int64_t start_time = tls_get_time_ms();

    while (total_sent < len) {
        int ret = mbedtls_ssl_write((mbedtls_ssl_context*)tls->ssl_ctx,
                                    data + total_sent, len - total_sent);
        if (ret < 0) {
            if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
                /* 检查是否超时 */
                int64_t elapsed = tls_get_time_ms() - start_time;
                if (elapsed > 5000) {
                    snprintf(tls->error, sizeof(tls->error), "SSL写入超时");
                    return -1;
                }
                struct pollfd pfd;
                pfd.fd = tls->sockfd;
                pfd.events = (ret == MBEDTLS_ERR_SSL_WANT_READ) ? POLLIN : POLLOUT;
                poll(&pfd, 1, 100);
                continue;
            }
            snprintf(tls->error, sizeof(tls->error), "SSL写入失败: -0x%x", -ret);
            return -1;
        }
        total_sent += ret;
    }

    return (int)total_sent;
}

/**
 * @brief 通过TLS读取数据
 *
 * 从TLS连接读取数据，处理WANT_READ/WANT_WRITE和对端关闭通知。
 *
 * @param tls     TLS传输层实例
 * @param data    接收缓冲区
 * @param max_len 缓冲区最大长度
 * @return 实际读取的字节数，0=对端关闭，-1=错误，-2=需要重试(WANT_READ/WANT_WRITE)
 */
int tls_transport_read(tls_transport_t *tls, uint8_t *data, size_t max_len) {
    if (!tls || !data || !tls->connected) return -1;

    int ret = mbedtls_ssl_read((mbedtls_ssl_context*)tls->ssl_ctx, data, max_len);
    if (ret < 0) {
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            return -2;
        }
        /* 对端正常关闭连接 */
        if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            return 0;
        }
        snprintf(tls->error, sizeof(tls->error), "SSL读取失败: -0x%x", -ret);
        return -1;
    }

    return ret;
}

/**
 * @brief 轮询TLS连接是否有数据可读
 *
 * @param tls        TLS传输层实例
 * @param timeout_ms 超时时间（毫秒）
 * @return >0=有数据可读，0=超时，-1=错误
 */
int tls_transport_poll(tls_transport_t *tls, int timeout_ms) {
    if (!tls || tls->sockfd < 0) return -1;

    struct pollfd pfd;
    pfd.fd = tls->sockfd;
    pfd.events = POLLIN;

    return poll(&pfd, 1, timeout_ms);
}

/**
 * @brief 检查TLS连接是否已建立
 *
 * @param tls TLS传输层实例
 * @return true=已连接，false=未连接
 */
bool tls_transport_is_connected(tls_transport_t *tls) {
    if (!tls) return false;
    return tls->connected;
}

/**
 * @brief 获取最近的错误信息
 *
 * @param tls TLS传输层实例
 * @return 错误信息字符串
 */
const char* tls_transport_get_error(tls_transport_t *tls) {
    if (!tls) return "传输层实例为空";
    return tls->error[0] ? tls->error : "无错误";
}
