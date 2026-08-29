/**
 * test_ffvp.c — ffvp 干净音频直读路线探针
 * 复用 wakeup_module 同款 asr_engine 初始化, feed 3ch 后 ffvp_read 取输出
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <math.h>
#include <unistd.h>

typedef void* (*asr_init_t)(void*);
typedef int  (*asr_setp_t)(int, int, void*);
typedef int  (*asr_feed_t)(void*, const void*, int);
typedef int  (*asr_ffvp_read_t)(void*, void*, int);
typedef int  (*asr_start_t)(void*);

/* 与 wakeup_module 一致的 asr_config 布局 */
typedef struct {
    int32_t sample_rate; int32_t channels;
    char product_id[64]; char device_id[64];
    char main_wake_word[64]; char main_wake_thresh[16];
    char custom_wake_word[64]; char custom_wake_thresh[16];
    char local_command_word[64]; char local_command_thresh[16];
    char aec_res_path[256]; char wakeup_res_path[256];
    char uda_res_path[256]; char vad_res_path[256];
    char major_config[256]; char dcheck_config[256];
    int32_t malls_mode; int32_t reserved[10];
} asr_config_t;

/* sair 主程序提供的占位(sair.asm 同款) */
void *dump_open(void *ctx, int type, void *data) { (void)ctx; (void)type; (void)data; return 0; }
void dump_close(void *ctx) { (void)ctx; }
int dump_write(void *ctx, const void *data, int size) { (void)ctx; (void)data; return size; }

static void cb(int ev, int res) { printf("[cb] ev=%d res=%d\n", ev, res); }

