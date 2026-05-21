#include "audio_precache.h"
#include "protocol_handler.h"
#include "plog.h"
#include <string.h>

void audio_precache_init(audio_precache_t *pc)
{
    if (!pc)
        return;
    memset(pc, 0, sizeof(*pc));
    pthread_mutex_init(&pc->mutex, NULL);
}

void audio_precache_destroy(audio_precache_t *pc)
{
    if (!pc)
        return;
    pthread_mutex_destroy(&pc->mutex);
}

void audio_precache_start(audio_precache_t *pc)
{
    if (!pc)
        return;
    pthread_mutex_lock(&pc->mutex);
    pc->active = true;
    pc->head = 0;
    pc->count = 0;
    pc->overflow_count = 0;
    pthread_mutex_unlock(&pc->mutex);
    PLOG_I("PRECACHE", "缓存已启动 (容量=%d帧, 约%dms)",
           PRECACHE_MAX_FRAMES, PRECACHE_MAX_FRAMES * 60);
}

void audio_precache_stop(audio_precache_t *pc)
{
    if (!pc)
        return;
    pthread_mutex_lock(&pc->mutex);
    if (pc->active && pc->count > 0)
    {
        PLOG_W("PRECACHE", "缓存被丢弃: %d帧 (约%dms), 原因: 连接未建立或会话结束",
               pc->count, pc->count * 60);
    }
    pc->active = false;
    pc->head = 0;
    pc->count = 0;
    pc->overflow_count = 0;
    pthread_mutex_unlock(&pc->mutex);
}

bool audio_precache_is_active(audio_precache_t *pc)
{
    if (!pc)
        return false;
    pthread_mutex_lock(&pc->mutex);
    bool a = pc->active;
    pthread_mutex_unlock(&pc->mutex);
    return a;
}

int audio_precache_push(audio_precache_t *pc, const uint8_t *opus_data, size_t opus_len)
{
    if (!pc || !opus_data || opus_len == 0)
        return -1;

    pthread_mutex_lock(&pc->mutex);

    if (!pc->active)
    {
        pthread_mutex_unlock(&pc->mutex);
        return -1;
    }

    if (opus_len > PRECACHE_FRAME_DATA_SIZE)
    {
        PLOG_W("PRECACHE", "帧过大 %zu > %d, 丢弃", opus_len, PRECACHE_FRAME_DATA_SIZE);
        pthread_mutex_unlock(&pc->mutex);
        return -1;
    }

    if (pc->count >= PRECACHE_MAX_FRAMES)
    {
        pc->head = (pc->head + 1) % PRECACHE_MAX_FRAMES;
        pc->count--;
        pc->overflow_count++;
        if (pc->overflow_count <= 3 || pc->overflow_count % 10 == 0)
        {
            PLOG_W("PRECACHE", "缓存已满, 丢弃最旧帧 (溢出=%d)", pc->overflow_count);
        }
    }

    int idx = (pc->head + pc->count) % PRECACHE_MAX_FRAMES;
    memcpy(pc->frames[idx].data, opus_data, opus_len);
    pc->frames[idx].len = opus_len;
    pc->count++;

    pthread_mutex_unlock(&pc->mutex);
    return 0;
}

int audio_precache_drain_to_proto(audio_precache_t *pc, void *proto_ptr)
{
    protocol_handler_t *proto = (protocol_handler_t *)proto_ptr;
    if (!pc || !proto)
        return -1;

    pthread_mutex_lock(&pc->mutex);

    if (!pc->active)
    {
        pthread_mutex_unlock(&pc->mutex);
        return 0;
    }

    int total = pc->count;
    int skipped = 0;

    if (pc->count > 0 && pc->overflow_count > 0)
    {
        skipped = 1;
        pc->head = (pc->head + 1) % PRECACHE_MAX_FRAMES;
        pc->count--;
        PLOG_I("PRECACHE", "溢出发生过(%d次), 跳过最旧1帧避免边界截断",
               pc->overflow_count);
    }

    PLOG_I("PRECACHE", "开始排空: %d帧 (跳过=%d, 溢出=%d, 约%dms)",
           pc->count, skipped, pc->overflow_count, pc->count * 60);

    int sent = 0;
    for (int i = 0; i < pc->count; i++)
    {
        int idx = (pc->head + i) % PRECACHE_MAX_FRAMES;
        int ret = protocol_handler_send_audio(proto,
                                               pc->frames[idx].data,
                                               pc->frames[idx].len);
        if (ret == 0)
            sent++;
        else
            PLOG_W("PRECACHE", "排空发送失败 (帧%d/%d)", i + 1, pc->count);
    }

    pc->active = false;
    pc->head = 0;
    pc->count = 0;
    pc->overflow_count = 0;

    pthread_mutex_unlock(&pc->mutex);

    PLOG_I("PRECACHE", "排空完成: 发送%d/%d帧 (跳过%d帧)", sent, total - skipped, skipped);
    return sent;
}

int audio_precache_count(audio_precache_t *pc)
{
    if (!pc)
        return 0;
    pthread_mutex_lock(&pc->mutex);
    int c = pc->count;
    pthread_mutex_unlock(&pc->mutex);
    return c;
}

void audio_precache_clear(audio_precache_t *pc)
{
    if (!pc)
        return;
    pthread_mutex_lock(&pc->mutex);
    pc->head = 0;
    pc->count = 0;
    pc->overflow_count = 0;
    pthread_mutex_unlock(&pc->mutex);
}
