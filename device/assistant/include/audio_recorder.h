#ifndef AUDIO_RECORDER_MODULE_H
#define AUDIO_RECORDER_MODULE_H

#include "audio_dispatcher.h"
#include "protocol_handler.h"
#include "audio_precache.h"
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

#define RECORDER_RIGHT_CHANNEL_BUF_SIZE 960
#define RECORDER_OPUS_BUF_SIZE 1500

typedef struct {
    protocol_handler_t *proto;
    audio_dispatcher_t *disp;
    int disp_callback_id;

    int16_t right_channel_buf[RECORDER_RIGHT_CHANNEL_BUF_SIZE];
    int right_channel_count;

    void *opus_encoder;
    uint8_t opus_output_buf[RECORDER_OPUS_BUF_SIZE];

    int sample_rate;
    int channels;
    int frame_duration;
    int bitrate;

    volatile bool sending;
    audio_precache_t precache;
    pthread_mutex_t mutex;
} audio_recorder_module_t;

int audio_recorder_module_init(audio_recorder_module_t *rec, protocol_handler_t *proto, audio_dispatcher_t *disp);
int audio_recorder_module_early_init(audio_recorder_module_t *rec, audio_dispatcher_t *disp);
void audio_recorder_module_set_proto(audio_recorder_module_t *rec, protocol_handler_t *proto);
void audio_recorder_module_destroy(audio_recorder_module_t *rec);

int audio_recorder_module_start_sending(audio_recorder_module_t *rec);
int audio_recorder_module_stop_sending(audio_recorder_module_t *rec);
bool audio_recorder_module_is_sending(audio_recorder_module_t *rec);

#endif
