#include "audio_recorder.h"
#include "plog.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>

#include "opus.h"
#include "audioproc.h"

/* ---- 上行链诊断 dump (Q5/Q7 联调专用, 定位后移除):
 * 触摸 /var/upgrade/.rec_dump 存在即启用; 会话(sending)期间把
 * [ch0 ch1 ref post-NLMS] 四路 int16 交错写入 /tmp/rec_dump.raw,
 * 4MB 自动停. post 为最终送入 Opus 的数据, ch0/ch1/ref 为原始三路. */
#define REC_DUMP_FLAG   "/var/upgrade/.rec_dump"
#define REC_DUMP_PATH   "/tmp/rec_dump.raw"
#define REC_DUMP_MAX    (4 * 1024 * 1024)
static FILE   *s_dump_fp;
static long    s_dump_bytes;
static int16_t s_dump_buf[4096];
static int     s_dump_n;

static void rec_dump_close(void)
{
    if (s_dump_fp)
    {
        if (s_dump_n > 0)
            fwrite(s_dump_buf, sizeof(int16_t), (size_t)s_dump_n, s_dump_fp);
        fclose(s_dump_fp);
        s_dump_fp = NULL;
        PLOG_I("REC", "dump 关闭: 共 %ld 字节", s_dump_bytes);
    }
}

static void rec_dump_sample(int16_t ch0, int16_t ch1, int16_t ref, int16_t post)
{
    if (s_dump_n + 4 > (int)(sizeof(s_dump_buf) / sizeof(s_dump_buf[0])))
    {
        fwrite(s_dump_buf, sizeof(int16_t), (size_t)s_dump_n, s_dump_fp);
        s_dump_n = 0;
    }
    s_dump_buf[s_dump_n++] = ch0;
    s_dump_buf[s_dump_n++] = ch1;
    s_dump_buf[s_dump_n++] = ref;
    s_dump_buf[s_dump_n++] = post;
    s_dump_bytes += 8;
}

