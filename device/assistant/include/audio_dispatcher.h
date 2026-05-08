#ifndef AUDIO_DISPATCHER_H
#define AUDIO_DISPATCHER_H

#include <stdint.h>
#include <stdatomic.h>

#define AUDIO_DISPATCHER_MAX_CALLBACKS 8

typedef void (*audio_data_callback_t)(const int16_t* data, int len, void* user_data);

typedef struct {
    audio_data_callback_t callbacks[AUDIO_DISPATCHER_MAX_CALLBACKS];
    void* user_datas[AUDIO_DISPATCHER_MAX_CALLBACKS];
    _Atomic int count;
} audio_dispatcher_t;

void audio_dispatcher_init(audio_dispatcher_t* disp);
void audio_dispatcher_dispatch(audio_dispatcher_t* disp, const int16_t* data, int len);
int audio_dispatcher_register(audio_dispatcher_t* disp, audio_data_callback_t cb, void* user_data);
void audio_dispatcher_unregister(audio_dispatcher_t* disp, audio_data_callback_t cb);

#endif
