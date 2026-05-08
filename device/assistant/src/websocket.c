/**
 * @file websocket.c
 * @brief WebSocket客户端实现
 *
 * 实现WebSocket协议的客户端功能，包括：
 * - WebSocket连接建立（握手）
 * - 帧的发送和接收（文本帧、二进制帧、Ping/Pong帧、关闭帧）
 * - TLS加密连接支持（wss://）
 * - 心跳保活机制（自动Ping）
 * - 接收缓冲区管理和帧解析
 */

#include "websocket.h"
#include "plog.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>

/* WebSocket帧操作码定义 */
#define WS_OPCODE_TEXT   0x01   /* 文本帧 */
#define WS_OPCODE_BINARY 0x02   /* 二进制帧 */
#define WS_OPCODE_CLOSE  0x08   /* 关闭帧 */
#define WS_OPCODE_PING   0x09   /* Ping帧 */
#define WS_OPCODE_PONG   0x0A   /* Pong帧 */

/**
 * @brief 获取当前时间的毫秒数
 * @return 当前时间戳（毫秒）
 */
static uint64_t ws_get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/**
 * @brief 解析WebSocket URL，提取协议、主机、端口和路径
 * @param ws WebSocket实例指针
 * @return 0成功，-1失败
 */
static int ws_parse_url(websocket_t *ws) {
    const char *p = ws->url;

    /* 判断协议类型：wss或ws */
    if (strncmp(p, "wss://", 6) == 0) {
        ws->secure = true;
        ws->port = 443;
        p += 6;
    } else if (strncmp(p, "ws://", 5) == 0) {
        ws->secure = false;
        ws->port = 80;
        p += 5;
    } else {
        return -1;
    }

    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');

    /* 提取主机名和端口号 */
    if (colon && (!slash || colon < slash)) {
        size_t host_len = colon - p;
        if (host_len >= sizeof(ws->host)) host_len = sizeof(ws->host) - 1;
        memcpy(ws->host, p, host_len);
        ws->host[host_len] = '\0';
        ws->port = atoi(colon + 1);
        p = slash ? slash : p + strlen(p);
    } else if (slash) {
        size_t host_len = slash - p;
        if (host_len >= sizeof(ws->host)) host_len = sizeof(ws->host) - 1;
        memcpy(ws->host, p, host_len);
        ws->host[host_len] = '\0';
        p = slash;
    } else {
        strncpy(ws->host, p, sizeof(ws->host) - 1);
        p = p + strlen(p);
    }

    /* 提取路径部分 */
    if (*p) {
        strncpy(ws->path, p, sizeof(ws->path) - 1);
    } else {
        strcpy(ws->path, "/");
    }

    return 0;
}

/**
 * @brief 底层发送原始数据
 * @param ws WebSocket实例指针
 * @param data 待发送数据
 * @param len 数据长度
 * @return 实际发送字节数，失败返回-1
 */
static int ws_send_raw(websocket_t *ws, const uint8_t *data, size_t len) {
    if (!ws) return -1;
    if (!ws->secure && ws->sockfd < 0) return -1;

    if (ws->secure) {
        /* TLS加密连接发送 */
        if (!tls_transport_is_connected(&ws->tls)) return -1;
        return tls_transport_write(&ws->tls, data, len);
    } else {
        /* 普通TCP连接发送 */
        ssize_t total = 0;
        while (total < (ssize_t)len) {
            ssize_t n = write(ws->sockfd, data + total, len - total);
            if (n <= 0) return -1;
            total += n;
        }
        return (int)total;
    }
}

/**
 * @brief 底层接收原始数据
 * @param ws WebSocket实例指针
 * @param buf 接收缓冲区
 * @param max_len 缓冲区最大长度
 * @return 实际接收字节数，连接关闭返回0，失败返回-1
 */
