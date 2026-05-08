#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include "protocol_handler.h"

typedef struct {
    void *track_handle;
    int sample_rate;
    int track_sample_rate;
    int channels;
    int s16_to_s32_shift;
    bool playing;
    bool track_open;
    bool pending_close;
    bool aborted;
    pthread_mutex_t mutex;

    void *opus_decoder;
    int16_t *decode_buf;
    int32_t *s32_buf;

    timestamp_queue_t *ts_queue;
} audio_player_t;

int audio_player_init(audio_player_t *player, int sample_rate, int channels);
void audio_player_destroy(audio_player_t *player);

int audio_player_start(audio_player_t *player);
int audio_player_stop(audio_player_t *player);
int audio_player_stop_with_wait(audio_player_t *player, bool wait_for_completion);
void audio_player_release_track(audio_player_t *player);
void audio_player_reset_decoder(audio_player_t *player);
int audio_player_write_opus(audio_player_t *player, const uint8_t *opus_data, size_t opus_len, uint32_t timestamp);
void audio_player_clear_buffer(audio_player_t *player);

int audio_player_set_sample_rate(audio_player_t *player, int new_sample_rate);
bool audio_player_is_playing(audio_player_t *player);
int audio_player_set_volume(audio_player_t *player, int volume);

#endif
