#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include "tls_transport.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define HTTP_MAX_URL_LEN 512
#define HTTP_MAX_HOST_LEN 128
#define HTTP_MAX_PATH_LEN 256
#define HTTP_MAX_HEADER_LEN 4096
#define HTTP_MAX_RESPONSE_LEN 8192

typedef struct {
    char host[HTTP_MAX_HOST_LEN];
    int port;
    char path[HTTP_MAX_PATH_LEN];
    bool use_ssl;
} http_url_t;

typedef struct {
    int status_code;
    char status_text[64];
    char headers[HTTP_MAX_HEADER_LEN];
    char *body;
    size_t body_len;
    size_t body_capacity;
} http_response_t;

int http_parse_url(const char *url, http_url_t *parsed);
int http_get(const char *url, const char *headers, http_response_t *response);
int http_post(const char *url, const char *headers, const char *body, http_response_t *response);
void http_response_free(http_response_t *response);

#endif