static void do_encode_send(audio_recorder_module_t *rec)
{
    int opus_len = opus_encode((OpusEncoder *)rec->opus_encoder,
                               rec->mic_sample_buf,
                               rec->frame_count,
                               rec->opus_output_buf,
                               RECORDER_OPUS_BUF_SIZE);

    rec->frame_count = 0;

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

static void on_audio_data(const int16_t *data, int len, void *user_data)
{
    audio_recorder_module_t *rec = (audio_recorder_module_t *)user_data;
    if (!rec)
        return;

    /* Q7 诊断: 分发层到达统计(每5秒汇总, 定位后移除) */
    {
        static long s_calls, s_early, s_samples;
        static time_t s_t0;
        s_calls++;
        if (!rec->sending && !audio_precache_is_active(&rec->precache))
            s_early++;
        s_samples += len / 3;
        time_t now = time(NULL);
        if (now - s_t0 >= 5)
        {
            PLOG_I("REC", "[诊断] cb到达%ld次(早退%ld) 样本%ld len=%d sending=%d",
                   s_calls, s_early, s_samples, len, rec->sending);
            s_calls = s_early = s_samples = 0;
            s_t0 = now;
        }
    }

    if (!rec->sending && !audio_precache_is_active(&rec->precache))
        return;

    pthread_mutex_lock(&rec->mutex);

    /* dump 开关检测 (每批一次, 不逐样本) */
    if (rec->sending)
    {
        if (!s_dump_fp && access(REC_DUMP_FLAG, F_OK) == 0)
        {
            s_dump_fp = fopen(REC_DUMP_PATH, "wb");
            s_dump_bytes = 0;
            s_dump_n = 0;
            if (s_dump_fp)
                PLOG_I("REC", "dump 开启 -> %s (4MB 上限)", REC_DUMP_PATH);
        }
        else if (s_dump_fp && s_dump_bytes > REC_DUMP_MAX)
        {
            rec_dump_close();
            PLOG_I("REC", "dump 达 4MB 上限自动关闭");
        }
    }
    else if (s_dump_fp)
    {
        rec_dump_close();   /* 会话结束(停止发送)即收尾 */
    }

    int total_samples = len / 3;

    for (int i = 0; i < total_samples; i++)
    {
        /* 本地 AEC 兜底(Q2/Q3, 《08 方案》R-06):
         * mix = (mic0>>1)+(mic1>>1) 双麦等权混入, ref = ch2(V2 定论),
         * 输出干净 mono 供 Opus 上行. 原 data[i*3+1] 单通道直取路线废弃. */
        int16_t mic_mix = (int16_t)(((int32_t)data[i * 3 + 0] + data[i * 3 + 1]) >> 1);
        int16_t ref     = data[i * 3 + 2];
        /* cloud AEC 模式: 本地 NLMS 旁路(官方语义: 云端/本地互斥), 直通上行,
         * 由服务器按 timestamp 配对做源信号对消 */
        int16_t out = (rec->proto && rec->proto->cloud_aec)
                          ? mic_mix
                          : audioproc_process_sample(mic_mix, ref);
        rec->mic_sample_buf[rec->frame_count] = out;
        if (s_dump_fp)
            rec_dump_sample(data[i * 3 + 0], data[i * 3 + 1], ref,
                            rec->mic_sample_buf[rec->frame_count]);
        rec->frame_count++;

        if (rec->frame_count >= RECORDER_FRAME_SIZE)
        {
            do_encode_send(rec);
        }
    }

    pthread_mutex_unlock(&rec->mutex);
}

static int create_opus_encoder(audio_recorder_module_t *rec)
{
    int error;
    if (rec->opus_encoder)
    {
        opus_encoder_destroy((OpusEncoder *)rec->opus_encoder);
        rec->opus_encoder = NULL;
    }
    rec->opus_encoder = opus_encoder_create(RECORDER_SAMPLE_RATE, RECORDER_CHANNELS,
                                            OPUS_APPLICATION_VOIP, &error);
    if (!rec->opus_encoder || error != OPUS_OK)
    {
        PLOG_E("REC", "opus_encoder_create 创建失败: %d (%s)", error, opus_strerror(error));
        return -1;
    }
    opus_encoder_ctl((OpusEncoder *)rec->opus_encoder, OPUS_SET_BITRATE(RECORDER_BITRATE));
    opus_encoder_ctl((OpusEncoder *)rec->opus_encoder, OPUS_SET_COMPLEXITY(0));
    opus_encoder_ctl((OpusEncoder *)rec->opus_encoder, OPUS_SET_VBR(1));
    opus_encoder_ctl((OpusEncoder *)rec->opus_encoder, OPUS_SET_DTX(1));
    opus_encoder_ctl((OpusEncoder *)rec->opus_encoder, OPUS_SET_INBAND_FEC(0));
    return 0;
}

static int recorder_init_common(audio_recorder_module_t *rec, protocol_handler_t *proto, audio_dispatcher_t *disp)
{
    memset(rec, 0, sizeof(audio_recorder_module_t));
    rec->proto = proto;
    rec->disp = disp;

    if (create_opus_encoder(rec) != 0)
        return -1;

    audioproc_init(16000);   /* 本地AEC(NLMS兜底层)状态就绪 */

    pthread_mutex_init(&rec->mutex, NULL);
    audio_precache_init(&rec->precache);

    audio_dispatcher_register(disp, on_audio_data, rec);

    PLOG_I("REC", "初始化完成: 采样率=%d 声道=%d 码率=%d%s",
           RECORDER_SAMPLE_RATE, RECORDER_CHANNELS, RECORDER_BITRATE,
           proto ? "" : " (无proto)");
    return 0;
}

int audio_recorder_module_init(audio_recorder_module_t *rec, protocol_handler_t *proto, audio_dispatcher_t *disp)
{
    if (!rec || !proto || !disp)
        return -1;
    return recorder_init_common(rec, proto, disp);
}

int audio_recorder_module_early_init(audio_recorder_module_t *rec, audio_dispatcher_t *disp)
{
    if (!rec || !disp)
        return -1;
    return recorder_init_common(rec, NULL, disp);
}

void audio_recorder_module_set_proto(audio_recorder_module_t *rec, protocol_handler_t *proto)
{
    if (!rec || !proto)
        return;
    rec->proto = proto;
    PLOG_I("REC", "proto 已关联");
}

void audio_recorder_module_destroy(audio_recorder_module_t *rec)
{
    if (!rec)
        return;

    audio_recorder_module_stop_sending(rec);

    audio_precache_stop(&rec->precache);

    audio_dispatcher_unregister(rec->disp, on_audio_data);

    if (rec->opus_encoder)
    {
        opus_encoder_destroy((OpusEncoder *)rec->opus_encoder);
        rec->opus_encoder = NULL;
    }

    audio_precache_destroy(&rec->precache);
    pthread_mutex_destroy(&rec->mutex);
}

int audio_recorder_module_start_sending(audio_recorder_module_t *rec)
{
    if (!rec)
        return -1;

    pthread_mutex_lock(&rec->mutex);
    rec->frame_count = 0;
    rec->sending = true;
    pthread_mutex_unlock(&rec->mutex);

    PLOG_I("REC", "已开始发送录音");
    return 0;
}

int audio_recorder_module_stop_sending(audio_recorder_module_t *rec)
{
    if (!rec)
        return -1;

    pthread_mutex_lock(&rec->mutex);
    rec->sending = false;
    rec->frame_count = 0;
    pthread_mutex_unlock(&rec->mutex);

    PLOG_I("REC", "已停止发送录音");
    return 0;
}

bool audio_recorder_module_is_sending(audio_recorder_module_t *rec)
{
    if (!rec)
        return false;
    return rec->sending;
}
