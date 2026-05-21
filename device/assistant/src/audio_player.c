/**
 * @file audio_player.c
 * @brief 音频播放器实现
 *
 * 实现Opus音频解码和播放功能，包括：
 * - Opus解码器管理（创建、重置、销毁）
 * - 音频轨道（AudioTrack）的创建和管理
 * - Opus音频帧解码并写入播放轨道
 * - 延迟关闭机制（避免播放中断）
 * - 采样率动态切换
 * - 音量控制
 */

#include "audio_player.h"
#include "reverse/audio_service_api.h"
#include "plog.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>

#include "opus.h"

#define DECODE_BUF_SAMPLES 5760 /* Opus解码输出缓冲区样本数（最大120ms@48kHz） */
#define S32_BUF_SAMPLES 960     /* int32格式转换缓冲区样本数 */

/**
 * @brief 打开音频播放轨道
 * @param player 音频播放器实例指针
 * @return 0成功，-1失败
 *
 * 如果有待关闭或已打开的轨道，先释放再创建新轨道
 */
static int do_open_track(audio_player_t *player)
{
    /* 释放待关闭的轨道 */
    if (player->pending_close && player->track_handle)
    {
        audio_track_flush(player->track_handle);
        audio_track_stop(player->track_handle);
        audio_track_delete(player->track_handle);
        player->track_handle = NULL;
        player->pending_close = false;
        PLOG_I("PLAYER", "待关闭轨道已完全释放（flush+stop+delete）");
    }

    /* 释放仍然打开的轨道 */
    if (player->track_handle)
    {
        PLOG_W("PLAYER", "轨道仍然打开，强制完全释放（flush+stop+delete）");
        audio_track_flush(player->track_handle);
        audio_track_stop(player->track_handle);
        audio_track_delete(player->track_handle);
        player->track_handle = NULL;
    }

    /* 创建新的音频轨道 */
    void *handle = audio_track_create(NULL);
    if (!handle)
    {
        PLOG_E("PLAYER", "audio_track_create 创建失败");
        return -1;
    }

    int ret = audio_track_init(handle, player->channels, player->track_sample_rate, AUDIO_TRACK_TYPE_MUSIC);
    if (ret != 0)
    {
        PLOG_E("PLAYER", "audio_track_init 初始化失败: %d (声道=%d 采样率=%d)", ret, player->channels, player->sample_rate);
        audio_track_delete(handle);
        return -1;
    }

    ret = audio_track_play(handle);
    if (ret != 0)
    {
        PLOG_E("PLAYER", "audio_track_play 播放启动失败: %d", ret);
        audio_track_delete(handle);
        return -1;
    }

    player->track_handle = handle;
    player->track_open = true;

    PLOG_I("PLAYER", "轨道已打开: 采样率=%d(声道=%d) 轨道采样率=%d 位移=%d",
           player->sample_rate, player->channels, player->track_sample_rate, player->s16_to_s32_shift);
    return 0;
}

/**
 * @brief 延迟关闭音频轨道
 * @param player 音频播放器实例指针
 *
 * 只执行flush操作，将轨道标记为待关闭状态，
 * 实际的stop和delete在下次打开轨道时执行，避免播放中断
 */
static void do_deferred_close(audio_player_t *player)
{
    if (!player->track_handle)
        return;

    audio_track_flush(player->track_handle);
    player->track_open = false;
    player->pending_close = true;

    PLOG_I("PLAYER", "轨道延迟关闭（仅flush）");
}

/**
 * @brief 初始化音频播放器
 * @param player 音频播放器实例指针
 * @param sample_rate 采样率（默认24000）
 * @param channels 声道数（默认1）
 * @return 0成功，-1失败
 */
