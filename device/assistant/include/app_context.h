#ifndef APP_CONTEXT_H
#define APP_CONTEXT_H

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include "xiaozhi_config.h"
#include "reverse/applib_api.h"
#include "state_machine.h"
#include "watchdog.h"
#include "config_manager.h"
#include "wakeup_module.h"
#include "audio_dispatcher.h"
#include "touch_key.h"
#include "protocol_handler.h"
#include "audio_player.h"
#include "audio_recorder.h"
#include "mcp_handler.h"

typedef struct app_context_t {
    state_machine_t sm;
    watchdog_t watchdog;
    config_manager_t config;

    wakeup_module_t wakeup;
    audio_dispatcher_t audio_disp;
    touch_key_t touch_key;

    protocol_handler_t proto;
    audio_player_t player;
    audio_recorder_module_t recorder_mod;
    mcp_handler_t mcp;

    void* recorder_handle;
    int recorder_running;
    pthread_t recorder_thread;

    pthread_t ota_thread;
    volatile int ota_done;
    volatile int ota_config_received;

    pthread_t connect_thread;
    int connecting;
    int proto_initialized;
    int player_initialized;
    int recorder_initialized;
    int mcp_initialized;

    uint64_t session_start_ms;
    uint64_t state_enter_time_ms;
    uint64_t last_activation_check_ms;
    uint64_t last_activation_retry_ms;
    uint64_t last_cleaning_end_time_ms;
    uint64_t last_wakeup_ms;
    uint64_t wakeup_start_time_ms;
    uint64_t last_wifi_check_ms;
    uint64_t boot_time_ms;
    int listen_delayed;

    int in_session;
    int needs_activation;
    volatile int ignore_tts_audio;
    int wakeup_cooldown_done;
    volatile int pending_key_exit;
    volatile int pending_key_home;
    volatile int pending_wakeup;
    volatile int pending_wakeup_type;
    volatile int pending_cleaning_done;
    int running;
    char client_id[64];
    char device_mac[18];

    int self_pipe[2];
    int msg_pipe[2];
    volatile int msg_available;
    char msg_buf[MSG_SIZE];
    pthread_mutex_t msg_mutex;
    pthread_t msg_thread;
    int msg_thread_running;

    volatile int pending_api_wakeup;
    volatile int pending_api_abort;
    volatile int pending_api_activate;
    volatile int pending_api_config;
    char pending_config_buf[256];

    uint64_t listen_timeout_ms;
    uint64_t session_timeout_ms;
    uint64_t wakeup_cooldown_ms;
    uint64_t ws_ping_interval_ms;
} app_context_t;

#endif
