/**
 * @file protocol_handler.c
 * @brief 小智通信协议处理器实现
 *
 * 实现与服务器之间的通信协议，包括：
 * - WebSocket连接管理和数据收发
 * - 二进制音频帧的解析（v1/v2协议兼容）
 * - JSON消息的解析和分发
 * - 音频发送队列管理（独立发送线程）
 * - Hello握手和会话管理
 * - 时间戳队列（用于音频同步）
 */

#include "protocol_handler.h"
#include "plog.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/prctl.h>
#include <sched.h>

/**
 * @brief 从JSON数据中查找指定键的字符串值
 * @param json JSON数据指针
 * @param json_len JSON数据长度
 * @param key 要查找的键名
 * @param out 输出缓冲区
 * @param out_size 输出缓冲区大小
 * @return 找到返回输出缓冲区指针，未找到返回NULL
 */
static const char *find_json_string(const char *json, size_t json_len, const char *key, char *out, int out_size)
{
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\"", key);

    const char *p = memmem(json, json_len, search_key, strlen(search_key));
    if (!p)
        return NULL;

    p += strlen(search_key);
    while (p < json + json_len && (*p == ' ' || *p == ':' || *p == '\t'))
        p++;
    if (p >= json + json_len || *p != '"')
        return NULL;
    p++;

    /* 提取字符串值，处理转义字符 */
    int i = 0;
    while (p < json + json_len && *p != '"' && i < out_size - 1)
    {
        if (*p == '\\' && p + 1 < json + json_len)
        {
            p++;
            switch (*p)
            {
            case 'n':
                out[i++] = '\n';
                break;
            case 'r':
                out[i++] = '\r';
                break;
            case 't':
                out[i++] = '\t';
                break;
            case '"':
                out[i++] = '"';
                break;
            case '\\':
                out[i++] = '\\';
                break;
            default:
                out[i++] = *p;
                break;
            }
        }
        else
        {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return out;
}

/**
 * @brief WebSocket数据接收回调函数
 * @param user_data 用户数据（protocol_handler_t指针）
 * @param data 接收到的数据
 * @param len 数据长度
 * @param binary 是否为二进制数据
 */
static void on_ws_data(void *user_data, const char *data, size_t len, bool binary)
{
    protocol_handler_t *proto = (protocol_handler_t *)user_data;
    if (!proto)
        return;

    if (binary)
    {
        /* 处理二进制音频数据 */
        audio_packet_t packet;
        memset(&packet, 0, sizeof(packet));
        packet.sample_rate = proto->server_sample_rate;
        packet.frame_duration = proto->server_frame_duration;

        if (len >= sizeof(binary_header_v2_t))
        {
            /* 尝试解析v2协议头部 */
            binary_header_v2_t header;
            memcpy(&header, data, sizeof(header));

            uint16_t version = ntohs(header.version);
            uint16_t type = ntohs(header.type);
            uint32_t timestamp = ntohl(header.timestamp);
            uint32_t payload_size = ntohl(header.payload_size);

            /* 验证v2头部有效性 */
            if ((version == 2 || (version == 0 && type <= 1 && payload_size > 0 && payload_size + sizeof(binary_header_v2_t) == len)) && payload_size > 0 && payload_size < len)
            {
                PLOG_D("PROTO", "v2二进制帧: 类型=%d 时间戳=%u 大小=%u", type, timestamp, payload_size);

                size_t header_size = sizeof(binary_header_v2_t);
                if (header_size + payload_size > len)
                {
                    payload_size = len - header_size;
                }

                packet.timestamp = timestamp;
                packet.payload_size = payload_size;
                if (payload_size > PROTO_MAX_AUDIO_PAYLOAD)
                    payload_size = PROTO_MAX_AUDIO_PAYLOAD;
                memcpy(packet.payload, data + header_size, payload_size);
                packet.payload_size = payload_size;
            }
            else
            {
                /* v2头部无效，按v1协议处理（无头部） */
                PLOG_D("PROTO", "v1二进制帧: 长度=%zu (头部 版本=%d 类型=%d 负载大小=%u 不匹配)",
                       len, version, type, payload_size);

                size_t copy_len = len;
                if (copy_len > PROTO_MAX_AUDIO_PAYLOAD)
                    copy_len = PROTO_MAX_AUDIO_PAYLOAD;
                memcpy(packet.payload, data, copy_len);
                packet.payload_size = copy_len;
            }
        }
        else if (len > 0)
        {
            /* 短数据，按v1协议处理 */
            PLOG_D("PROTO", "v1二进制帧(短): 长度=%zu", len);

            size_t copy_len = len;
            if (copy_len > PROTO_MAX_AUDIO_PAYLOAD)
                copy_len = PROTO_MAX_AUDIO_PAYLOAD;
            memcpy(packet.payload, data, copy_len);
            packet.payload_size = copy_len;
        }
        else
        {
            return;
        }

        /* 回调上层处理音频数据 */
        if (proto->on_audio)
        {
            proto->on_audio(&packet, proto->user_data);
        }
    }
    else
    {
        /* 处理JSON文本消息 */
        PLOG_D("PROTO", "JSON消息: %.*s", (int)(len > 200 ? 200 : len), data);

        char type_str[64] = {0};
        find_json_string(data, len, "type", type_str, sizeof(type_str));

        /* 处理hello消息：获取会话ID和音频参数 */
        if (strcmp(type_str, "hello") == 0)
        {
            pthread_mutex_lock(&proto->mutex);

            find_json_string(data, len, "session_id", proto->session_id, sizeof(proto->session_id));

            /* 解析服务器音频参数 */
            const char *ap_start = memmem(data, len, "\"audio_params\"", 14);
            if (ap_start)
            {
                proto->server_sample_rate = 24000;
                proto->server_frame_duration = 60;

                const char *sr_search = memmem(ap_start, len - (ap_start - data), "\"sample_rate\"", 13);
                if (sr_search)
                {
                    const char *p = sr_search + 13;
                    while (p < data + len && (*p == ' ' || *p == ':' || *p == '\t'))
                        p++;
                    proto->server_sample_rate = atoi(p);
                }

                const char *fd_search = memmem(ap_start, len - (ap_start - data), "\"frame_duration\"", 16);
                if (fd_search)
                {
                    const char *p = fd_search + 16;
                    while (p < data + len && (*p == ' ' || *p == ':' || *p == '\t'))
                        p++;
                    proto->server_frame_duration = atoi(p);
                }
            }
            else
            {
                /* 无音频参数时使用默认值 */
                proto->server_sample_rate = 24000;
                proto->server_frame_duration = 60;
            }

            proto->hello_received = true;
            PLOG_I("PROTO", "收到hello: 会话=%s 采样率=%d 帧时长=%d",
                   proto->session_id, proto->server_sample_rate, proto->server_frame_duration);

            pthread_cond_signal(&proto->hello_cond);
            pthread_mutex_unlock(&proto->mutex);
        }

        /* 回调上层处理JSON消息 */
        if (proto->on_json)
        {
            proto->on_json(data, len, proto->user_data);
        }
    }
}

/**
 * @brief WebSocket连接成功回调
 * @param user_data 用户数据（protocol_handler_t指针）
 */
static void on_ws_connected(void *user_data)
{
    protocol_handler_t *proto = (protocol_handler_t *)user_data;
    if (!proto)
        return;
    PLOG_I("PROTO", "WebSocket已连接");
}

/**
 * @brief WebSocket断开连接回调
 * @param user_data 用户数据（protocol_handler_t指针）
 */
static void on_ws_disconnected(void *user_data)
{
    protocol_handler_t *proto = (protocol_handler_t *)user_data;
    if (!proto)
        return;

    PLOG_I("PROTO", "WebSocket已断开");
    proto->connected = false;

    /* 唤醒可能正在等待hello的线程 */
    pthread_mutex_lock(&proto->mutex);
    pthread_cond_signal(&proto->hello_cond);
    pthread_mutex_unlock(&proto->mutex);

    if (proto->on_disconnected)
    {
        proto->on_disconnected(proto->user_data);
    }
}

/**
 * @brief WebSocket错误回调
 * @param user_data 用户数据（protocol_handler_t指针）
 * @param error 错误信息
 */
static void on_ws_error(void *user_data, const char *error)
{
    protocol_handler_t *proto = (protocol_handler_t *)user_data;
    if (!proto)
        return;
    PLOG_E("PROTO", "WebSocket错误: %s", error);
    if (proto->on_error)
    {
        proto->on_error(error, proto->user_data);
    }
}

/**
 * @brief 音频发送线程函数
 * @param arg 线程参数（protocol_handler_t指针）
 * @return 线程返回值（始终为NULL）
 *
 * 从发送队列中取出音频帧并通过WebSocket发送，
 * 队列为空时等待条件变量通知
 */
static void *send_thread_func(void *arg)
{
    protocol_handler_t *proto = (protocol_handler_t *)arg;
    prctl(PR_SET_NAME, "audio_send");

    PLOG_I("PROTO", "发送线程已启动");

    while (proto->send_thread_running)
    {
        pthread_mutex_lock(&proto->send_queue_mutex);

        /* 等待队列中有数据或线程停止 */
        while (proto->send_queue_count == 0 && proto->send_thread_running)
        {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 10000000;
            if (ts.tv_nsec >= 1000000000)
            {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&proto->send_queue_cond, &proto->send_queue_mutex, &ts);
        }

        if (!proto->send_thread_running)
        {
            pthread_mutex_unlock(&proto->send_queue_mutex);
            break;
        }

        if (proto->send_queue_count == 0)
        {
            pthread_mutex_unlock(&proto->send_queue_mutex);
            continue;
        }

        /* 从队列头部取出一个音频帧 */
        uint8_t frame_data[PROTO_MAX_AUDIO_PAYLOAD + sizeof(binary_header_v2_t)];
        size_t frame_len = proto->send_queue_len[proto->send_queue_head];
        memcpy(frame_data, proto->send_queue[proto->send_queue_head], frame_len);

        proto->send_queue_head = (proto->send_queue_head + 1) % PROTO_SEND_QUEUE_SIZE;
        proto->send_queue_count--;

        pthread_mutex_unlock(&proto->send_queue_mutex);

        /* 检查WebSocket连接状态 */
        if (!websocket_is_connected(&proto->ws))
        {
            pthread_mutex_lock(&proto->send_queue_mutex);
            proto->send_queue_head = 0;
            proto->send_queue_tail = 0;
            proto->send_queue_count = 0;
            proto->send_thread_running = 0;
            pthread_mutex_unlock(&proto->send_queue_mutex);
            break;
        }

        /* 发送音频帧 */
        if (!websocket_send_binary(&proto->ws, frame_data, frame_len))
        {
            PLOG_D("PROTO", "发送音频帧失败，停止发送线程");
            pthread_mutex_lock(&proto->send_queue_mutex);
            proto->send_queue_head = 0;
            proto->send_queue_tail = 0;
            proto->send_queue_count = 0;
            proto->send_thread_running = 0;
            pthread_mutex_unlock(&proto->send_queue_mutex);
            break;
        }
    }

    PLOG_I("PROTO", "发送线程已停止");
    return NULL;
}

/**
 * @brief 初始化协议处理器
 * @param proto 协议处理器实例指针
 * @param config 协议配置参数
 * @return 0成功，-1失败
 */
int protocol_handler_init(protocol_handler_t *proto, protocol_config_t *config)
{
    if (!proto || !config)
        return -1;

    memset(proto, 0, sizeof(protocol_handler_t));
    memcpy(&proto->config, config, sizeof(protocol_config_t));

    pthread_mutex_init(&proto->mutex, NULL);
    pthread_cond_init(&proto->hello_cond, NULL);
    pthread_mutex_init(&proto->send_queue_mutex, NULL);
    pthread_cond_init(&proto->send_queue_cond, NULL);
    timestamp_queue_init(&proto->ts_queue);

    /* 初始化WebSocket并设置回调 */
    websocket_init(&proto->ws);
    if (config->ping_interval_ms > 0)
        proto->ws.ping_interval_ms = config->ping_interval_ms;
    websocket_set_callbacks(&proto->ws, on_ws_data, on_ws_connected, on_ws_disconnected, on_ws_error, proto);

    proto->protocol_version = 2;
    proto->server_sample_rate = 24000;
    proto->server_frame_duration = 60;

    /* 创建音频发送线程 */
    proto->send_thread_running = 1;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64 * 1024);
    pthread_create(&proto->send_thread, &attr, send_thread_func, proto);
    pthread_attr_destroy(&attr);

    {
        struct sched_param sp;
        sp.sched_priority = 10;
        if (pthread_setschedparam(proto->send_thread, SCHED_RR, &sp) == 0)
            PLOG_I("PROTO", "发送线程已设置 SCHED_RR 优先级 10");
    }

    PLOG_I("PROTO", "初始化完成 (采样率=%d, 帧时长=%d)",
           config->sample_rate, config->frame_duration);

    return 0;
}

