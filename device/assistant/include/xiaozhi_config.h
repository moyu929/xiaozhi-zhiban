#ifndef XIAOZHI_ASSISTANT_CONFIG_H
#define XIAOZHI_ASSISTANT_CONFIG_H

#define XIAOZHI_VERSION "2.0.0"
#define XIAOZHI_CHIP_MODEL "gs705b"

#define ACTIVATION_CHECK_INTERVAL_MS  5000
#define ACTIVATION_RETRY_INTERVAL_MS  30000
#define HTTP_RESPONSE_TIMEOUT_MS      20000
#define WAKEUP_COOLDOWN_MS            3000
#define POST_CLEANUP_COOLDOWN_MS      2000
#define TCP_CONNECT_TIMEOUT_MS        10000
#define TLS_HANDSHAKE_TIMEOUT_MS      15000
#define HELLO_TIMEOUT_MS              10000
#define LISTEN_TIMEOUT_MS             120000
#define SPEAK_TIMEOUT_MS              120000
#define SESSION_TIMEOUT_MS            300000
#define CLEANUP_TIMEOUT_MS            5000
#define WIFI_CHECK_INTERVAL_MS         5000
#define WATCHDOG_INTERVAL_MS          10000
#define MAIN_LOOP_TICK_MS             1000
#define WS_PING_INTERVAL_MS           25000
#define WS_IDLE_TIMEOUT_MS            60000

#define MSG_APP_QUIT             0x001
#define MSG_SAIR_ENABLE          0x23E
#define MSG_SAIR_DISABLE         0x23F
#define MSG_KEY_HOME             0x040
#define MSG_KEY_BACK             0x041

#define MSG_SAIR_OPEN            0x3E8
#define MSG_SAIR_CLOSE           0x3E9
#define MSG_SAIR_AI_START        0x3EB
#define MSG_SAIR_AI_STOP         0x3EC
#define MSG_SAIR_ASR_START       0x3ED
#define MSG_SAIR_ASR_STOP        0x3EE
#define MSG_SAIR_AEC_START       0x3EF
#define MSG_SAIR_AEC_STOP        0x3F0
#define MSG_SAIR_CHOOSE_AI       0x3F1
#define MSG_SAIR_REGISTER_CB     0x3F2
#define MSG_SAIR_STATUS_UPDATE   0x3F3
#define MSG_SAIR_GET_INFO        0x3F4
#define MSG_SAIR_POST_EVENT      0x3F5
#define MSG_SAIR_RECORD_REQUEST  0x3F6
#define MSG_SAIR_GET_EVENT       0x3F7

#define MSG_SAIR_AWAKE           0x235
#define MSG_SAIR_END             0x238
#define MSG_SAIR_AWAKE_CMD       0x239
#define MSG_SAIR_EMOTION         0x236

#define DEFAULT_OTA_URL          "https://api.tenclass.net/xiaozhi/ota/"
#define DEFAULT_WS_URL           "wss://api.tenclass.net/xiaozhi/v1/"
#define PLOG_PATH                "/var/upgrade/xiaozhi.log"

#define GOODIX_KEY_HOME    102
#define GOODIX_KEY_BACK     30
#define GOODIX_KEY_VOLUP   242
#define GOODIX_KEY_VOLDOWN 243

#define INJECT_KEY_VOLUP   115
#define INJECT_KEY_VOLDOWN 114

#define WIFI_STATE_CONNECTED 5
#define WIFI_INFO_SIZE       116

#define LOG_TAG "XIAOZHI"

#define LOG_E(tag, fmt, ...) fprintf(stderr, "[E][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define LOG_W(tag, fmt, ...) fprintf(stderr, "[W][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define LOG_I(tag, fmt, ...) fprintf(stdout, "[I][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define LOG_D(tag, fmt, ...) do {} while(0)

#endif
