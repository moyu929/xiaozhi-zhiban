#ifndef TLS_TRANSPORT_H
#define TLS_TRANSPORT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define TLS_MAX_HOST_LEN 128
#define TLS_MAX_ERROR_LEN 256
#define TLS_DEFAULT_MAX_RETRIES 3

typedef struct {
    int attempt;
    int max_retries;
    int delay_seconds;
    int64_t next_retry_ms;
    bool in_progress;
} tls_retry_state_t;

typedef struct {
    int sockfd;
    char host[TLS_MAX_HOST_LEN];
    int port;
    bool connected;
    char error[TLS_MAX_ERROR_LEN];

    void *ssl_ctx;
    void *ssl_conf;
    void *entropy;
    void *ctr_drbg;
    void *net_ctx;
    bool ssl_initialized;
} tls_transport_t;

int tls_transport_init(tls_transport_t *tls);
void tls_transport_destroy(tls_transport_t *tls);
int tls_transport_connect(tls_transport_t *tls, const char *host, int port);
void tls_transport_disconnect(tls_transport_t *tls);
int tls_transport_write(tls_transport_t *tls, const uint8_t *data, size_t len);
int tls_transport_read(tls_transport_t *tls, uint8_t *data, size_t max_len);
int tls_transport_poll(tls_transport_t *tls, int timeout_ms);
bool tls_transport_is_connected(tls_transport_t *tls);
const char* tls_transport_get_error(tls_transport_t *tls);

void tls_retry_state_init(tls_retry_state_t *state, int max_retries);
bool tls_retry_should_retry(tls_retry_state_t *state);
bool tls_retry_is_time_to_retry(tls_retry_state_t *state);
void tls_retry_record_failure(tls_retry_state_t *state);
void tls_retry_reset(tls_retry_state_t *state);
int tls_retry_get_wait_ms(tls_retry_state_t *state);

#endif