/**
 * @brief 销毁协议处理器，释放所有资源
 * @param proto 协议处理器实例指针
 */
void protocol_handler_destroy(protocol_handler_t *proto)
{
    if (!proto)
        return;

    protocol_handler_disconnect(proto);

    /* 停止发送线程 */
    if (proto->send_thread)
    {
        proto->send_thread_running = 0;
        pthread_cond_signal(&proto->send_queue_cond);
        pthread_join(proto->send_thread, NULL);
        proto->send_thread = 0;
    }

    websocket_destroy(&proto->ws);

    timestamp_queue_destroy(&proto->ts_queue);

    pthread_mutex_destroy(&proto->mutex);
    pthread_cond_destroy(&proto->hello_cond);
    pthread_mutex_destroy(&proto->send_queue_mutex);
    pthread_cond_destroy(&proto->send_queue_cond);
}

/**
 * @brief 设置协议处理器的事件回调函数
 * @param proto 协议处理器实例指针
 * @param on_connected 连接成功回调
 * @param on_disconnected 断开连接回调
 * @param on_audio 音频数据回调
 * @param on_json JSON消息回调
 * @param on_error 错误回调
 * @param user_data 传递给回调的用户数据
 */
void protocol_handler_set_callbacks(protocol_handler_t *proto,
                                    proto_connected_cb_t on_connected,
                                    proto_disconnected_cb_t on_disconnected,
                                    proto_audio_cb_t on_audio,
                                    proto_json_cb_t on_json,
                                    proto_error_cb_t on_error,
                                    void *user_data)
{
    if (!proto)
        return;
    proto->on_connected = on_connected;
    proto->on_disconnected = on_disconnected;
    proto->on_audio = on_audio;
    proto->on_json = on_json;
    proto->on_error = on_error;
    proto->user_data = user_data;
}

