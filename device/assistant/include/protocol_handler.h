#ifndef PROTOCOL_HANDLER_H
#define PROTOCOL_HANDLER_H

#include "websocket.h"
#include "mqtt_client.h"
#include "udp_audio.h"
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

#define PROTO_MAX_SESSION_ID 128
#define PROTO_MAX_AUDIO_PAYLOAD 4800
#define PROTO_SEND_QUEUE_SIZE 32
#define PROTO_MAX_TIMESTAMPS 8

#define TRANSPORT_MODE_WEBSOCKET 0
#define TRANSPORT_MODE_MQTT_UDP  1

typedef struct {
    uint16_t version;
    uint16_t type;
    uint32_t reserved;
    uint32_t timestamp;
    uint32_t payload_size;
} __attribute__((packed)) binary_header_v2_t;

typedef struct {
    uint32_t timestamps[PROTO_MAX_TIMESTAMPS];
    int head;
    int count;
    pthread_mutex_t mutex;
} timestamp_queue_t;

typedef struct {
    int sample_rate;
    int frame_duration;
    uint32_t timestamp;
    size_t payload_size;
    uint8_t payload[PROTO_MAX_AUDIO_PAYLOAD];
} audio_packet_t;

typedef void (*proto_connected_cb_t)(void *user_data);
typedef void (*proto_disconnected_cb_t)(void *user_data);
typedef void (*proto_audio_cb_t)(audio_packet_t *packet, void *user_data);
typedef void (*proto_json_cb_t)(const char *json, size_t len, void *user_data);
typedef void (*proto_error_cb_t)(const char *message, void *user_data);

typedef struct {
    char url[512];
    char token[512];
    char device_id[64];
    char client_id[64];
    int sample_rate;
    int channels;
    int frame_duration;
    uint64_t ping_interval_ms;

    int transport_mode;
    char mqtt_host[128];
    int mqtt_port;
    char mqtt_client_id[256];
    char mqtt_username[512];
    char mqtt_password[512];
    int mqtt_keepalive;
    char mqtt_subscribe_topic[256];
    char mqtt_publish_topic[256];
} protocol_config_t;

typedef struct {
    websocket_t ws;
    mqtt_client_t mqtt;
    udp_audio_t udp;
    protocol_config_t config;

    char session_id[PROTO_MAX_SESSION_ID];
    int server_sample_rate;
    int server_frame_duration;
    int protocol_version;
    int transport_mode;
    int cloud_aec;       /* 云端AEC: 1=hello声明aec:true(需服务器支持), 0=本地NLMS */

    bool connected;
    bool hello_received;
    bool mcp_initialized;
    pthread_mutex_t mutex;
    pthread_cond_t hello_cond;

    proto_connected_cb_t on_connected;
    proto_disconnected_cb_t on_disconnected;
    proto_audio_cb_t on_audio;
    proto_json_cb_t on_json;
    proto_error_cb_t on_error;
    void *user_data;

    uint8_t send_queue[PROTO_SEND_QUEUE_SIZE][PROTO_MAX_AUDIO_PAYLOAD];
    size_t send_queue_len[PROTO_SEND_QUEUE_SIZE];
    int send_queue_head;
    int send_queue_tail;
    int send_queue_count;
    uint64_t send_total_frames;
    uint64_t send_dropped_frames;
    pthread_mutex_t send_queue_mutex;
    pthread_cond_t send_queue_cond;
    pthread_t send_thread;
    int send_thread_running;

    timestamp_queue_t ts_queue;
} protocol_handler_t;

int protocol_handler_init(protocol_handler_t *proto, protocol_config_t *config);
void protocol_handler_destroy(protocol_handler_t *proto);

void protocol_handler_set_callbacks(protocol_handler_t *proto,
                                    proto_connected_cb_t on_connected,
                                    proto_disconnected_cb_t on_disconnected,
                                    proto_audio_cb_t on_audio,
                                    proto_json_cb_t on_json,
                                    proto_error_cb_t on_error,
                                    void *user_data);

int protocol_handler_connect(protocol_handler_t *proto);
void protocol_handler_disconnect(protocol_handler_t *proto);
bool protocol_handler_is_connected(protocol_handler_t *proto);
int protocol_handler_poll(protocol_handler_t *proto, int timeout_ms);

int protocol_handler_send_audio(protocol_handler_t *proto, const uint8_t *opus_data, size_t opus_len);
int protocol_handler_send_start_listening(protocol_handler_t *proto, const char *mode);
int protocol_handler_send_stop_listening(protocol_handler_t *proto);
int protocol_handler_send_abort(protocol_handler_t *proto, const char *reason);
int protocol_handler_send_json(protocol_handler_t *proto, const char *json, size_t len);
void protocol_handler_clear_send_queue(protocol_handler_t *proto);

void timestamp_queue_init(timestamp_queue_t *q);
void timestamp_queue_destroy(timestamp_queue_t *q);
void timestamp_queue_push(timestamp_queue_t *q, uint32_t ts);
uint32_t timestamp_queue_pop(timestamp_queue_t *q);
void timestamp_queue_clear(timestamp_queue_t *q);

const char *proto_find_json_str(const char *json, size_t json_len, const char *key, char *out, int out_size);
int proto_find_json_int(const char *json, size_t json_len, const char *key);

#endif