static int ws_recv_raw(websocket_t *ws, uint8_t *buf, size_t max_len) {
    if (!ws) return -1;
    if (!ws->secure && ws->sockfd < 0) return -1;

    if (ws->secure) {
        /* TLS加密连接接收 */
        if (!tls_transport_is_connected(&ws->tls)) return -1;
        return tls_transport_read(&ws->tls, buf, max_len);
    } else {
        /* 普通TCP连接接收 */
        ssize_t n = read(ws->sockfd, buf, max_len);
        if (n <= 0) return (n == 0) ? 0 : -1;
        return (int)n;
    }
}

/**
 * @brief 轮询socket是否有数据可读
 * @param ws WebSocket实例指针
 * @param timeout_ms 超时时间（毫秒）
 * @return >0有数据可读，0超时，-1出错
 */
static int ws_poll_raw(websocket_t *ws, int timeout_ms) {
    if (!ws || ws->sockfd < 0) return -1;

    struct pollfd pfd;
    pfd.fd = ws->sockfd;
    pfd.events = POLLIN;
    return poll(&pfd, 1, timeout_ms);
}

/**
 * @brief 生成WebSocket握手所需的Sec-WebSocket-Key（Base64编码的16字节随机数）
 * @param key 输出缓冲区
 * @param key_len 缓冲区大小
 */
static void ws_generate_key(char *key, int key_len) {
    uint8_t rand_bytes[16];
    /* 优先从/dev/urandom读取随机数 */
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        read(fd, rand_bytes, sizeof(rand_bytes));
        close(fd);
    } else {
        /* 回退到伪随机数 */
        srand(time(NULL));
        for (int i = 0; i < 16; i++) rand_bytes[i] = rand() & 0xFF;
    }

    /* 将16字节随机数编码为Base64字符串 */
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int j = 0;
    for (int i = 0; i < 16; i += 3) {
        key[j++] = b64[rand_bytes[i] >> 2];
        if (i + 1 < 16) {
            key[j++] = b64[((rand_bytes[i] & 0x3) << 4) | (rand_bytes[i + 1] >> 4)];
            if (i + 2 < 16) {
                key[j++] = b64[((rand_bytes[i + 1] & 0xf) << 2) | (rand_bytes[i + 2] >> 6)];
                key[j++] = b64[rand_bytes[i + 2] & 0x3f];
            } else {
                key[j++] = b64[(rand_bytes[i + 1] & 0xf) << 2];
                key[j++] = '=';
            }
        } else {
            key[j++] = b64[(rand_bytes[i] & 0x3) << 4];
            key[j++] = '=';
            key[j++] = '=';
        }
    }
    key[j] = '\0';
}

/**
 * @brief 发送WebSocket握手请求
 * @param ws WebSocket实例指针
 * @return true成功，false失败
 */
static bool ws_send_handshake(websocket_t *ws) {
    char key[32];
    ws_generate_key(key, sizeof(key));

    /* 构造HTTP升级请求 */
    char request[4096];
    int len = snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n",
        ws->path, ws->host, key);

    /* 添加自定义头部 */
    for (int i = 0; i < ws->header_count; i++) {
        len += snprintf(request + len, sizeof(request) - len,
                       "%s: %s\r\n", ws->headers[i].name, ws->headers[i].value);
    }

    len += snprintf(request + len, sizeof(request) - len, "\r\n");

    PLOG_I("WS", "正在发送握手请求到 %s:%d%s (长度=%d)", ws->host, ws->port, ws->path, len);
    PLOG_I("WS", "握手: GET %s 主机=%s 密钥=%s 头部数=%d", ws->path, ws->host, key, ws->header_count);
    for (int hi = 0; hi < ws->header_count; hi++) {
        PLOG_I("WS", "握手: %s: %s", ws->headers[hi].name, ws->headers[hi].value);
    }

    int send_ret = ws_send_raw(ws, (const uint8_t*)request, len);
    if (send_ret < 0) {
        snprintf(ws->error, sizeof(ws->error), "Failed to send handshake (ret=%d, tls_conn=%d, sockfd=%d)",
                 send_ret, tls_transport_is_connected(&ws->tls), ws->sockfd);
        PLOG_E("WS", "握手发送失败: 返回值=%d TLS已连接=%d 套接字=%d 安全=%d",
               send_ret, tls_transport_is_connected(&ws->tls), ws->sockfd, ws->secure);
        return false;
    }

    PLOG_I("WS", "握手已发送 %d 字节", send_ret);
    return true;
}