/**
 * @brief 建立与服务器的协议连接
 * @param proto 协议处理器实例指针
 * @return 0成功，-1失败
 *
 * 连接流程：设置URL和头部 → WebSocket连接 → 发送hello消息
 */
int protocol_handler_connect(protocol_handler_t *proto)
{
    if (!proto)
        return -1;

    proto->hello_received = false;
    proto->session_id[0] = '\0';

    /* 设置WebSocket连接参数 */
    websocket_set_url(&proto->ws, proto->config.url);
    websocket_set_header(&proto->ws, "Authorization", proto->config.token[0] ? proto->config.token : "Bearer ");
    websocket_set_header(&proto->ws, "Protocol-Version", "2");
    websocket_set_header(&proto->ws, "Device-Id", proto->config.device_id);
    websocket_set_header(&proto->ws, "Client-Id", proto->config.client_id);

    PLOG_I("PROTO", "正在连接到 %s", proto->config.url);

    /* 如果发送线程未运行，则重新创建 */
    if (!proto->send_thread_running)
    {
        if (proto->send_thread)
        {
            pthread_join(proto->send_thread, NULL);
            proto->send_thread = 0;
        }
        proto->send_thread_running = 1;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 64 * 1024);
        pthread_create(&proto->send_thread, &attr, send_thread_func, proto);
        pthread_attr_destroy(&attr);
        {
            struct sched_param sp;
            sp.sched_priority = 10;
            pthread_setschedparam(proto->send_thread, SCHED_RR, &sp);
        }
        PLOG_I("PROTO", "发送线程已重启");
    }

    /* 建立WebSocket连接 */
    if (!websocket_connect(&proto->ws))
    {
        PLOG_E("PROTO", "WebSocket连接失败: %s", websocket_get_error(&proto->ws));
        return -1;
    }

    /* 构造并发送hello消息 */
    char hello_json[1024];
    snprintf(hello_json, sizeof(hello_json),
             "{\"type\":\"hello\",\"version\":2,\"transport\":\"websocket\","
             "\"features\":{\"mcp\":true,\"aec\":false},"
             "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":%d,\"channels\":%d,\"frame_duration\":%d}}",
             proto->config.sample_rate,
             proto->config.channels,
             proto->config.frame_duration);

    PLOG_I("PROTO", "发送hello: %s", hello_json);

    if (!websocket_send_text(&proto->ws, hello_json))
    {
        PLOG_E("PROTO", "发送hello失败");
        websocket_disconnect(&proto->ws);
        return -1;
    }

    proto->connected = true;

    return 0;
}

