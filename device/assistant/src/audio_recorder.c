/**
 * @file audio_recorder.c
 * @brief 音频录制模块实现
 *
 * 实现音频录制和编码发送功能，包括：
 * - Opus编码器管理（创建、配置、销毁）
 * - 从音频分发器获取录音数据
 * - 提取右声道数据并编码为Opus格式
 * - 通过协议处理器发送编码后的音频
 */

#include "audio_recorder.h"
#include "plog.h"
#include <string.h>
#include <stdlib.h>

#include "opus.h"

/**
 * @brief 编码右声道缓冲区数据并发送
 * @param rec 录制模块实例指针
 *
 * 将积累的右声道PCM数据编码为Opus格式，
 * 然后通过协议处理器发送到服务器
 */
static void do_encode_send(audio_recorder_module_t *rec)
{
    int opus_len = opus_encode((OpusEncoder *)rec->opus_encoder,
                               rec->right_channel_buf,
                               rec->right_channel_count,
                               rec->opus_output_buf,
                               RECORDER_OPUS_BUF_SIZE);

    rec->right_channel_count = 0;

    if (opus_len > 0)
    {
        if (rec->sending)
        {
            protocol_handler_send_audio(rec->proto, rec->opus_output_buf, opus_len);
        }
        else if (audio_precache_is_active(&rec->precache))
        {
            audio_precache_push(&rec->precache, rec->opus_output_buf, opus_len);
        }
    }
    else if (opus_len < 0)
    {
        PLOG_W("REC", "opus_encode 编码失败: %d", opus_len);
    }
}

/**
 * @brief 音频数据回调函数
 * @param data 三声道交错的PCM数据（左、右、参考）
 * @param len 数据总长度（字节数）
 * @param user_data 用户数据（audio_recorder_module_t指针）
 *
 * 从三声道交错的音频数据中提取右声道（索引1），
 * 积累到足够帧数后编码发送
 */
static void on_audio_data(const int16_t *data, int len, void *user_data)
{
    audio_recorder_module_t *rec = (audio_recorder_module_t *)user_data;
    if (!rec)
        return;

    if (!rec->sending && !audio_precache_is_active(&rec->precache))
        return;

    pthread_mutex_lock(&rec->mutex);

    /* 计算总样本数（三声道交错，每个采样点3个int16） */
    int total_samples = len / 3;

    for (int i = 0; i < total_samples; i++)
    {
        /* 提取右声道数据（三声道交错中索引为1的位置） */
        rec->right_channel_buf[rec->right_channel_count++] = data[i * 3 + 1];

        /* 缓冲区满时编码发送 */
        if (rec->right_channel_count >= RECORDER_RIGHT_CHANNEL_BUF_SIZE)
        {
            do_encode_send(rec);
        }
    }

    pthread_mutex_unlock(&rec->mutex);
}

/**
 * @brief 初始化音频录制模块
 * @param rec 录制模块实例指针
 * @param proto 协议处理器指针（用于发送编码后的音频）
 * @param disp 音频分发器指针（用于获取录音数据）
 * @return 0成功，-1失败
 */
int audio_recorder_module_init(audio_recorder_module_t *rec, protocol_handler_t *proto, audio_dispatcher_t *disp)
{
    if (!rec || !proto || !disp)
        return -1;

    memset(rec, 0, sizeof(audio_recorder_module_t));
    rec->proto = proto;
    rec->disp = disp;

    rec->sample_rate = 16000;
    rec->channels = 1;
    rec->frame_duration = 60;
    rec->bitrate = 16000;

    int error;
    rec->opus_encoder = opus_encoder_create(rec->sample_rate, rec->channels,
                                            OPUS_APPLICATION_VOIP, &error);
    if (!rec->opus_encoder || error != OPUS_OK)
    {
        PLOG_E("REC", "opus_encoder_create 创建失败: %d (%s)", error, opus_strerror(error));
        return -1;
    }

    opus_encoder_ctl((OpusEncoder *)rec->opus_encoder, OPUS_SET_BITRATE(rec->bitrate));
    opus_encoder_ctl((OpusEncoder *)rec->opus_encoder, OPUS_SET_COMPLEXITY(0));
    opus_encoder_ctl((OpusEncoder *)rec->opus_encoder, OPUS_SET_VBR(1));
    opus_encoder_ctl((OpusEncoder *)rec->opus_encoder, OPUS_SET_DTX(1));
    opus_encoder_ctl((OpusEncoder *)rec->opus_encoder, OPUS_SET_INBAND_FEC(0));

    pthread_mutex_init(&rec->mutex, NULL);
    audio_precache_init(&rec->precache);

    audio_dispatcher_register(disp, on_audio_data, rec);

    PLOG_I("REC", "初始化完成: 采样率=%d 声道=%d 码率=%d", rec->sample_rate, rec->channels, rec->bitrate);
    return 0;
}

