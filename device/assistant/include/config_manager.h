#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <stdint.h>

typedef struct {
    char wake_word[256];
    char wake_thresh[64];
    char device_mac[18];
    char client_id[64];
    char ws_url[512];
    char ws_token[512];
    char activation_code[64];
    int has_ws_config;
    int needs_activation;
    int realtime_mode;
} config_manager_t;

int config_manager_init(config_manager_t* cfg);
void config_manager_destroy(config_manager_t* cfg);
int config_manager_check_wifi(void);
int config_manager_check_activation(config_manager_t* cfg);
int config_manager_get_mac(char* buf, int buf_size);
int config_manager_get_or_create_client_id(char* buf, int buf_size);
int config_manager_reload(config_manager_t* cfg);

#endif