/**
 * @brief 断开与服务器的协议连接
 * @param proto 协议处理器实例指针
 */
void protocol_handler_disconnect(protocol_handler_t *proto)
{
    if (!proto)
        return;

    if (proto->connected || websocket_is_connected(&proto->ws))
    {
        websocket_disconnect(&proto->ws);
    }

    proto->connected = false;
    proto->hello_received = false;
    proto->session_id[0] = '\0';

    /* 停止发送线程 */
    if (proto->send_thread_running)
    {
        proto->send_thread_running = 0;
        pthread_cond_signal(&proto->send_queue_cond);
        pthread_join(proto->send_thread, NULL);
        proto->send_thread = 0;
        PLOG_I("PROTO", "断开连接时发送线程已合并");
    }

    /* 清空发送队列 */
    pthread_mutex_lock(&proto->send_queue_mutex);
    proto->send_queue_head = 0;
    proto->send_queue_tail = 0;
    proto->send_queue_count = 0;
    pthread_mutex_unlock(&proto->send_queue_mutex);
}

/**
 * @brief 检查协议连接是否就绪
 * @param proto 协议处理器实例指针
 * @return true已连接且收到hello，false未就绪
 */
bool protocol_handler_is_connected(protocol_handler_t *proto)
{
    if (!proto)
        return false;
    return proto->connected && proto->hello_received;
}