/**
 * @brief 接收并验证WebSocket握手响应
 * @param ws WebSocket实例指针
 * @return true成功，false失败
 */
static bool ws_recv_handshake_response(websocket_t *ws) {
    char response[4096];
    memset(response, 0, sizeof(response));
    size_t total = 0;
    int64_t start = ws_get_time_ms();

    /* 循环接收直到获取完整的HTTP响应头（以\r\n\r\n结尾）或超时 */
    while (total < sizeof(response) - 1) {
        if (ws_get_time_ms() - start > 10000) {
            snprintf(ws->error, sizeof(ws->error), "Handshake response timeout");
            return false;
        }

        int ret = ws_poll_raw(ws, 2000);
        if (ret < 0) {
            snprintf(ws->error, sizeof(ws->error), "Poll error during handshake");
            return false;
        }
        if (ret == 0) continue;

        ret = ws_recv_raw(ws, (uint8_t*)response + total, sizeof(response) - total - 1);
        if (ret < 0 && ret != -2) {
            snprintf(ws->error, sizeof(ws->error), "Recv error during handshake");
            return false;
        }
        if (ret > 0) {
            total += ret;
            response[total] = '\0';
            if (strstr(response, "\r\n\r\n")) break;
        }
    }

    /* 验证响应状态码是否为101 Switching Protocols */
    if (strncmp(response, "HTTP/1.1 101", 12) != 0 &&
        strncmp(response, "HTTP/1.0 101", 12) != 0) {
        PLOG_E("WS", "握手被拒绝! 状态行:");
        char *line_end = strstr(response, "\r\n");
        if (line_end) {
            int line_len = line_end - response;
            if (line_len > 200) line_len = 200;
            PLOG_E("WS", "  %.*s", line_len, response);
        }
        char *body = strstr(response, "\r\n\r\n");
        if (body) {
            body += 4;
            int body_len = strlen(body);
            if (body_len > 0) {
                if (body_len > 300) body_len = 300;
                PLOG_E("WS", "  响应体: %.*s", body_len, body);
            }
        }
        snprintf(ws->error, sizeof(ws->error), "Invalid handshake response: %.64s", response);
        return false;
    }

    PLOG_I("WS", "握手完成");
    return true;
}

/**
 * @brief 发送一个WebSocket帧
 * @param ws WebSocket实例指针
 * @param opcode 帧操作码（TEXT/BINARY/PING/PONG/CLOSE）
 * @param payload 帧负载数据
 * @param payload_len 负载长度
 * @return 0成功，-1失败
 * @note 发送操作通过互斥锁保护，确保线程安全
 */
