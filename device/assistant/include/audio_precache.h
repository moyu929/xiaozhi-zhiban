#ifndef AUDIO_PRECACHE_H
#define AUDIO_PRECACHE_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

#define PRECACHE_FRAME_DATA_SIZE 1500
#define PRECACHE_MAX_FRAMES 64

typedef struct {
    uint8_t data[PRECACHE_FRAME_DATA_SIZE];
    size_t len;
} precache_frame_t;

typedef struct {
    precache_frame_t frames[PRECACHE_MAX_FRAMES];
    int head;
    int count;
    int overflow_count;
    bool active;
    pthread_mutex_t mutex;
} audio_precache_t;

void audio_precache_init(audio_precache_t *pc);
void audio_precache_destroy(audio_precache_t *pc);

void audio_precache_start(audio_precache_t *pc);
void audio_precache_stop(audio_precache_t *pc);
bool audio_precache_is_active(audio_precache_t *pc);

int audio_precache_push(audio_precache_t *pc, const uint8_t *opus_data, size_t opus_len);

int audio_precache_drain_to_proto(audio_precache_t *pc, void *proto);

int audio_precache_count(audio_precache_t *pc);
void audio_precache_clear(audio_precache_t *pc);

#endif
