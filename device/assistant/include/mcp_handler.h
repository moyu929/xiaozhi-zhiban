#ifndef MCP_HANDLER_H
#define MCP_HANDLER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef int (*mcp_send_json_cb_t)(const char *json, size_t len, void *user_data);

typedef struct {
    void *lib_handle;

    int (*sound_set_sys_volume)(int volume);
    int (*sound_get_sys_volume)(void);
    int (*sound_set_sys_mute)(int mute);
    int (*sound_is_sys_mute)(void);
    int (*power_get_charge_status)(int *out);
    int (*power_get_battery_cap)(void);
    int (*power_get_battery_voltage)(void);
    void (*sound_tts_play)(int index);

    mcp_send_json_cb_t send_json;
    void *user_data;
} mcp_handler_t;

int mcp_handler_init(mcp_handler_t *mcp);
void mcp_handler_destroy(mcp_handler_t *mcp);
void mcp_handler_set_send_cb(mcp_handler_t *mcp, mcp_send_json_cb_t send_cb, void *user_data);
void mcp_handler_process_message(mcp_handler_t *mcp, const char *json, size_t len);

#endif