int audio_player_init(audio_player_t *player, int sample_rate, int channels)
{
    if (!player)
        return -1;

    memset(player, 0, sizeof(audio_player_t));
    player->sample_rate = sample_rate > 0 ? sample_rate : 24000;
    player->channels = channels > 0 ? channels : 1;
    player->s16_to_s32_shift = 0;
    player->track_sample_rate = player->sample_rate;
    player->playing = false;
    player->track_open = false;
    player->pending_close = false;

    pthread_mutex_init(&player->mutex, NULL);

    /* 创建Opus解码器 */
    int error;
    player->opus_decoder = opus_decoder_create(player->sample_rate, player->channels, &error);
    if (!player->opus_decoder || error != OPUS_OK)
    {
        PLOG_E("PLAYER", "opus_decoder_create 创建失败: %d (%s)", error, opus_strerror(error));
        pthread_mutex_destroy(&player->mutex);
        return -1;
    }

    /* 分配解码和格式转换缓冲区 */
    player->decode_buf = (int16_t *)malloc(DECODE_BUF_SAMPLES * sizeof(int16_t));
    player->s32_buf = (int32_t *)malloc(S32_BUF_SAMPLES * sizeof(int32_t));
    if (!player->decode_buf || !player->s32_buf)
    {
        PLOG_E("PLAYER", "缓冲区分配失败");
        if (player->opus_decoder)
            opus_decoder_destroy((OpusDecoder *)player->opus_decoder);
        free(player->decode_buf);
        free(player->s32_buf);
        pthread_mutex_destroy(&player->mutex);
        return -1;
    }

    PLOG_I("PLAYER", "初始化完成: 采样率=%d 轨道采样率=%d 声道=%d 位移=%d",
           player->sample_rate, player->track_sample_rate, player->channels, player->s16_to_s32_shift);
    return 0;
}

/**
 * @brief 销毁音频播放器，释放所有资源
 * @param player 音频播放器实例指针
 */
void audio_player_destroy(audio_player_t *player)
{
    if (!player)
        return;

    audio_player_stop(player);

    /* 释放音频轨道 */
    pthread_mutex_lock(&player->mutex);
    if (player->track_handle)
    {
        audio_track_flush(player->track_handle);
        audio_track_stop(player->track_handle);
        audio_track_delete(player->track_handle);
        player->track_handle = NULL;
    }
    player->track_open = false;
    player->pending_close = false;
    pthread_mutex_unlock(&player->mutex);

    /* 销毁Opus解码器 */
    if (player->opus_decoder)
    {
        opus_decoder_destroy((OpusDecoder *)player->opus_decoder);
        player->opus_decoder = NULL;
    }

    free(player->decode_buf);
    free(player->s32_buf);
    player->decode_buf = NULL;
    player->s32_buf = NULL;

    pthread_mutex_destroy(&player->mutex);
}

/**
 * @brief 启动音频播放
 * @param player 音频播放器实例指针
 * @return 0成功，-1失败
 */
int audio_player_start(audio_player_t *player)
{
    if (!player)
        return -1;

    pthread_mutex_lock(&player->mutex);

    if (player->pending_close && player->track_handle)
    {
        audio_track_play(player->track_handle);
        player->track_open = true;
        player->pending_close = false;
        PLOG_I("PLAYER", "延迟关闭轨道已恢复播放 (免重建)");
    }
    else if (!player->track_open)
    {
        if (do_open_track(player) != 0)
        {
            pthread_mutex_unlock(&player->mutex);
            return -1;
        }
    }
    else if (player->track_handle)
    {
        audio_track_flush(player->track_handle);
        audio_track_play(player->track_handle);
        PLOG_I("PLAYER", "轨道已刷新并重启播放 (防underrun)");
    }

    player->playing = true;
    pthread_mutex_unlock(&player->mutex);

    PLOG_I("PLAYER", "已启动播放");
    return 0;
}

/**
 * @brief 停止音频播放（不等待完成）
 * @param player 音频播放器实例指针
 * @return 0成功，-1失败
 */
int audio_player_stop(audio_player_t *player)
{
    return audio_player_stop_with_wait(player, false);
}

/**
 * @brief 停止音频播放，可选择等待播放完成
 * @param player 音频播放器实例指针
 * @param wait_for_completion 是否等待播放完成
 * @return 0成功，-1失败
 */
int audio_player_stop_with_wait(audio_player_t *player, bool wait_for_completion)
{
    if (!player)
        return -1;

    pthread_mutex_lock(&player->mutex);
    player->playing = false;

    if (player->ts_queue)
    {
        timestamp_queue_clear(player->ts_queue);
    }

    if (wait_for_completion && player->track_handle && player->track_open)
    {
        pthread_mutex_unlock(&player->mutex);

        /* 轮询等待轨道播放完成（最多2秒） */
        PLOG_I("PLAYER", "等待播放完成...");
        for (int i = 0; i < 100; i++)
        {
            usleep(20000);
            pthread_mutex_lock(&player->mutex);
            if (!player->track_open || !player->track_handle)
            {
                pthread_mutex_unlock(&player->mutex);
                break;
            }
            pthread_mutex_unlock(&player->mutex);
        }
        PLOG_I("PLAYER", "播放完成等待结束");

        pthread_mutex_lock(&player->mutex);
    }

    do_deferred_close(player);
    pthread_mutex_unlock(&player->mutex);

    PLOG_I("PLAYER", "已停止播放 (等待=%d)", wait_for_completion);
    return 0;
}

