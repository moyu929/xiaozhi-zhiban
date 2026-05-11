/**
 * @file audio_dispatcher.c
 * @brief 音频数据分发器
 *
 * 本模块实现音频数据的分发机制，将采集到的音频数据
 * 广播给所有已注册的回调函数。使用互斥锁保证线程安全，
 * 支持动态注册和注销回调，适用于多模块共享同一音频源的场景。
 */

#include "audio_dispatcher.h"
#include <string.h>

void audio_dispatcher_init(audio_dispatcher_t *disp)
{
    memset(disp, 0, sizeof(audio_dispatcher_t));
    pthread_mutex_init(&disp->mutex, NULL);
}

void audio_dispatcher_destroy(audio_dispatcher_t *disp)
{
    if (!disp)
        return;
    pthread_mutex_destroy(&disp->mutex);
}

void audio_dispatcher_dispatch(audio_dispatcher_t *disp, const int16_t *data, int len)
{
    pthread_mutex_lock(&disp->mutex);
    int count = disp->count;
    audio_data_callback_t cbs[AUDIO_DISPATCHER_MAX_CALLBACKS];
    void *uds[AUDIO_DISPATCHER_MAX_CALLBACKS];
    memcpy(cbs, disp->callbacks, sizeof(audio_data_callback_t) * count);
    memcpy(uds, disp->user_datas, sizeof(void *) * count);
    pthread_mutex_unlock(&disp->mutex);

    for (int i = 0; i < count; i++)
    {
        if (cbs[i])
        {
            cbs[i](data, len, uds[i]);
        }
    }
}

int audio_dispatcher_register(audio_dispatcher_t *disp, audio_data_callback_t cb, void *user_data)
{
    if (!cb)
        return -1;

    pthread_mutex_lock(&disp->mutex);
    if (disp->count >= AUDIO_DISPATCHER_MAX_CALLBACKS)
    {
        pthread_mutex_unlock(&disp->mutex);
        return -1;
    }

    int idx = disp->count;
    disp->callbacks[idx] = cb;
    disp->user_datas[idx] = user_data;
    disp->count++;
    pthread_mutex_unlock(&disp->mutex);
    return 0;
}

void audio_dispatcher_unregister(audio_dispatcher_t *disp, audio_data_callback_t cb)
{
    if (!cb)
        return;

    pthread_mutex_lock(&disp->mutex);
    int count = disp->count;
    for (int i = 0; i < count; i++)
    {
        if (disp->callbacks[i] == cb)
        {
            int last = count - 1;
            if (i < last)
            {
                disp->callbacks[i] = disp->callbacks[last];
                disp->user_datas[i] = disp->user_datas[last];
            }
            disp->callbacks[last] = NULL;
            disp->user_datas[last] = NULL;
            disp->count = last;
            break;
        }
    }
    pthread_mutex_unlock(&disp->mutex);
}
