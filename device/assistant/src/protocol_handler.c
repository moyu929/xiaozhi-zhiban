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
#include <sys/time.h>
#include <sched.h>

const char *proto_find_json_str(const char *json, size_t json_len, const char *key, char *out, int out_size)
{
    if (!out || out_size <= 0)
        return NULL;
    out[0] = '\0';
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

    int i = 0;
    while (p < json + json_len && *p != '"' && i < out_size - 1)
    {
        if (*p == '\\' && p + 1 < json + json_len)
        {
            p++;
            switch (*p)
            {
            case 'n': out[i++] = '\n'; break;
            case 'r': out[i++] = '\r'; break;
            case 't': out[i++] = '\t'; break;
            case '"': out[i++] = '"'; break;
            case '\\': out[i++] = '\\'; break;
            default: out[i++] = *p; break;
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

int proto_find_json_int(const char *json, size_t json_len, const char *key)
{
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\"", key);

    const char *p = memmem(json, json_len, search_key, strlen(search_key));
    if (!p)
        return -1;

    p += strlen(search_key);
    while (p < json + json_len && (*p == ' ' || *p == ':' || *p == '\t'))
        p++;
    if (p >= json + json_len || (*p < '0' || *p > '9'))
        return -1;

    return atoi(p);
}

static void handle_hello_message(protocol_handler_t *proto, const char *data, size_t len)
{
    pthread_mutex_lock(&proto->mutex);

    proto_find_json_str(data, len, "session_id", proto->session_id, sizeof(proto->session_id));

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
        proto->server_sample_rate = 24000;
        proto->server_frame_duration = 60;
    }

    if (proto->transport_mode == TRANSPORT_MODE_MQTT_UDP)
    {
        char udp_server[64] = {0};
        char key_hex[128] = {0};
        char nonce_hex[128] = {0};

        const char *udp_start = memmem(data, len, "\"udp\"", 5);
        if (udp_start)
        {
            size_t udp_len = len - (udp_start - data);
            proto_find_json_str(udp_start, udp_len, "server", udp_server, sizeof(udp_server));
            int udp_port = proto_find_json_int(udp_start, udp_len, "port");
            proto_find_json_str(udp_start, udp_len, "key", key_hex, sizeof(key_hex));
            proto_find_json_str(udp_start, udp_len, "nonce", nonce_hex, sizeof(nonce_hex));

            if (udp_server[0] && udp_port > 0 && key_hex[0] && nonce_hex[0])
            {
                PLOG_I("PROTO", "UDP参数: server=%s port=%d", udp_server, udp_port);
                if (udp_audio_connect(&proto->udp, udp_server, udp_port, key_hex, nonce_hex) == 0)
                {
                    PLOG_I("PROTO", "UDP音频通道已建立");
                }
                else
                {
                    PLOG_E("PROTO", "UDP音频通道建立失败");
                }
            }
        }
    }

    proto->hello_received = true;
    PLOG_I("PROTO", "收到hello: 会话=%s 采样率=%d 帧时长=%d",
           proto->session_id, proto->server_sample_rate, proto->server_frame_duration);

    pthread_cond_signal(&proto->hello_cond);
    pthread_mutex_unlock(&proto->mutex);
}

static void on_ws_data(void *user_data, const char *data, size_t len, bool binary)
{
    protocol_handler_t *proto = (protocol_handler_t *)user_data;
    if (!proto)
        return;

    if (binary)
    {
        if (len < sizeof(binary_header_v2_t))
        {
            PLOG_W("PROTO", "帧过短: len=%d < %zu, 丢弃",
                   (int)len, sizeof(binary_header_v2_t));
            return;
        }

        binary_header_v2_t header;
        memcpy(&header, data, sizeof(header));

        uint16_t version = ntohs(header.version);
        uint16_t type = ntohs(header.type);
        uint32_t ts = ntohl(header.timestamp);
        uint32_t psize = ntohl(header.payload_size);

        if (psize == 0 || sizeof(binary_header_v2_t) + psize != len)
        {
            PLOG_W("PROTO", "帧解析失败: ver=%u type=%u psize=%u len=%d, 丢弃",
                   version, type, psize, (int)len);
            return;
        }

        if (type == 1)
        {
            if (proto->on_json)
                proto->on_json((const char *)(data + sizeof(binary_header_v2_t)), psize, proto->user_data);
            return;
        }

        audio_packet_t packet;
        memset(&packet, 0, sizeof(packet));
        packet.sample_rate = proto->server_sample_rate;
        packet.frame_duration = proto->server_frame_duration;
        packet.timestamp = ts;

        size_t copy_len = psize;
        if (copy_len > PROTO_MAX_AUDIO_PAYLOAD)
            copy_len = PROTO_MAX_AUDIO_PAYLOAD;
        memcpy(packet.payload, data + sizeof(binary_header_v2_t), copy_len);
        packet.payload_size = copy_len;

        if (proto->on_audio)
            proto->on_audio(&packet, proto->user_data);
    }
    else
    {
        char type_str[64] = {0};
        proto_find_json_str(data, len, "type", type_str, sizeof(type_str));

        if (strcmp(type_str, "hello") == 0)
            handle_hello_message(proto, data, len);

        if (proto->on_json)
            proto->on_json(data, len, proto->user_data);
    }
}

static void on_ws_connected(void *user_data)
{
    protocol_handler_t *proto = (protocol_handler_t *)user_data;
    if (!proto) return;
    PLOG_I("PROTO", "WebSocket已连接");
}

static void on_ws_disconnected(void *user_data)
{
    protocol_handler_t *proto = (protocol_handler_t *)user_data;
    if (!proto) return;
    PLOG_I("PROTO", "WebSocket已断开");
    proto->connected = false;
    pthread_mutex_lock(&proto->mutex);
    pthread_cond_signal(&proto->hello_cond);
    pthread_mutex_unlock(&proto->mutex);
    if (proto->on_disconnected)
        proto->on_disconnected(proto->user_data);
}

static void on_ws_error(void *user_data, const char *error)
{
    protocol_handler_t *proto = (protocol_handler_t *)user_data;
    if (!proto) return;
    PLOG_E("PROTO", "WebSocket错误: %s", error);
    if (proto->on_error)
        proto->on_error(error, proto->user_data);
}

static void on_udp_audio_recv(const uint8_t *data, size_t len, uint32_t timestamp, void *user_data)
{
    protocol_handler_t *proto = (protocol_handler_t *)user_data;
    if (!proto || !proto->on_audio) return;

    audio_packet_t packet;
    memset(&packet, 0, sizeof(packet));
    packet.sample_rate = proto->server_sample_rate;
    packet.frame_duration = proto->server_frame_duration;
    packet.timestamp = timestamp;
    size_t copy_len = len;
    if (copy_len > PROTO_MAX_AUDIO_PAYLOAD)
        copy_len = PROTO_MAX_AUDIO_PAYLOAD;
    memcpy(packet.payload, data, copy_len);
    packet.payload_size = copy_len;

    proto->on_audio(&packet, proto->user_data);
}

static void on_mqtt_message(const char *topic, const char *payload, size_t payload_len, void *user_data)
{
    protocol_handler_t *proto = (protocol_handler_t *)user_data;
    if (!proto || !payload || payload_len == 0) return;

    PLOG_D("PROTO", "MQTT收到: [%s] %.*s", topic ? topic : "null", (int)(payload_len > 500 ? 500 : payload_len), payload);

    char type_str[64] = {0};
    proto_find_json_str(payload, payload_len, "type", type_str, sizeof(type_str));

    if (strcmp(type_str, "mcp") == 0)
    {
        char method_str[128] = {0};
        const char *payload_key = memmem(payload, payload_len, "\"payload\"", 9);
        if (payload_key)
        {
            size_t inner_len = payload_len - (payload_key - payload);
            proto_find_json_str(payload_key, inner_len, "method", method_str, sizeof(method_str));
        }

        if (strcmp(method_str, "initialize") == 0)
        {
            int id_val = proto_find_json_int(payload, payload_len, "id");

            char resp[512];
            snprintf(resp, sizeof(resp),
                     "{\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":%d,"
                     "\"result\":{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{},"
                     "\"serverInfo\":{\"name\":\"xiaozhi-device\",\"version\":\"1.0.0\"}}}}",
                     id_val);

            if (proto->config.mqtt_publish_topic[0])
                mqtt_client_publish(&proto->mqtt, proto->config.mqtt_publish_topic, resp, strlen(resp));

            PLOG_I("PROTO", "MCP initialize 已回复 (id=%d)", id_val);
            proto->mcp_initialized = true;
        }
        else if (strcmp(method_str, "notifications/initialized") == 0)
        {
            PLOG_D("PROTO", "MCP notifications/initialized 收到");
        }
        else if (strcmp(method_str, "tools/list") == 0)
        {
            int id_val = proto_find_json_int(payload, payload_len, "id");

            char resp[256];
            snprintf(resp, sizeof(resp),
                     "{\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":%d,"
                     "\"result\":{\"tools\":[]}}}",
                     id_val);

            if (proto->config.mqtt_publish_topic[0])
                mqtt_client_publish(&proto->mqtt, proto->config.mqtt_publish_topic, resp, strlen(resp));

            PLOG_I("PROTO", "MCP tools/list 已回复 (id=%d)", id_val);
        }
        else
        {
            if (proto->on_json)
                proto->on_json(payload, payload_len, proto->user_data);
        }
        return;
    }

    if (strcmp(type_str, "hello") == 0)
        handle_hello_message(proto, payload, payload_len);

    if (strcmp(type_str, "error") == 0)
    {
        char err_msg[256] = {0};
        proto_find_json_str(payload, payload_len, "message", err_msg, sizeof(err_msg));
        PLOG_E("PROTO", "服务端返回error: %s (session=%s)", err_msg, proto->session_id);
    }

    if (strcmp(type_str, "goodbye") == 0)
    {
        PLOG_I("PROTO", "收到goodbye消息");
        proto->connected = false;
        pthread_mutex_lock(&proto->mutex);
        pthread_cond_signal(&proto->hello_cond);
        pthread_mutex_unlock(&proto->mutex);
        if (proto->on_disconnected)
            proto->on_disconnected(proto->user_data);
        return;
    }

    if (proto->on_json)
        proto->on_json(payload, payload_len, proto->user_data);
}

static void on_mqtt_disconnected(void *user_data)
{
    protocol_handler_t *proto = (protocol_handler_t *)user_data;
    if (!proto) return;
    PLOG_I("PROTO", "MQTT连接断开");
    proto->connected = false;
    pthread_mutex_lock(&proto->mutex);
    pthread_cond_signal(&proto->hello_cond);
    pthread_mutex_unlock(&proto->mutex);
    if (proto->on_disconnected)
        proto->on_disconnected(proto->user_data);
}

static void *send_thread_func(void *arg)
{
    protocol_handler_t *proto = (protocol_handler_t *)arg;
    prctl(PR_SET_NAME, "audio_send");

    PLOG_I("PROTO", "发送线程已启动 (模式=%s)", proto->transport_mode == TRANSPORT_MODE_MQTT_UDP ? "MQTT+UDP" : "WebSocket");

    while (proto->send_thread_running)
    {
        pthread_mutex_lock(&proto->send_queue_mutex);

        while (proto->send_queue_count == 0 && proto->send_thread_running)
        {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 100000000;
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

        uint8_t frame_data[PROTO_MAX_AUDIO_PAYLOAD];
        size_t frame_len = proto->send_queue_len[proto->send_queue_head];
        memcpy(frame_data, proto->send_queue[proto->send_queue_head], frame_len);

        proto->send_queue_head = (proto->send_queue_head + 1) % PROTO_SEND_QUEUE_SIZE;
        proto->send_queue_count--;

        pthread_mutex_unlock(&proto->send_queue_mutex);

        if (proto->transport_mode == TRANSPORT_MODE_MQTT_UDP)
        {
            if (!udp_audio_is_connected(&proto->udp))
            {
                proto->send_dropped_frames++;
                continue;
            }

            struct timeval tv;
            gettimeofday(&tv, NULL);
            uint32_t frame_ts = (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);

            if (udp_audio_send(&proto->udp, frame_data, frame_len, frame_ts) != 0)
            {
                PLOG_D("PROTO", "UDP发送失败");
                proto->send_dropped_frames++;
            }
        }
        else
        {
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
    }

    PLOG_I("PROTO", "发送线程已停止");
    return NULL;
}

int protocol_handler_init(protocol_handler_t *proto, protocol_config_t *config)
{
    if (!proto || !config)
        return -1;

    memset(proto, 0, sizeof(protocol_handler_t));
    memcpy(&proto->config, config, sizeof(protocol_config_t));

    proto->transport_mode = config->transport_mode;

    pthread_mutex_init(&proto->mutex, NULL);
    pthread_cond_init(&proto->hello_cond, NULL);
    pthread_mutex_init(&proto->send_queue_mutex, NULL);
    pthread_cond_init(&proto->send_queue_cond, NULL);
    timestamp_queue_init(&proto->ts_queue);

    if (proto->transport_mode == TRANSPORT_MODE_MQTT_UDP)
    {
        mqtt_client_init(&proto->mqtt);
        mqtt_client_set_callbacks(&proto->mqtt, on_mqtt_message, NULL, on_mqtt_disconnected, proto);

        udp_audio_init(&proto->udp);
        udp_audio_set_callbacks(&proto->udp, on_udp_audio_recv, proto);

        proto->protocol_version = 3;
    }
    else
    {
        websocket_init(&proto->ws);
        if (config->ping_interval_ms > 0)
            proto->ws.ping_interval_ms = config->ping_interval_ms;
        websocket_set_callbacks(&proto->ws, on_ws_data, on_ws_connected, on_ws_disconnected, on_ws_error, proto);

        /* 2026-08-27 定论(交叉矩阵实验): hello 不响应的真凶是 features."aec":true,
         * 与 version 无关(v1/v2 均通). 此处取 1 = xiaozhi-esp32 官方客户端主流形态. */
        proto->protocol_version = 2; /* v2: 带时间戳二进制协议, 云端AEC 依赖(官方同款) */
    }

    proto->server_sample_rate = 24000;
    proto->server_frame_duration = 60;

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

    PLOG_I("PROTO", "初始化完成 (模式=%s, protocol_version=%d, 采样率=%d, 帧时长=%d)",
           proto->transport_mode == TRANSPORT_MODE_MQTT_UDP ? "MQTT+UDP" : "WebSocket",
           proto->protocol_version, config->sample_rate, config->frame_duration);

    return 0;
}

void protocol_handler_destroy(protocol_handler_t *proto)
{
    if (!proto)
        return;

    protocol_handler_disconnect(proto);

    if (proto->send_thread)
    {
        proto->send_thread_running = 0;
        pthread_cond_signal(&proto->send_queue_cond);
        pthread_join(proto->send_thread, NULL);
        proto->send_thread = 0;
    }

    if (proto->transport_mode == TRANSPORT_MODE_MQTT_UDP)
    {
        mqtt_client_destroy(&proto->mqtt);
        udp_audio_destroy(&proto->udp);
    }
    else
    {
        websocket_destroy(&proto->ws);
    }

    timestamp_queue_destroy(&proto->ts_queue);

    pthread_mutex_destroy(&proto->mutex);
    pthread_cond_destroy(&proto->hello_cond);
    pthread_mutex_destroy(&proto->send_queue_mutex);
    pthread_cond_destroy(&proto->send_queue_cond);
}

void protocol_handler_set_callbacks(protocol_handler_t *proto,
                                    proto_connected_cb_t on_connected,
                                    proto_disconnected_cb_t on_disconnected,
                                    proto_audio_cb_t on_audio,
                                    proto_json_cb_t on_json,
                                    proto_error_cb_t on_error,
                                    void *user_data)
{
    if (!proto) return;
    proto->on_connected = on_connected;
    proto->on_disconnected = on_disconnected;
    proto->on_audio = on_audio;
    proto->on_json = on_json;
    proto->on_error = on_error;
    proto->user_data = user_data;
}

static int connect_websocket(protocol_handler_t *proto)
{
    websocket_set_url(&proto->ws, proto->config.url);
    websocket_set_header(&proto->ws, "Authorization", proto->config.token[0] ? proto->config.token : "Bearer ");
    char ver_str[8];
    snprintf(ver_str, sizeof(ver_str), "%d", proto->protocol_version);
    websocket_set_header(&proto->ws, "Protocol-Version", ver_str);
    websocket_set_header(&proto->ws, "Device-Id", proto->config.device_id);
    websocket_set_header(&proto->ws, "Client-Id", proto->config.client_id);

    PLOG_I("PROTO", "正在连接到 %s (Protocol-Version: %d)", proto->config.url, proto->protocol_version);

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

    if (!websocket_connect(&proto->ws))
    {
        PLOG_E("PROTO", "WebSocket连接失败: %s", websocket_get_error(&proto->ws));
        return -1;
    }

    char hello_json[1024];
    /* 2026-08-27 深夜实测终论: 设备侧(真实客户端指纹)发 features.aec:true + v2
     * 同样被服务器 10s 无响应拒绝(23:34 实录, 连接超时进 Cleaning). 云端 AEC
     * 在官方代码为编译期选项(CONFIG_USE_SERVER_AEC), 说明服务端具备该能力但
     * 当前网关不开放. features 不声明 aec 即为本服务器唯一可行形态;
     * 上行仍保留 BinaryProtocol2 + timestamp_queue 配对链路(官方云端 AEC 同款
     * 机制, 若未来服务器开放只需在 features 加回 "aec":true 即可启用). */
    /* 云端AEC(aec_mode=cloud): 官方 CONFIG_USE_SERVER_AEC 同款声明, 服务器以
     * BinaryProtocol2 上行时间戳配对做源信号对消(设备端 timestamp_queue 已就位).
     * 2026-08-27 实测当前网关拒绝 aec:true(10s 无响应), 故默认 local 不声明;
     * 面板可切 cloud, 待服务器开放即用. */
    const char *feat = proto->cloud_aec ? "\"mcp\":true,\"aec\":true" : "\"mcp\":true";
    snprintf(hello_json, sizeof(hello_json),
             "{\"type\":\"hello\",\"version\":%d,\"transport\":\"websocket\","
             "\"features\":{%s},"
             "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":%d,\"channels\":%d,\"frame_duration\":%d}}",
             proto->protocol_version, feat,
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

static int connect_mqtt_udp(protocol_handler_t *proto)
{
    PLOG_I("PROTO", "正在连接MQTT %s:%d", proto->config.mqtt_host, proto->config.mqtt_port);

    strncpy(proto->mqtt.host, proto->config.mqtt_host, sizeof(proto->mqtt.host) - 1);
    proto->mqtt.port = proto->config.mqtt_port;
    strncpy(proto->mqtt.client_id, proto->config.mqtt_client_id, sizeof(proto->mqtt.client_id) - 1);
    strncpy(proto->mqtt.username, proto->config.mqtt_username, sizeof(proto->mqtt.username) - 1);
    strncpy(proto->mqtt.password, proto->config.mqtt_password, sizeof(proto->mqtt.password) - 1);
    proto->mqtt.keepalive = proto->config.mqtt_keepalive;
    strncpy(proto->mqtt.subscribe_topic, proto->config.mqtt_subscribe_topic, sizeof(proto->mqtt.subscribe_topic) - 1);

    if (mqtt_client_connect(&proto->mqtt) != 0)
    {
        PLOG_E("PROTO", "MQTT连接失败: %s", mqtt_client_get_error(&proto->mqtt));
        return -1;
    }

    PLOG_I("PROTO", "MQTT连接成功，立即发送hello");

    char hello_json[1024];
    snprintf(hello_json, sizeof(hello_json),
             "{\"type\":\"hello\",\"version\":3,\"transport\":\"udp\","
             "\"features\":{\"mcp\":true,\"aec\":false},"
             "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":%d,\"channels\":%d,\"frame_duration\":%d}}",
             proto->config.sample_rate,
             proto->config.channels,
             proto->config.frame_duration);

    PLOG_I("PROTO", "发送hello: %s", hello_json);

    if (mqtt_client_publish(&proto->mqtt, proto->config.mqtt_publish_topic, hello_json, strlen(hello_json)) != 0)
    {
        PLOG_E("PROTO", "发送hello失败");
        mqtt_client_disconnect(&proto->mqtt);
        return -1;
    }

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
    }

    proto->connected = true;
    return 0;
}

int protocol_handler_connect(protocol_handler_t *proto)
{
    if (!proto)
        return -1;

    proto->hello_received = false;
    proto->mcp_initialized = false;
    proto->session_id[0] = '\0';

    if (proto->transport_mode == TRANSPORT_MODE_MQTT_UDP)
        return connect_mqtt_udp(proto);
    else
        return connect_websocket(proto);
}

void protocol_handler_disconnect(protocol_handler_t *proto)
{
    if (!proto)
        return;

    if (proto->transport_mode == TRANSPORT_MODE_MQTT_UDP)
    {
        if (proto->connected && proto->session_id[0])
        {
            char goodbye[256];
            snprintf(goodbye, sizeof(goodbye),
                     "{\"session_id\":\"%s\",\"type\":\"goodbye\"}", proto->session_id);
            mqtt_client_publish(&proto->mqtt, proto->config.mqtt_publish_topic, goodbye, strlen(goodbye));
        }
        udp_audio_disconnect(&proto->udp);
        mqtt_client_disconnect(&proto->mqtt);
    }
    else
    {
        if (proto->connected || websocket_is_connected(&proto->ws))
            websocket_disconnect(&proto->ws);
    }

    proto->connected = false;
    proto->hello_received = false;
    proto->mcp_initialized = false;
    proto->session_id[0] = '\0';

    if (proto->send_thread_running)
    {
        proto->send_thread_running = 0;
        pthread_cond_signal(&proto->send_queue_cond);
        pthread_join(proto->send_thread, NULL);
        proto->send_thread = 0;
        PLOG_I("PROTO", "断开连接时发送线程已合并");
    }

    pthread_mutex_lock(&proto->send_queue_mutex);
    proto->send_queue_head = 0;
    proto->send_queue_tail = 0;
    proto->send_queue_count = 0;
    pthread_mutex_unlock(&proto->send_queue_mutex);
}

bool protocol_handler_is_connected(protocol_handler_t *proto)
{
    if (!proto) return false;
    return proto->connected && proto->hello_received;
}

int protocol_handler_poll(protocol_handler_t *proto, int timeout_ms)
{
    if (!proto || !proto->connected)
        return -1;

    if (proto->transport_mode == TRANSPORT_MODE_MQTT_UDP)
    {
        if (mqtt_client_is_connected(&proto->mqtt))
            return 1;
        return -1;
    }
    else
    {
        return websocket_poll(&proto->ws, timeout_ms);
    }
}

int protocol_handler_send_audio(protocol_handler_t *proto, const uint8_t *opus_data, size_t opus_len)
{
    if (!proto || !opus_data || opus_len == 0)
        return -1;
    if (!proto->connected || !proto->hello_received)
        return -1;

    uint8_t frame_buf[PROTO_MAX_AUDIO_PAYLOAD];
    size_t frame_len = 0;

    if (proto->transport_mode == TRANSPORT_MODE_WEBSOCKET)
    {
        if (sizeof(binary_header_v2_t) + opus_len > sizeof(frame_buf))
            return -1;
        binary_header_v2_t hdr;
        hdr.version = htons(2);
        hdr.type = htons(0);
        hdr.reserved = 0;
        hdr.timestamp = htonl(timestamp_queue_pop(&proto->ts_queue));
        hdr.payload_size = htonl((uint32_t)opus_len);
        memcpy(frame_buf, &hdr, sizeof(hdr));
        memcpy(frame_buf + sizeof(hdr), opus_data, opus_len);
        frame_len = sizeof(hdr) + opus_len;
    }
    else
    {
        if (opus_len > sizeof(frame_buf))
            return -1;
        memcpy(frame_buf, opus_data, opus_len);
        frame_len = opus_len;
    }

    if (frame_len > sizeof(proto->send_queue[0]))
        return -1;

    pthread_mutex_lock(&proto->send_queue_mutex);

    proto->send_total_frames++;

    if (proto->send_queue_count >= PROTO_SEND_QUEUE_SIZE)
    {
        proto->send_queue_head = (proto->send_queue_head + 1) % PROTO_SEND_QUEUE_SIZE;
        proto->send_queue_count--;
        proto->send_dropped_frames++;
    }

    int idx = proto->send_queue_tail;
    memcpy(proto->send_queue[idx], frame_buf, frame_len);
    proto->send_queue_len[idx] = frame_len;

    proto->send_queue_tail = (proto->send_queue_tail + 1) % PROTO_SEND_QUEUE_SIZE;
    proto->send_queue_count++;

    pthread_cond_signal(&proto->send_queue_cond);
    pthread_mutex_unlock(&proto->send_queue_mutex);

    return 0;
}

int protocol_handler_send_start_listening(protocol_handler_t *proto, const char *mode)
{
    if (!proto || !proto->connected)
        return -1;

    char json[512];
    snprintf(json, sizeof(json),
             "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"start\",\"mode\":\"%s\"}",
             proto->session_id, mode ? mode : "auto");

    PLOG_I("PROTO", "发送开始监听: 模式=%s json=%s", mode ? mode : "auto", json);

    if (proto->transport_mode == TRANSPORT_MODE_MQTT_UDP)
        return mqtt_client_publish(&proto->mqtt, proto->config.mqtt_publish_topic, json, strlen(json));
    else
        return websocket_send_text(&proto->ws, json) ? 0 : -1;
}

int protocol_handler_send_stop_listening(protocol_handler_t *proto)
{
    if (!proto || !proto->connected)
        return -1;

    char json[512];
    snprintf(json, sizeof(json),
             "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"stop\"}",
             proto->session_id);

    PLOG_I("PROTO", "发送停止监听");

    if (proto->transport_mode == TRANSPORT_MODE_MQTT_UDP)
        return mqtt_client_publish(&proto->mqtt, proto->config.mqtt_publish_topic, json, strlen(json));
    else
        return websocket_send_text(&proto->ws, json) ? 0 : -1;
}

int protocol_handler_send_abort(protocol_handler_t *proto, const char *reason)
{
    if (!proto || !proto->connected)
        return -1;

    char json[512];
    if (reason)
        snprintf(json, sizeof(json),
                 "{\"session_id\":\"%s\",\"type\":\"abort\",\"reason\":\"%s\"}",
                 proto->session_id, reason);
    else
        snprintf(json, sizeof(json),
                 "{\"session_id\":\"%s\",\"type\":\"abort\"}",
                 proto->session_id);

    PLOG_I("PROTO", "发送中止: 原因=%s", reason ? reason : "无");

    if (proto->transport_mode == TRANSPORT_MODE_MQTT_UDP)
        return mqtt_client_publish(&proto->mqtt, proto->config.mqtt_publish_topic, json, strlen(json));
    else
        return websocket_send_text(&proto->ws, json) ? 0 : -1;
}

int protocol_handler_send_json(protocol_handler_t *proto, const char *json, size_t len)
{
    if (!proto || !json || len == 0)
        return -1;
    if (!proto->connected || !proto->hello_received)
        return -1;

    PLOG_D("PROTO", "发送JSON: %.*s", (int)(len > 200 ? 200 : len), json);

    if (proto->transport_mode == TRANSPORT_MODE_MQTT_UDP)
        return mqtt_client_publish(&proto->mqtt, proto->config.mqtt_publish_topic, json, len);
    else
        return websocket_send_text(&proto->ws, json) ? 0 : -1;
}

void protocol_handler_clear_send_queue(protocol_handler_t *proto)
{
    if (!proto) return;
    pthread_mutex_lock(&proto->send_queue_mutex);
    proto->send_queue_head = 0;
    proto->send_queue_tail = 0;
    proto->send_queue_count = 0;
    pthread_mutex_unlock(&proto->send_queue_mutex);
    PLOG_I("PROTO", "发送队列已清空 (总计=%llu 丢弃=%llu 丢弃率=%.1f%%)",
           (unsigned long long)proto->send_total_frames,
           (unsigned long long)proto->send_dropped_frames,
           proto->send_total_frames > 0 ? (float)proto->send_dropped_frames * 100 / proto->send_total_frames : 0);
}

void timestamp_queue_init(timestamp_queue_t *q)
{
    if (!q) return;
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->mutex, NULL);
}

void timestamp_queue_destroy(timestamp_queue_t *q)
{
    if (!q) return;
    pthread_mutex_destroy(&q->mutex);
}

void timestamp_queue_push(timestamp_queue_t *q, uint32_t ts)
{
    if (!q) return;
    pthread_mutex_lock(&q->mutex);
    if (q->count >= PROTO_MAX_TIMESTAMPS)
    {
        q->head = (q->head + 1) % PROTO_MAX_TIMESTAMPS;
        q->count--;
    }
    int idx = (q->head + q->count) % PROTO_MAX_TIMESTAMPS;
    q->timestamps[idx] = ts;
    q->count++;
    pthread_mutex_unlock(&q->mutex);
}

uint32_t timestamp_queue_pop(timestamp_queue_t *q)
{
    if (!q) return 0;
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
    if (!q) return;
    pthread_mutex_lock(&q->mutex);
    q->head = 0;
    q->count = 0;
    pthread_mutex_unlock(&q->mutex);
}