static int ws_send_frame(websocket_t *ws, int opcode, const uint8_t *payload, size_t payload_len) {
    if (!ws) return -1;

    pthread_mutex_lock(&ws->send_mutex);

    uint8_t header[14];
    int header_len = 0;

    /* 构造帧头：FIN位 + 操作码 */
    header[0] = 0x80 | (opcode & 0x0F);

    /* 生成掩码密钥（客户端发送的帧必须掩码） */
    uint8_t mask_key[4];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        read(fd, mask_key, 4);
        close(fd);
    } else {
        for (int i = 0; i < 4; i++) mask_key[i] = rand();
    }

    /* 根据负载长度编码帧头 */
    if (payload_len <= 125) {
        header[1] = 0x80 | (uint8_t)payload_len;
        header_len = 2;
    } else if (payload_len <= 65535) {
        header[1] = 0x80 | 126;
        header[2] = (payload_len >> 8) & 0xFF;
        header[3] = payload_len & 0xFF;
        header_len = 4;
    } else {
        header[1] = 0x80 | 127;
        for (int i = 0; i < 8; i++) {
            header[2 + i] = (payload_len >> (56 - i * 8)) & 0xFF;
        }
        header_len = 10;
    }

    /* 将掩码密钥追加到帧头 */
    memcpy(header + header_len, mask_key, 4);
    header_len += 4;

    /* 发送帧头 */
    int ret = ws_send_raw(ws, header, header_len);
    if (ret < 0) {
        pthread_mutex_unlock(&ws->send_mutex);
        return -1;
    }

    /* 分块发送掩码后的负载数据 */
    if (payload_len > 0 && payload) {
        uint8_t masked[4096];
        size_t offset = 0;
        while (offset < payload_len) {
            size_t chunk = payload_len - offset;
            if (chunk > sizeof(masked)) chunk = sizeof(masked);
            for (size_t i = 0; i < chunk; i++) {
                masked[i] = payload[offset + i] ^ mask_key[(offset + i) % 4];
            }
            ret = ws_send_raw(ws, masked, chunk);
            if (ret < 0) {
                pthread_mutex_unlock(&ws->send_mutex);
                return -1;
            }
            offset += chunk;
        }
    }

    pthread_mutex_unlock(&ws->send_mutex);
    return 0;
}

/**
 * @brief 处理接收到的WebSocket帧
 * @param ws WebSocket实例指针
 * @param opcode 帧操作码
 * @param payload 帧负载数据
 * @param len 负载长度
 */
static void ws_handle_frame(websocket_t *ws, int opcode, const uint8_t *payload, size_t len) {
    switch (opcode) {
    case WS_OPCODE_TEXT:
        ws->last_data_ms = ws_get_time_ms();
        if (ws->callbacks.on_data) {
            ws->callbacks.on_data(ws->callbacks.user_data, (const char*)payload, len, false);
        }
        break;
    case WS_OPCODE_BINARY:
        ws->last_data_ms = ws_get_time_ms();
        if (ws->callbacks.on_data) {
            ws->callbacks.on_data(ws->callbacks.user_data, (const char*)payload, len, true);
        }
        break;
    case WS_OPCODE_PING:
        PLOG_D("WS", "收到Ping帧，发送Pong响应");
        ws_send_frame(ws, WS_OPCODE_PONG, payload, len);
        ws->last_data_ms = ws_get_time_ms();
        break;
    case WS_OPCODE_PONG:
        PLOG_D("WS", "收到Pong帧");
        ws->last_data_ms = ws_get_time_ms();
        break;
    case WS_OPCODE_CLOSE:
        PLOG_I("WS", "收到关闭帧");
        ws->connected = false;
        if (ws->callbacks.on_disconnected) {
            ws->callbacks.on_disconnected(ws->callbacks.user_data);
        }
        break;
    }
}

/**
 * @brief 处理接收缓冲区中的数据，解析出完整的WebSocket帧
 * @param ws WebSocket实例指针
 * @return 0成功，-1失败
 */
