#ifndef SAIR_ASR_API_H
#define SAIR_ASR_API_H

#include <stdint.h>

#define ASR_EVENT_INIT_DONE     0
#define ASR_EVENT_WAKEUP        2
#define ASR_EVENT_DIFF_WORD     3
#define ASR_EVENT_VAD_CHANGE    4
#define ASR_EVENT_VAD_END       5
#define ASR_EVENT_VAD_TIMEOUT   6

typedef struct {
    int32_t sample_rate;
    int32_t channels;
    char product_id[64];
    char device_id[64];
    char main_wake_word[64];
    char main_wake_thresh[16];
    char custom_wake_word[64];
    char custom_wake_thresh[16];
    char local_command_word[64];
    char local_command_thresh[16];
    char aec_res_path[256];
    char wakeup_res_path[256];
    char uda_res_path[256];
    char vad_res_path[256];
    char major_config[256];
    char dcheck_config[256];
    int32_t malls_mode;
    int32_t reserved[10];
} asr_config_t;

typedef void (*asr_callback_t)(int event_type, int result);

typedef int  (*asr_set_params_t)(int param_type, int param_value, void* callback);
typedef void* (*asr_init_t)(asr_config_t* config);
typedef int  (*asr_finalize_t)(void* handle);
typedef int  (*asr_start_t)(void* handle);
typedef int  (*asr_stop_t)(void* handle);
typedef int  (*asr_feed_data_t)(void* handle, const void* data, int size);

void* asr_engine_init(asr_config_t* config);
int asr_engine_finalize(void* handle);
int asr_engine_start(void* handle);
int asr_engine_stop(void* handle);
int asr_engine_feed_data(void* handle, const void* data, int size);
int asr_engine_set_params(int param_type, int param_value, void* callback);

#endif