/**
 * @brief 轮询接收数据
 * @param proto 协议处理器实例指针
 * @param timeout_ms 超时时间（毫秒）
 * @return 1有数据，0超时，-1出错
 */
int protocol_handler_poll(protocol_handler_t *proto, int timeout_ms)
{
    if (!proto || !proto->connected)
        return -1;
    return websocket_poll(&proto->ws, timeout_ms);
}

/**
 * @brief 发送音频数据到服务器
 * @param proto 协议处理器实例指针
 * @param opus_data Opus编码的音频数据
 * @param opus_len 音频数据长度
 * @return 0成功，-1失败
 *
 * 将音频数据添加v2协议头部后放入发送队列，
 * 由独立的发送线程异步发送
 */
int protocol_handler_send_audio(protocol_handler_t *proto, const uint8_t *opus_data, size_t opus_len)
{
    if (!proto || !opus_data || opus_len == 0)
        return -1;
    if (!proto->connected || !proto->hello_received)
        return -1;

    /* 获取AEC时间戳 */
    uint32_t aec_timestamp = timestamp_queue_pop(&proto->ts_queue);

    /* 构造v2二进制协议头部 */
    binary_header_v2_t header;
    memset(&header, 0, sizeof(header));
    header.version = htons(2);
    header.type = htons(0);
    header.reserved = 0;
    header.timestamp = htonl(aec_timestamp);
    header.payload_size = htonl((uint32_t)opus_len);

    size_t frame_len = sizeof(binary_header_v2_t) + opus_len;
    if (frame_len > sizeof(proto->send_queue[0]))
    {
        PLOG_W("PROTO", "音频帧过大: %zu", frame_len);
        return -1;
    }

    pthread_mutex_lock(&proto->send_queue_mutex);

    proto->send_total_frames++;

    /* 队列满时丢弃最旧的帧 */
    if (proto->send_queue_count >= PROTO_SEND_QUEUE_SIZE)
    {
        proto->send_queue_head = (proto->send_queue_head + 1) % PROTO_SEND_QUEUE_SIZE;
        proto->send_queue_count--;
        proto->send_dropped_frames++;
        PLOG_W("PROTO", "发送队列已满 (总计=%llu 丢弃=%llu)，丢弃最旧帧",
               (unsigned long long)proto->send_total_frames,
               (unsigned long long)proto->send_dropped_frames);
    }

    /* 队列使用率超过3/4时输出警告 */
    if (proto->send_queue_count >= PROTO_SEND_QUEUE_SIZE * 3 / 4)
    {
        PLOG_D("PROTO", "发送队列高水位: %d/%d (总计=%llu 丢弃=%llu)",
               proto->send_queue_count, PROTO_SEND_QUEUE_SIZE,
               (unsigned long long)proto->send_total_frames,
               (unsigned long long)proto->send_dropped_frames);
    }

    /* 将帧数据（头部+音频）放入队列尾部 */
    int idx = proto->send_queue_tail;
    memcpy(proto->send_queue[idx], &header, sizeof(header));
    memcpy(proto->send_queue[idx] + sizeof(header), opus_data, opus_len);
    proto->send_queue_len[idx] = frame_len;

    proto->send_queue_tail = (proto->send_queue_tail + 1) % PROTO_SEND_QUEUE_SIZE;
    proto->send_queue_count++;

    pthread_cond_signal(&proto->send_queue_cond);
    pthread_mutex_unlock(&proto->send_queue_mutex);

    return 0;
}