/**
 * @brief 立即释放音频轨道资源（保留解码器）
 * @param player 音频播放器实例指针
 */
void audio_player_release_track(audio_player_t *player)
{
    if (!player)
        return;

    pthread_mutex_lock(&player->mutex);
    player->playing = false;
    if (player->track_handle)
    {
        audio_track_flush(player->track_handle);
        audio_track_stop(player->track_handle);
        audio_track_delete(player->track_handle);
        player->track_handle = NULL;
    }
    player->track_open = false;
    player->pending_close = false;
    pthread_mutex_unlock(&player->mutex);

    PLOG_I("PLAYER", "轨道已释放（解码器保留）");
}

/**
 * @brief 写入Opus编码的音频数据并播放
 * @param player 音频播放器实例指针
 * @param opus_data Opus编码的音频数据
 * @param opus_len 音频数据长度
 * @param timestamp 时间戳（用于AEC回声消除）
 * @return 0成功，-1失败
 *
 * 流程：Opus解码 → int16转int32 → 写入AudioTrack
 */
int audio_player_write_opus(audio_player_t *player, const uint8_t *opus_data, size_t opus_len, uint32_t timestamp)
{
    if (!player || !opus_data)
        return -1;

    pthread_mutex_lock(&player->mutex);

    if (!player->playing)
    {
        pthread_mutex_unlock(&player->mutex);
        return -1;
    }

    if (player->aborted)
    {
        pthread_mutex_unlock(&player->mutex);
        return -1;
    }

    /* 如果轨道未打开，先打开 */
    if (!player->track_open || !player->track_handle)
    {
        if (do_open_track(player) != 0)
        {
            pthread_mutex_unlock(&player->mutex);
            return -1;
        }
    }

    /* Opus解码 */
    int samples = opus_decode((OpusDecoder *)player->opus_decoder,
                              opus_data, opus_len,
                              player->decode_buf, DECODE_BUF_SAMPLES,
                              0);

    if (samples < 0)
    {
        PLOG_W("PLAYER", "opus_decode 解码失败: %d", samples);
        pthread_mutex_unlock(&player->mutex);
        return -1;
    }

    /* 分块将int16转换为int32并写入播放轨道 */
    int offset = 0;
    while (offset < samples)
    {
        int chunk = samples - offset;
        if (chunk > S32_BUF_SAMPLES)
            chunk = S32_BUF_SAMPLES;

        /* int16左移转换为int32格式 */
        for (int i = 0; i < chunk; i++)
        {
            player->s32_buf[i] = (int32_t)player->decode_buf[offset + i] << player->s16_to_s32_shift;
        }

        audio_track_write_params_t params;
        memset(&params, 0, sizeof(params));
        params.channel_data[0] = player->s32_buf;
        params.num_channels = player->channels;
        params.num_samples = chunk;
        params.shift_bits = 0;

        int ret = audio_track_write_data(player->track_handle, &params);
        if (ret != 0)
        {
            PLOG_W("PLAYER", "audio_track_write_data 写入失败: %d", ret);
            pthread_mutex_unlock(&player->mutex);
            return ret;
        }

        offset += chunk;
    }

    /* 将时间戳推入时间戳队列 */
    if (player->ts_queue && timestamp > 0)
    {
        timestamp_queue_push(player->ts_queue, timestamp);
    }

    pthread_mutex_unlock(&player->mutex);
    return 0;
}

/**
 * @brief 清空音频播放缓冲区
 * @param player 音频播放器实例指针
 */
void audio_player_clear_buffer(audio_player_t *player)
{
    if (!player)
        return;

    pthread_mutex_lock(&player->mutex);
    if (player->track_handle)
    {
        audio_track_flush(player->track_handle);
    }
    pthread_mutex_unlock(&player->mutex);
}

/**
 * @brief 重置Opus解码器
 * @param player 音频播放器实例指针
 *
 * 销毁并重新创建解码器，同时清空播放缓冲区和时间戳队列
 */
