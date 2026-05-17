#ifndef WEBSOCKET_H
#define WEBSOCKET_H

#include "tls_transport.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>

#define WS_MAX_URL_LEN 512
#define WS_MAX_HEADER_NAME_LEN 64
#define WS_MAX_HEADER_VALUE_LEN 256
#define WS_MAX_HEADERS 16
#define WS_MAX_ERROR_LEN 256
#define WS_RECV_BUF_SIZE 16384

typedef void (*websocket_data_callback_t)(void *user_data, const char *data, size_t len, bool binary);
typedef void (*websocket_connected_callback_t)(void *user_data);
typedef void (*websocket_disconnected_callback_t)(void *user_data);
typedef void (*websocket_error_callback_t)(void *user_data, const char *error);

typedef struct {
    websocket_data_callback_t on_data;
    websocket_connected_callback_t on_connected;
    websocket_disconnected_callback_t on_disconnected;
    websocket_error_callback_t on_error;
    void *user_data;
} websocket_callbacks_t;

typedef struct {
    char url[WS_MAX_URL_LEN];
    char host[128];
    char path[256];
    int port;
    bool secure;

    struct {
        char name[WS_MAX_HEADER_NAME_LEN];
        char value[WS_MAX_HEADER_VALUE_LEN];
    } headers[WS_MAX_HEADERS];
    int header_count;

    int sockfd;
    tls_transport_t tls;
    bool connected;
    char error[WS_MAX_ERROR_LEN];

    websocket_callbacks_t callbacks;

    uint8_t recv_buf[WS_RECV_BUF_SIZE];
    size_t recv_buf_len;

    pthread_mutex_t send_mutex;

    uint64_t last_ping_ms;
    uint64_t last_data_ms;
    uint64_t ping_interval_ms;
} websocket_t;

void websocket_init(websocket_t *ws);
void websocket_destroy(websocket_t *ws);
void websocket_set_url(websocket_t *ws, const char *url);
void websocket_set_header(websocket_t *ws, const char *name, const char *value);
void websocket_set_callbacks(websocket_t *ws,
                             websocket_data_callback_t on_data,
                             websocket_connected_callback_t on_connected,
                             websocket_disconnected_callback_t on_disconnected,
                             websocket_error_callback_t on_error,
                             void *user_data);
bool websocket_connect(websocket_t *ws);
void websocket_disconnect(websocket_t *ws);
bool websocket_is_connected(websocket_t *ws);
bool websocket_send_text(websocket_t *ws, const char *text);
bool websocket_send_binary(websocket_t *ws, const uint8_t *data, size_t len);
int websocket_send(websocket_t *ws, const uint8_t *data, size_t len, bool binary);
int websocket_poll(websocket_t *ws, int timeout_ms);
bool websocket_send_ping(websocket_t *ws);
const char* websocket_get_error(websocket_t *ws);

#endif
