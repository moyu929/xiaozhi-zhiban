/**
 * test_native_aec.c — 原生 AEC (libduilite_fespl.so aec_api_*) 探针
 * 编译: 交叉编译后 push 到设备 /var/upgrade 运行
 *
 * 目标: 实测验证逆向 ABI 草案 (08 文档 §10.8), 定位模型加载路径
 *
 * 策略:
 *  1. aec_api_version / mem_size / framesize / mic_num / wavChan getter 自验证
 *  2. aec_api_new(NULL) + paramSet(mode) 探测通道预设
 *  3. duilite_library_load 加载 AEC 模型(原生 sair 同款路径)
 *  4. feed_pcm 喂合成正弦(模拟 TTS 回声) + run + pop_ifft 取输出
 *  5. 输出能量统计: 有有效输出 = ABI 成立
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <math.h>

typedef void* (*fn_new)(void*);
typedef int  (*fn_paramSet)(void*, int);
typedef int  (*fn_feed)(void*, const char*, int);
typedef int  (*fn_run)(void*);
typedef int  (*fn_pop_ifft)(void*, int, short*);
typedef int  (*fn_reset)(void*);
typedef void (*fn_delete)(void*);
typedef int  (*fn_get)(void*);
typedef const char* (*fn_ver)(void);
typedef int  (*fn_libload)(const char*);
typedef void (*fn_librel)(void);
typedef int  (*fn_usmaxvol)(void*);

#define AEC_MODEL "/usr/local/resource/duilite/AEC_ch3-2-ch2_1ref_common_20181226_v0.9.4.bin"

static void dump_getters(void *h, fn_get framesize, fn_get mic_num, fn_get wavChan, fn_get mem_size)
{
    if (!h) return;
    printf("  framesize=%d mic_num=%d wavChan=%d mem_size=%d\n",
           framesize(h), mic_num(h), wavChan(h), mem_size(h));
}

int main(void)
{
    printf("=== 原生 AEC 探针 ===\n");

    /* DDS 认证符号由 stub 提供(讯飞 SDK 授权层, 原生由 libdds.so 提供) */
    void *stub = dlopen("/usr/lib/libauth_ats3605d.so", RTLD_GLOBAL | RTLD_NOW);
    if (!stub)
        stub = dlopen("/var/upgrade/libdds_auth_stub.so", RTLD_GLOBAL | RTLD_NOW);
    printf("dds_auth_stub: %p\n", stub);
    if (stub)
    {
        void *sym = dlsym(stub, "dds_auth_do");
        printf("libdds dlsym(dds_auth_do) = %p\n", sym);
    }
    void *so = dlopen("/usr/lib/libduilite_fespl.so", RTLD_NOW);
    if (!so)
    {
        printf("设备版 fespl 失败: %s, 尝试本地自足版…\n", dlerror());
        so = dlopen("/var/upgrade/libduilite_fespl_local.so", RTLD_NOW);
    }
    if (!so) { printf("dlopen 失败: %s\n", dlerror()); return 1; }
    printf("fespl = %p\n", so);
    printf("dlopen ok\n");

    fn_ver      aec_ver   = (fn_ver)dlsym(so, "aec_api_version");
    fn_new      aec_new   = (fn_new)dlsym(so, "aec_api_new");
    fn_paramSet aec_pset  = (fn_paramSet)dlsym(so, "aec_api_paramSet");
    fn_feed     aec_feed  = (fn_feed)dlsym(so, "aec_api_feed_pcm");
    fn_run      aec_run   = (fn_run)dlsym(so, "aec_api_run");
    fn_pop_ifft aec_pop   = (fn_pop_ifft)dlsym(so, "aec_api_pop_ifft");
    fn_reset    aec_rst   = (fn_reset)dlsym(so, "aec_api_reset");
    fn_delete   aec_del   = (fn_delete)dlsym(so, "aec_api_delete");
    fn_get      g_frame   = (fn_get)dlsym(so, "aec_api_framesize");
    fn_get      g_mic     = (fn_get)dlsym(so, "aec_api_mic_num");
    fn_get      g_wav     = (fn_get)dlsym(so, "aec_api_wavChan");
    fn_get      g_mem     = (fn_get)dlsym(so, "aec_api_mem_size");
    fn_libload  lib_load  = (fn_libload)dlsym(so, "duilite_library_load");
    fn_librel   lib_rel   = (fn_librel)dlsym(so, "duilite_library_release");
    fn_usmaxvol usmaxvol  = (fn_usmaxvol)dlsym(so, "aec_api_usMaxVolumeDetect");

    printf("符号: ver=%p new=%p pset=%p feed=%p run=%p pop=%p rst=%p del=%p\n",
           (void*)aec_ver, (void*)aec_new, (void*)aec_pset, (void*)aec_feed,
           (void*)aec_run, (void*)aec_pop, (void*)aec_rst, (void*)aec_del);
    printf("      framesize=%p mic=%p wav=%p mem=%p libload=%p usmaxvol=%p\n",
           (void*)g_frame, (void*)g_mic, (void*)g_wav, (void*)g_mem,
           (void*)lib_load, (void*)usmaxvol);

    if (aec_ver) printf("AEC version: %s\n", aec_ver());

    /* ---- 1. 资源库加载: 参数为引擎配置 JSON(asr_config 序列化, asr_engine_init 反汇编定论) ---- */
    if (lib_load)
    {
        /* asr_engine_init 构造的 auth cfg 同款(含 savedProfile 指向设备认证档案),
         * 使 dds auth 通过 → 全局分配器环境就绪 → new 完整初始化 */
        const char *libcfg = "{\"productId\":\"278577032\","
            "\"savedProfile\":\"/tmp/saveprofile.txt\","
            "\"devInfo\":{\"customerDeviceId\":\"500000100100709D\"},"
            "\"aecBinPath\":\"" AEC_MODEL "\","
            "\"wakeupBinPath\":\"\","
            "\"beamformingBinPath\":\"\"}";
        printf("library_load(auth cfg 同款 json)...\n");
        fflush(stdout);
        int r = lib_load(libcfg);
        printf("library_load ret=%d\n", r);
    }

    /* ---- 2. new + getter ---- */
    void *h = aec_new ? aec_new((void*)AEC_MODEL) : NULL; /* cfg=模型bin路径(V2 §5: 内部 cfg_new_bin 读文件) */
    if (!h) { printf("aec_api_new(NULL) 失败, 尝试栈配置…\n"); }
    else
    {
        printf("aec_api_new ok, handle=%p\n", h);
        printf("getter: ");
        dump_getters(h, g_frame, g_mic, g_wav, g_mem);
        /* new 后立即 dump 分配结构(挪到 usmaxvol/rst 之前——昨晚崩在其后) */
        {
            char *hh = (char *)h;
            void *micIdx = *(void **)(hh + 4);
            void *refIdx = *(void **)(hh + 8);
            void *micRing = *(void **)(hh + 232);
            void *refRing = *(void **)(hh + 236);
            void *micEng = *(void **)(hh + 240);
            printf("分配: micIdx=%p refIdx=%p micRing=%p refRing=%p micEng=%p\n",
                   micIdx, refIdx, micRing, refRing, micEng);
            if (micIdx)
                printf("micIdx[] 前4: %d %d %d %d\n",
                       ((signed char *)micIdx)[0], ((signed char *)micIdx)[1],
                       ((signed char *)micIdx)[2], ((signed char *)micIdx)[3]);
            if (refIdx)
                printf("refIdx[] 前4: %d %d %d %d\n",
                       ((signed char *)refIdx)[0], ((signed char *)refIdx)[1],
                       ((signed char *)refIdx)[2], ((signed char *)refIdx)[3]);
        }
        /* 原生序列(ba498/ba4a4): new -> usMaxVolumeDetect(h) -> reset */
        if (usmaxvol) printf("usMaxVolumeDetect(h) ret=%d\n", usmaxvol(h));
        aec_rst(h);
        printf("reset done; micIdx=%p refIdx=%p micRing=%p refRing=%p\n",
               *(void**)((char*)h+4), *(void**)((char*)h+8),
               *(void**)((char*)h+232), *(void**)((char*)h+236));

        /* paramSet ABI 存疑(实测调用破坏结构), 模型已含正确配置(512/2/3), 跳过 */
        /* ---- 4. feed + run + pop 测试 (用默认 mode, 喂静音+正弦) ---- */
        int fs = g_frame(h);
        int mic = g_mic(h);
        int wav = g_wav(h);
        if (fs <= 0 || fs > 4096) { printf("异常 framesize=%d, 停止\n", fs); aec_del(h); return 1; }
        printf("用 framesize=%d mic=%d wavChan=%d 测试 feed/run/pop\n", fs, mic, wav);

        int nch = (wav > 0 && wav < 8) ? wav : 3;
        int samples_per_ch = fs;
        int total = samples_per_ch * nch; /* 帧长512(模型定论), 恰一帧 */
        short *pcm = malloc(total * sizeof(short));
        /* 合成: 每通道 440Hz 正弦(模拟回声), 幅度 8000 */
        for (int i = 0; i < samples_per_ch; i++)
        {
            short s = (short)(8000.0 * sin(2.0 * M_PI * 440.0 * i / 16000.0));
            for (int c = 0; c < nch; c++)
                pcm[i * nch + c] = s;
        }

        int rf = aec_feed(h, (const char*)pcm, total * sizeof(short));
        printf("feed_pcm(%d 样本x%d通道=%dB) ret=%d\n", samples_per_ch, nch, total * 2, rf);

        int rr = aec_run(h);
        printf("run ret=%d\n", rr);

        /* pop_ifft: 每个输出通道取 framesize 样本 */
        short *out = malloc(samples_per_ch * sizeof(short) * 4);
        for (int ch = 0; ch < 4; ch++)
        {
            memset(out, 0, samples_per_ch * sizeof(short));
            int rp = aec_pop(h, ch, out);
            if (rp == 0)
            {
                long sum = 0;
                int nz = 0;
                for (int i = 0; i < samples_per_ch; i++)
                {
                    sum += (long)out[i] * out[i];
                    if (out[i]) nz++;
                }
                int rms = (int)sqrt((double)sum / samples_per_ch);
                printf("pop_ifft(ch=%d) ret=0 RMS=%d 非零=%d/%d 前8: %d %d %d %d %d %d %d %d\n",
                       ch, rms, nz, samples_per_ch,
                       out[0], out[1], out[2], out[3], out[4], out[5], out[6], out[7]);
            }
            else
                printf("pop_ifft(ch=%d) ret=%d\n", ch, rp);
        }

        /* 连续喂 10 帧(正弦), 观察输出收敛(自适应启动) */
        printf("连续 10 帧收敛测试:\n");
        for (int fr = 0; fr < 10; fr++)
        {
            aec_feed(h, (const char*)pcm, total * sizeof(short));
            aec_run(h);
            for (int ch = 0; ch < 2 && ch < nch; ch++)
            {
                memset(out, 0, samples_per_ch * sizeof(short));
                if (aec_pop(h, ch, out) == 0)
                {
                    long sum = 0;
                    for (int i = 0; i < samples_per_ch; i++)
                        sum += (long)out[i] * out[i];
                    printf("  帧%d ch%d RMS=%d\n", fr, ch, (int)sqrt((double)sum / samples_per_ch));
                }
            }
        }

        free(pcm);
        free(out);
        aec_rst(h);
        aec_del(h);
        printf("reset+delete ok\n");
    }

    if (lib_rel) lib_rel();
    printf("=== 探针完成 ===\n");
    return 0;
}
