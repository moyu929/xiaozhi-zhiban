#ifndef AUDIO_RECORDER_API_H
#define AUDIO_RECORDER_API_H

#include <stdint.h>

#define RECORDER_FORMAT_PCM_MONO    1
#define RECORDER_FORMAT_PCM_STEREO  2
#define RECORDER_FORMAT_PCM_QUAD    4

#define RECORDER_TYPE_CAPTURE_0     0
#define RECORDER_TYPE_CAPTURE_3     1
#define RECORDER_TYPE_MULTIMIC      2
#define RECORDER_TYPE_CAPTURE_5     4

typedef struct {
    int32_t format;
    int32_t type;
    int32_t channels;
    int32_t mic_num;
    int32_t ref_num;
    int32_t sample_rate;
    int32_t field_24;
    int32_t field_28;
    int32_t field_32;
} audio_recorder_config_t;

void* audio_recorder_open(audio_recorder_config_t* config);
int audio_recorder_close(void* handle);
int audio_recorder_start(void* handle);
int audio_recorder_stop(void* handle);
int audio_recorder_read(void* handle, void* buffer, int size);

#endif
