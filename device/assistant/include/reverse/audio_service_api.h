#ifndef AUDIO_SERVICE_API_ASSISTANT_H
#define AUDIO_SERVICE_API_ASSISTANT_H

#include <stdint.h>

#define AUDIO_TRACK_TYPE_DEFAULT      0
#define AUDIO_TRACK_TYPE_MUSIC        1
#define AUDIO_TRACK_TYPE_ALARM        2
#define AUDIO_TRACK_TYPE_NOTIFICATION 3
#define AUDIO_TRACK_TYPE_VOICE_CALL   4

typedef struct {
    int32_t* channel_data[6];
    int32_t  num_channels;
    int32_t  num_samples;
    int32_t  shift_bits;
    int32_t  reserved[3];
} audio_track_write_params_t;

void* audio_track_create(const char* app_name);
int   audio_track_init(void* handle, int channels, int sample_rate, int type);
int   audio_track_play(void* handle);
int   audio_track_stop(void* handle);
int   audio_track_flush(void* handle);
void  audio_track_delete(void* handle);
int   audio_track_write_data(void* handle, audio_track_write_params_t* params);
int   audio_track_set_volume(void* handle, int volume);

#endif