static int ws_process_recv_buffer(websocket_t *ws) {
    uint8_t *buf = ws->recv_buf;
    size_t buf_len = ws->recv_buf_len;
    size_t consumed = 0;

    while (consumed < buf_len) {
        if (buf_len - consumed < 2) break;

        /* 解析帧头基本信息 */
        int opcode = buf[consumed] & 0x0F;
        bool masked = (buf[consumed + 1] & 0x80) != 0;
        uint64_t payload_len = buf[consumed + 1] & 0x7F;

        size_t header_size = 2;

        /* 解析扩展负载长度 */
        if (payload_len == 126) {
            if (buf_len - consumed < 4) break;
            payload_len = ((uint16_t)buf[consumed + 2] << 8) | buf[consumed + 3];
            header_size = 4;
        } else if (payload_len == 127) {
            if (buf_len - consumed < 10) break;
            payload_len = 0;
            for (int i = 0; i < 8; i++) {
                payload_len = (payload_len << 8) | buf[consumed + 2 + i];
            }
            header_size = 10;
        }

        /* 解析掩码密钥 */
        uint8_t mask_key[4] = {0};
        if (masked) {
            if (buf_len - consumed < header_size + 4) break;
            memcpy(mask_key, buf + consumed + header_size, 4);
            header_size += 4;
        }

        /* 检查是否已接收完整的帧数据 */
        if (consumed + header_size + payload_len > buf_len) break;

        /* 如果帧被掩码，则进行解码 */
        uint8_t *payload = buf + consumed + header_size;
        if (masked) {
            for (uint64_t i = 0; i < payload_len; i++) {
                payload[i] ^= mask_key[i % 4];
            }
        }

        /* 处理解析出的帧 */
        ws_handle_frame(ws, opcode, payload, (size_t)payload_len);
        consumed += header_size + payload_len;
    }

    /* 移动缓冲区中未消费的数据到头部 */
    if (consumed > 0 && consumed < ws->recv_buf_len) {
        memmove(ws->recv_buf, ws->recv_buf + consumed, ws->recv_buf_len - consumed);
    }
    ws->recv_buf_len -= consumed;

    return 0;
}

/**
 * @brief 初始化WebSocket实例
 * @param ws WebSocket实例指针
 */
void websocket_init(websocket_t *ws) {
    if (!ws) return;
    memset(ws, 0, sizeof(websocket_t));
    ws->sockfd = -1;
    pthread_mutex_init(&ws->send_mutex, NULL);
}

/**
 * @brief 销毁WebSocket实例，释放资源
 * @param ws WebSocket实例指针
 */
void websocket_destroy(websocket_t *ws) {
    if (!ws) return;
    websocket_disconnect(ws);
    pthread_mutex_destroy(&ws->send_mutex);
}

/**
 * @brief 设置WebSocket连接URL
 * @param ws WebSocket实例指针
 * @param url WebSocket服务器地址（ws://或wss://）
 */
void websocket_set_url(websocket_t *ws, const char *url) {
    if (!ws || !url) return;
    strncpy(ws->url, url, sizeof(ws->url) - 1);
    ws->header_count = 0;
    ws_parse_url(ws);
}

/**
 * @brief 添加自定义HTTP头部（用于握手请求）
 * @param ws WebSocket实例指针
 * @param name 头部名称
 * @param value 头部值
 */
void websocket_set_header(websocket_t *ws, const char *name, const char *value) {
    if (!ws || ws->header_count >= WS_MAX_HEADERS) return;
    strncpy(ws->headers[ws->header_count].name, name, WS_MAX_HEADER_NAME_LEN - 1);
    strncpy(ws->headers[ws->header_count].value, value, WS_MAX_HEADER_VALUE_LEN - 1);
    ws->header_count++;
}

/**
 * @brief 设置WebSocket事件回调函数
 * @param ws WebSocket实例指针
 * @param on_data 数据接收回调
 * @param on_connected 连接成功回调
 * @param on_disconnected 断开连接回调
 * @param on_error 错误回调
 * @param user_data 传递给回调的用户数据
 */
void websocket_set_callbacks(websocket_t *ws,
                             websocket_data_callback_t on_data,
                             websocket_connected_callback_t on_connected,
                             websocket_disconnected_callback_t on_disconnected,
                             websocket_error_callback_t on_error,
                             void *user_data) {
    if (!ws) return;
    ws->callbacks.on_data = on_data;
    ws->callbacks.on_connected = on_connected;
    ws->callbacks.on_disconnected = on_disconnected;
    ws->callbacks.on_error = on_error;
    ws->callbacks.user_data = user_data;
}