int audio_recorder_module_early_init(audio_recorder_module_t *rec, audio_dispatcher_t *disp)
{
    if (!rec || !disp)
        return -1;

    memset(rec, 0, sizeof(audio_recorder_module_t));
    rec->disp = disp;

    rec->sample_rate = 16000;
    rec->channels = 1;
    rec->frame_duration = 60;
    rec->bitrate = 16000;

    int error;
    rec->opus_encoder = opus_encoder_create(rec->sample_rate, rec->channels,
                                            OPUS_APPLICATION_VOIP, &error);
    if (!rec->opus_encoder || error != OPUS_OK)
    {
        PLOG_E("REC", "opus_encoder_create(early) 创建失败: %d (%s)", error, opus_strerror(error));
        return -1;
    }

    opus_encoder_ctl((OpusEncoder *)rec->opus_encoder, OPUS_SET_BITRATE(rec->bitrate));
    opus_encoder_ctl((OpusEncoder *)rec->opus_encoder, OPUS_SET_COMPLEXITY(0));
    opus_encoder_ctl((OpusEncoder *)rec->opus_encoder, OPUS_SET_VBR(1));
    opus_encoder_ctl((OpusEncoder *)rec->opus_encoder, OPUS_SET_DTX(1));
    opus_encoder_ctl((OpusEncoder *)rec->opus_encoder, OPUS_SET_INBAND_FEC(0));

    pthread_mutex_init(&rec->mutex, NULL);
    audio_precache_init(&rec->precache);

    audio_dispatcher_register(disp, on_audio_data, rec);

    PLOG_I("REC", "早期初始化完成 (无proto): 采样率=%d 声道=%d 码率=%d",
           rec->sample_rate, rec->channels, rec->bitrate);
    return 0;
}

void audio_recorder_module_set_proto(audio_recorder_module_t *rec, protocol_handler_t *proto)
{
    if (!rec || !proto)
        return;
    rec->proto = proto;
    PLOG_I("REC", "proto 已关联");
}

/**
 * @brief 销毁音频录制模块，释放资源
 * @param rec 录制模块实例指针
 */
void audio_recorder_module_destroy(audio_recorder_module_t *rec)
{
    if (!rec)
        return;

    audio_recorder_module_stop_sending(rec);

    audio_precache_stop(&rec->precache);

    /* 取消注册音频回调 */
    audio_dispatcher_unregister(rec->disp, on_audio_data);

    /* 销毁Opus编码器 */
    if (rec->opus_encoder)
    {
        opus_encoder_destroy((OpusEncoder *)rec->opus_encoder);
        rec->opus_encoder = NULL;
    }

    audio_precache_destroy(&rec->precache);
    pthread_mutex_destroy(&rec->mutex);
}

/**
 * @brief 开始发送录音数据
 * @param rec 录制模块实例指针
 * @return 0成功，-1失败
 */
int audio_recorder_module_start_sending(audio_recorder_module_t *rec)
{
    if (!rec)
        return -1;

    pthread_mutex_lock(&rec->mutex);
    rec->right_channel_count = 0;
    rec->sending = true;
    pthread_mutex_unlock(&rec->mutex);

    PLOG_I("REC", "已开始发送录音");
    return 0;
}

/**
 * @brief 停止发送录音数据
 * @param rec 录制模块实例指针
 * @return 0成功，-1失败
 */
int audio_recorder_module_stop_sending(audio_recorder_module_t *rec)
{
    if (!rec)
        return -1;

    pthread_mutex_lock(&rec->mutex);
    rec->sending = false;
    rec->right_channel_count = 0;
    pthread_mutex_unlock(&rec->mutex);

    PLOG_I("REC", "已停止发送录音");
    return 0;
}

/**
 * @brief 检查是否正在发送录音数据
 * @param rec 录制模块实例指针
 * @return true正在发送，false未发送
 */
bool audio_recorder_module_is_sending(audio_recorder_module_t *rec)
{
    if (!rec)
        return false;
    return rec->sending;
}