/**
 * @brief 发送开始监听指令
 * @param proto 协议处理器实例指针
 * @param mode 监听模式（"auto"/"manual"等），NULL时默认"auto"
 * @return 0成功，-1失败
 */
int protocol_handler_send_start_listening(protocol_handler_t *proto, const char *mode)
{
    if (!proto || !proto->connected)
        return -1;

    char json[512];
    snprintf(json, sizeof(json),
             "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"start\",\"mode\":\"%s\"}",
             proto->session_id, mode ? mode : "auto");

    PLOG_I("PROTO", "发送开始监听: 模式=%s", mode ? mode : "auto");
    return websocket_send_text(&proto->ws, json) ? 0 : -1;
}

/**
 * @brief 发送停止监听指令
 * @param proto 协议处理器实例指针
 * @return 0成功，-1失败
 */
int protocol_handler_send_stop_listening(protocol_handler_t *proto)
{
    if (!proto || !proto->connected)
        return -1;

    char json[512];
    snprintf(json, sizeof(json),
             "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"stop\"}",
             proto->session_id);

    PLOG_I("PROTO", "发送停止监听");
    return websocket_send_text(&proto->ws, json) ? 0 : -1;
}

/**
 * @brief 发送中止指令
 * @param proto 协议处理器实例指针
 * @param reason 中止原因，NULL表示无原因
 * @return 0成功，-1失败
 */