/**
 * @brief 建立WebSocket连接
 * @param ws WebSocket实例指针
 * @return true连接成功，false连接失败
 * @note 支持TLS(wss)和非TLS(ws)两种连接方式
 */
bool websocket_connect(websocket_t *ws) {
    if (!ws) return false;

    if (ws->secure) {
        /* TLS加密连接 */
        if (tls_transport_init(&ws->tls) != 0) {
            snprintf(ws->error, sizeof(ws->error), "TLS init failed");
            return false;
        }

        if (tls_transport_connect(&ws->tls, ws->host, ws->port) != 0) {
            snprintf(ws->error, sizeof(ws->error), "TLS connect failed: %s", tls_transport_get_error(&ws->tls));
            tls_transport_destroy(&ws->tls);
            return false;
        }

        ws->sockfd = ws->tls.sockfd;
    } else {
        /* 普通TCP连接 */
        ws->sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (ws->sockfd < 0) {
            snprintf(ws->error, sizeof(ws->error), "Socket create failed");
            return false;
        }

        /* DNS解析 */
        struct hostent *he = gethostbyname(ws->host);
        if (!he) {
            snprintf(ws->error, sizeof(ws->error), "DNS resolve failed");
            close(ws->sockfd);
            ws->sockfd = -1;
            return false;
        }

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(ws->port);
        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

        if (connect(ws->sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            snprintf(ws->error, sizeof(ws->error), "TCP connect failed");
            close(ws->sockfd);
            ws->sockfd = -1;
            return false;
        }
    }

    /* 发送WebSocket握手请求 */
    if (!ws_send_handshake(ws)) {
        if (ws->secure) {
            tls_transport_disconnect(&ws->tls);
            tls_transport_destroy(&ws->tls);
        } else {
            close(ws->sockfd);
        }
        ws->sockfd = -1;
        return false;
    }

    /* 接收并验证握手响应 */
    if (!ws_recv_handshake_response(ws)) {
        if (ws->secure) {
            tls_transport_disconnect(&ws->tls);
            tls_transport_destroy(&ws->tls);
        } else {
            close(ws->sockfd);
        }
        ws->sockfd = -1;
        return false;
    }

    ws->connected = true;
    ws->recv_buf_len = 0;
    ws->last_ping_ms = ws_get_time_ms();
    ws->last_data_ms = ws_get_time_ms();

    PLOG_I("WS", "已连接到 %s:%d", ws->host, ws->port);

    if (ws->callbacks.on_connected) {
        ws->callbacks.on_connected(ws->callbacks.user_data);
    }

    return true;
}

/**
 * @brief 断开WebSocket连接
 * @param ws WebSocket实例指针
 */
void websocket_disconnect(websocket_t *ws) {
    if (!ws) return;

    /* 发送关闭帧 */
    if (ws->connected) {
        uint8_t close_code[2] = {0x03, 0xE8};
        ws_send_frame(ws, WS_OPCODE_CLOSE, close_code, 2);
        ws->connected = false;
    }

    /* 关闭TLS或TCP连接 */
    if (ws->secure && ws->tls.ssl_initialized) {
        tls_transport_disconnect(&ws->tls);
        tls_transport_destroy(&ws->tls);
    }

    if (ws->sockfd >= 0 && !ws->secure) {
        close(ws->sockfd);
    }

    ws->sockfd = -1;
    ws->recv_buf_len = 0;

    PLOG_I("WS", "已断开连接");
}

/**
 * @brief 检查WebSocket是否已连接
 * @param ws WebSocket实例指针
 * @return true已连接，false未连接
 */
bool websocket_is_connected(websocket_t *ws) {
    if (!ws) return false;
    return ws->connected;
}

/**
 * @brief 发送文本数据
 * @param ws WebSocket实例指针
 * @param text 待发送的文本字符串
 * @return true发送成功，false发送失败
 */
bool websocket_send_text(websocket_t *ws, const char *text) {
    return ws_send_frame(ws, WS_OPCODE_TEXT, (const uint8_t*)text, strlen(text)) == 0;
}

/**
 * @brief 发送二进制数据
 * @param ws WebSocket实例指针
 * @param data 待发送的二进制数据
 * @param len 数据长度
 * @return true发送成功，false发送失败
 */
bool websocket_send_binary(websocket_t *ws, const uint8_t *data, size_t len) {
    return ws_send_frame(ws, WS_OPCODE_BINARY, data, len) == 0;
}

/**
 * @brief 通用发送接口，根据binary参数选择帧类型
 * @param ws WebSocket实例指针
 * @param data 待发送数据
 * @param len 数据长度
 * @param binary 是否为二进制帧
 * @return 0成功，-1失败
 */
int websocket_send(websocket_t *ws, const uint8_t *data, size_t len, bool binary) {
    return ws_send_frame(ws, binary ? WS_OPCODE_BINARY : WS_OPCODE_TEXT, data, len);
}

/**
 * @brief 轮询接收数据并处理心跳
 * @param ws WebSocket实例指针
 * @param timeout_ms 轮询超时时间（毫秒）
 * @return 1有数据到达，0超时，-1连接断开或出错
 * @note 自动处理心跳Ping和空闲超时检测
 */
int websocket_poll(websocket_t *ws, int timeout_ms) {
    if (!ws || !ws->connected) return -1;

    uint64_t now = ws_get_time_ms();
    /* 每25秒发送一次Ping保活 */
    if (now - ws->last_ping_ms > 25000) {
        PLOG_D("WS", "发送Ping帧");
        ws_send_frame(ws, WS_OPCODE_PING, NULL, 0);
        ws->last_ping_ms = now;
    }

    /* 60秒未收到数据则判定为空闲超时 */
    if (now - ws->last_data_ms > 60000) {
        PLOG_W("WS", "空闲超时（60秒未收到数据）");
        ws->connected = false;
        if (ws->callbacks.on_disconnected) {
            ws->callbacks.on_disconnected(ws->callbacks.user_data);
        }
        return -1;
    }

    int ret = ws_poll_raw(ws, timeout_ms);
    if (ret <= 0) return ret;

    /* 接收数据到缓冲区 */
    size_t avail = WS_RECV_BUF_SIZE - ws->recv_buf_len;
    if (avail == 0) {
        PLOG_W("WS", "接收缓冲区已满，丢弃数据");
        ws->recv_buf_len = 0;
        avail = WS_RECV_BUF_SIZE;
    }

    int n = ws_recv_raw(ws, ws->recv_buf + ws->recv_buf_len, avail);
    if (n < 0 && n != -2) {
        PLOG_W("WS", "接收错误，断开连接");
        ws->connected = false;
        if (ws->callbacks.on_disconnected) {
            ws->callbacks.on_disconnected(ws->callbacks.user_data);
        }
        return -1;
    }
    if (n > 0) {
        ws->recv_buf_len += n;
        ws_process_recv_buffer(ws);
    }

    return 1;
}

/**
 * @brief 主动发送Ping帧
 * @param ws WebSocket实例指针
 * @return true发送成功，false发送失败或未连接
 */
bool websocket_send_ping(websocket_t *ws) {
    if (!ws || !ws->connected) return false;
    ws_send_frame(ws, WS_OPCODE_PING, NULL, 0);
    ws->last_ping_ms = ws_get_time_ms();
    return true;
}

/**
 * @brief 获取最近的错误信息
 * @param ws WebSocket实例指针
 * @return 错误信息字符串
 */
const char* websocket_get_error(websocket_t *ws) {
    if (!ws) return "NULL websocket";
    return ws->error[0] ? ws->error : "No error";
}