int main(void)
{
    void *auth = dlopen("/usr/lib/libauth_ats3605d.so", RTLD_GLOBAL | RTLD_NOW);
    dlopen("/usr/lib/libapconfig.so", RTLD_GLOBAL | RTLD_NOW); /* get/set_config */
    dlopen("/usr/lib/libconfigpart.so", RTLD_GLOBAL | RTLD_NOW);
    dlopen("/usr/lib/libapplib.so", RTLD_GLOBAL | RTLD_NOW);   /* is_app_exist 等 */
    printf("auth=%p\n", auth);
    void *lib = dlopen("/usr/lib/libsair_asr.so", RTLD_NOW);
    if (!lib) { printf("asr lib fail: %s\n", dlerror()); return 1; }
    printf("asr lib ok\n");

    asr_setp_t setp = (asr_setp_t)dlsym(lib, "asr_engine_set_params");
    asr_init_t init = (asr_init_t)dlsym(lib, "asr_engine_init");
    asr_feed_t feed = (asr_feed_t)dlsym(lib, "asr_engine_feed_data");
    asr_start_t start = (asr_start_t)dlsym(lib, "asr_engine_start");
    asr_ffvp_read_t ffr = (asr_ffvp_read_t)dlsym(lib, "asr_engine_ffvp_read");
    asr_ffvp_read_t vdr = (asr_ffvp_read_t)dlsym(lib, "asr_engine_vad_read");
    printf("setp=%p init=%p feed=%p start=%p ffvp_read=%p\n", (void*)setp, (void*)init, (void*)feed, (void*)start, (void*)ffr);
    if (!init || !feed || !ffr) return 1;

    if (setp && setp(0, 4, (void*)cb) != 0) { printf("set_params fail\n"); return 1; }
    /* type=2 实测导致引擎状态错乱(feed 崩), 已移除.
     * sair.asm 153b4 原生真序列: set_params(h_ffvp, 4, cfg) — type=4 且
     * r0 为引擎句柄, 靠 start 后的引擎内建管线, 探针先走最小路径验证 */

    asr_config_t cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.sample_rate = 16000; cfg.channels = 3;
    strcpy(cfg.product_id, "278577032");
    strcpy(cfg.device_id, "20:32:33:3e:c5:d8");
    strcpy(cfg.main_wake_word, "xiaozhi");
    strcpy(cfg.main_wake_thresh, "major:2000");
    strcpy(cfg.aec_res_path, "/usr/local/resource/duilite/AEC_ch3-2-ch2_1ref_common_20181226_v0.9.4.bin");
    strcpy(cfg.wakeup_res_path, "/usr/local/resource/duilite/wakeup.bin");
    strcpy(cfg.uda_res_path, "/usr/local/resource/duilite/UDA_asr_ch2_2_ch2_40mm_20181226_v1.1.0.8_wkppost1_asrpost0_v2.bin");
    strcpy(cfg.vad_res_path, "/usr/local/resource/duilite/vad_bnn.bin");

    void *eng = init(&cfg);
    printf("engine=%p\n", eng);
    if (!eng) return 1;

    /* 原生顺序(sair.asm 14bc8-14bf4): init 之后才 set_params(2,4,{2,1}).
     * 昨晚崩溃根因: 在 init 前调用 type=2 破坏未初始化的全局引擎结构. */
    int ffvp_on[1] = {2};
    printf("set_params(2,4,{2}) ret=%d\n", setp ? setp(2, 4, ffvp_on) : -1);

    /* engine 结构关键字段 dump: +0(回调)/+40(ffvp缓冲)/+44(vad缓冲)/+152/+176/+180 */
    unsigned int *E = (unsigned int *)eng;
    printf("engine dump: [0]=%08x [40]=%08x [44]=%08x [152]=%08x [176]=%08x [180]=%08x\n",
           E[0], E[10], E[11], E[38], E[44], E[45]);

    int sr = start ? start(eng) : -99;
    printf("asr_engine_start ret=%d (0=ok)\n", sr);
    sleep(1); /* 等 init_done 回调 */

    /* feed 真实语音 3ch 流(/var/upgrade/feed3ch.raw, 含唤醒词+说话) */
    printf("[diag] pre-fopen\n"); fflush(stdout);
    FILE *f = fopen("/var/upgrade/feed3ch.raw", "rb");
    if (!f) { printf("feed3ch.raw 缺失\n"); return 1; }
    printf("[diag] file open, will feed\n"); fflush(stdout);
    static short pcm[1024 * 3]; /* 原生块尺寸: 1024 样本x3ch = 6144B */
    int frames = 0, frames_with_data = 0;
    long total_out = 0;
    int fr = 0;
    while (fread(pcm, 1, sizeof(pcm), f) == sizeof(pcm))
    {
        printf("[diag] feed frame %d\n", fr); fflush(stdout);
        feed(eng, pcm, sizeof(pcm));
        printf("[diag] feed ok\n"); fflush(stdout);
        usleep(64000); /* 1024 样本 = 64ms 实时节奏 */
        static short ffvp_out[2048];
        int n = ffr(eng, ffvp_out, sizeof(ffvp_out));
        int vn = vdr ? vdr(eng, ffvp_out, sizeof(ffvp_out)) : -1;
        if (fr % 5 == 1) printf("帧%d ffvp=%d vad=%d\n", fr, n, vn);
        if (n > 0) { frames_with_data++; total_out += n; }
        if (fr % 5 == 0)
            printf("帧%d(%ds): ffvp_read=%d\n", fr, fr * 3072 / 16000, n);
        frames++;
        fr++;
    }
    fclose(f);
    printf("总帧=%d 有数据帧=%d 累计字节=%ld\n", frames, frames_with_data, total_out);
    printf("[keepalive] 30s 观察窗 (pid=%d)\n", (int)getpid());
    fflush(stdout);
    for (int i = 0; i < 30; i++)
    {
        sleep(1);
        static short ffvp_out2[2048];
        int n2 = ffr(eng, ffvp_out2, sizeof(ffvp_out2));
        if (n2 > 0) { printf("[keepalive] %ds: ffvp_read=%d!\n", i, n2); fflush(stdout); }
    }
    return 0;
}