int protocol_handler_send_abort(protocol_handler_t *proto, const char *reason)
{
    if (!proto || !proto->connected)
        return -1;

    char json[512];
    if (reason)
    {
        snprintf(json, sizeof(json),
                 "{\"session_id\":\"%s\",\"type\":\"abort\",\"reason\":\"%s\"}",
                 proto->session_id, reason);
    }
    else
    {
        snprintf(json, sizeof(json),
                 "{\"session_id\":\"%s\",\"type\":\"abort\"}",
                 proto->session_id);
    }

    PLOG_I("PROTO", "发送中止: 原因=%s", reason ? reason : "无");
    return websocket_send_text(&proto->ws, json) ? 0 : -1;
}

/**
 * @brief 发送自定义JSON消息
 * @param proto 协议处理器实例指针
 * @param json JSON字符串
 * @param len JSON字符串长度
 * @return 0成功，-1失败
 */
int protocol_handler_send_json(protocol_handler_t *proto, const char *json, size_t len)
{
    if (!proto || !json || len == 0)
        return -1;
    if (!proto->connected || !proto->hello_received)
        return -1;

    PLOG_D("PROTO", "发送JSON: %.*s", (int)(len > 200 ? 200 : len), json);
    return websocket_send_text(&proto->ws, json) ? 0 : -1;
}

/**
 * @brief 清空发送队列
 * @param proto 协议处理器实例指针
 */
void protocol_handler_clear_send_queue(protocol_handler_t *proto)
{
    if (!proto)
        return;
    pthread_mutex_lock(&proto->send_queue_mutex);
    proto->send_queue_head = 0;
    proto->send_queue_tail = 0;
    proto->send_queue_count = 0;
    pthread_mutex_unlock(&proto->send_queue_mutex);
    PLOG_I("PROTO", "发送队列已清空");
}

/**
 * @brief 初始化时间戳队列
 * @param q 时间戳队列指针
 */
void timestamp_queue_init(timestamp_queue_t *q)
{
    if (!q)
        return;
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->mutex, NULL);
}

/**
 * @brief 销毁时间戳队列
 * @param q 时间戳队列指针
 */
void timestamp_queue_destroy(timestamp_queue_t *q)
{
    if (!q)
        return;
    pthread_mutex_destroy(&q->mutex);
}

/**
 * @brief 向时间戳队列中推入一个时间戳
 * @param q 时间戳队列指针
 * @param ts 时间戳值
 */
void timestamp_queue_push(timestamp_queue_t *q, uint32_t ts)
{
    if (!q)
        return;
    pthread_mutex_lock(&q->mutex);

    if (q->count >= PROTO_MAX_TIMESTAMPS)
    {
        q->head = (q->head + 1) % PROTO_MAX_TIMESTAMPS;
        q->count--;
        PLOG_W("PROTO", "时间戳队列已满，丢弃最旧值");
    }

    int idx = (q->head + q->count) % PROTO_MAX_TIMESTAMPS;
    q->timestamps[idx] = ts;
    q->count++;

    pthread_mutex_unlock(&q->mutex);
}

/**
 * @brief 从时间戳队列中弹出一个时间戳
 * @param q 时间戳队列指针
 * @return 时间戳值，队列为空时返回0
 */
uint32_t timestamp_queue_pop(timestamp_queue_t *q)
{
    if (!q)
        return 0;
    pthread_mutex_lock(&q->mutex);

    uint32_t ts = 0;
    if (q->count > 0)
    {
        ts = q->timestamps[q->head];
        q->head = (q->head + 1) % PROTO_MAX_TIMESTAMPS;
        q->count--;
    }

    pthread_mutex_unlock(&q->mutex);
    return ts;
}

void timestamp_queue_clear(timestamp_queue_t *q)
{
    if (!q)
        return;
    pthread_mutex_lock(&q->mutex);
    q->head = 0;
    q->count = 0;
    pthread_mutex_unlock(&q->mutex);
}