void audio_player_reset_decoder(audio_player_t *player)
{
    if (!player)
        return;

    pthread_mutex_lock(&player->mutex);

    /* 重建Opus解码器 */
    if (player->opus_decoder)
    {
        int error;
        void *new_decoder = opus_decoder_create(
            player->sample_rate, player->channels, &error);
        if (!new_decoder || error != OPUS_OK)
        {
            PLOG_E("PLAYER", "解码器重置失败: %d，保留旧解码器", error);
        }
        else
        {
            opus_decoder_destroy((OpusDecoder *)player->opus_decoder);
            player->opus_decoder = new_decoder;
        }
    }

    /* 清空播放缓冲区 */
    if (player->track_handle)
    {
        audio_track_flush(player->track_handle);
    }

    /* 清空时间戳队列 */
    if (player->ts_queue)
    {
        timestamp_queue_clear(player->ts_queue);
    }

    pthread_mutex_unlock(&player->mutex);
    PLOG_I("PLAYER", "解码器已重置");
}

/**
 * @brief 切换采样率并重建解码器和播放轨道
 * @param player 音频播放器实例指针
 * @param new_sample_rate 新的采样率
 * @return 0成功，-1失败
 */
int audio_player_set_sample_rate(audio_player_t *player, int new_sample_rate)
{
    if (!player)
        return -1;

    if (player->sample_rate == new_sample_rate)
        return 0;

    PLOG_I("PLAYER", "重建解码器: %d -> %d", player->sample_rate, new_sample_rate);

    pthread_mutex_lock(&player->mutex);

    /* 创建新采样率的解码器（先创建再销毁旧的，避免失败后无解码器可用） */
    int error;
    void *new_decoder = opus_decoder_create(new_sample_rate, player->channels, &error);
    if (!new_decoder || error != OPUS_OK)
    {
        PLOG_E("PLAYER", "opus_decoder_create 创建失败 采样率 %d: %d", new_sample_rate, error);
        pthread_mutex_unlock(&player->mutex);
        return -1;
    }
    if (player->opus_decoder)
    {
        opus_decoder_destroy((OpusDecoder *)player->opus_decoder);
    }
    player->opus_decoder = new_decoder;

    /* 释放旧轨道（完整释放：flush+stop+delete） */
    if (player->pending_close && player->track_handle)
    {
        audio_track_flush(player->track_handle);
        audio_track_stop(player->track_handle);
        audio_track_delete(player->track_handle);
        player->track_handle = NULL;
        player->pending_close = false;
    }
    if (player->track_handle)
    {
        audio_track_flush(player->track_handle);
        audio_track_stop(player->track_handle);
        audio_track_delete(player->track_handle);
        player->track_handle = NULL;
        player->track_open = false;
    }

    /* 更新采样率参数 */
    player->sample_rate = new_sample_rate;
    player->s16_to_s32_shift = 0;
    player->track_sample_rate = new_sample_rate;

    /* 如果正在播放，重新打开轨道 */
    if (player->playing)
    {
        do_open_track(player);
    }

    pthread_mutex_unlock(&player->mutex);

    PLOG_I("PLAYER", "解码器已重建: 采样率=%d 位移=%d", player->sample_rate, player->s16_to_s32_shift);
    return 0;
}

/**
 * @brief 检查播放器是否正在播放
 * @param player 音频播放器实例指针
 * @return true正在播放，false未播放
 */
bool audio_player_is_playing(audio_player_t *player)
{
    if (!player)
        return false;
    return player->playing;
}

/**
 * @brief 设置播放音量
 * @param player 音频播放器实例指针
 * @param volume 音量值（0-100）
 * @return 0成功，-1失败
 * @note 音量映射：0-100 → 0-80（底层AudioTrack范围）
 */
int audio_player_set_volume(audio_player_t *player, int volume)
{
    if (!player)
        return -1;
    if (volume < 0)
        volume = 0;
    if (volume > 100)
        volume = 100;
    int track_vol = volume * 80 / 100;

    pthread_mutex_lock(&player->mutex);
    if (!player->track_handle)
    {
        pthread_mutex_unlock(&player->mutex);
        return -1;
    }
    int ret = audio_track_set_volume(player->track_handle, track_vol);
    pthread_mutex_unlock(&player->mutex);
    return ret;
}
