/**
 * @file audioproc.c
 * @brief 定点 NLMS 回声消除（软兜底层，见 audioproc.h）
 *
 * 算法：归一化 LMS（NLMS），Q13 权重 + Q15 样本，参考延迟线 64 taps。
 * 收敛速度足够压制扬声器回声残差（目标 ERLE≈10–15dB@语音段），比裸信号
 * 上行的双讲残留改善明显；精确指标见《08 方案》§3.2 验收口径。
 *
 * 2026-08-27 实机修正（Q7 dump 定论）：
 * - 实测 CAPTURE_5 模式下 ch2 参考恒为直流（-176），无真实回采；
 *   恒定参考会让 NLMS 学出反向权重吞噬麦克风能量（实测 post RMS 仅为 mic 一半）。
 *   故新增 ref 活性检测：参考滑动绝对偏差低于阈值时直通并软复位滤波器。
 * - 修复估计/更新两循环的索引配对错位（原估计 w[0]↔最旧样本、更新 w[0]↔最新样本，
 *   权重永远学不出正确传递函数）。
 */
#include "audioproc.h"
#include "plog.h"

#include <string.h>

#define AP_TAPS      1024        /* 64ms 覆盖: audio_track 缓冲延迟+腔体+空气路径(2026-08-27
                                  * 实测 64 taps=4ms 完全够不着回声主体, V2 反馈) */
#define AP_W_Q       13          /* 权重 Q13：范围 [-4.0, 4.0) */
#define AP_MU_Q7     40          /* 步长 μ ≈ 0.31，Q7 定标（NLMS 归一化后乘子） */

/* ref 活性阈值：偏差 EMA 超过此值视为有效参考（进入），低于此值视为恒定（退出） */
#define AP_REF_ON    256
#define AP_REF_OFF   64

typedef struct {
    int16_t  x_hist[AP_TAPS];    /* 参考信号历史环（Q15） */
    uint16_t x_idx;
    int32_t  w[AP_TAPS];         /* 自适应权重（Q13） */
    int32_t  x_energy_slow;      /* 平滑能量，防静音段除小数发散 */
    /* ref 活性检测状态 */
    int32_t  ref_ema;            /* 参考滑动均值（Q15） */
    int32_t  ref_dev_ema;        /* 参考绝对偏差滑动均值（Q15） */
    int      ref_active;         /* 参考是否有效（0=恒定/死通道，旁路） */
    int      logged_active;      /* 状态切换日志去重 */
    int32_t  e_ema;              /* 误差绝对值 EMA（双讲检测） */
    int32_t  y_ema;              /* 远端估计绝对值 EMA（双讲检测） */
} ap_ctx_t;

static ap_ctx_t g_ap;

void audioproc_init(int sample_rate)
{
    (void)sample_rate;
    audioproc_reset();
}

void audioproc_reset(void)
{
    memset(&g_ap, 0, sizeof(g_ap));
    g_ap.x_energy_slow = 1 << 20; /* 约 256 个满幅样本的能量级，初始柔和起点 */
}

int16_t audioproc_process_sample(int16_t mic, int16_t ref)
{
    ap_ctx_t *a = &g_ap;

    /* ---- ref 活性检测（Q7 定论：恒定参考必须旁路） -------------------- */
    int32_t dev = (int32_t)ref - a->ref_ema;
    a->ref_ema += dev >> 6;                   /* 慢速均值跟踪 */
    if (dev < 0)
        dev = -dev;
    a->ref_dev_ema += (dev - a->ref_dev_ema) >> 6;

    if (!a->ref_active)
    {
        if (a->ref_dev_ema > AP_REF_ON)       /* 参考变活：从零重学 */
        {
            int32_t ema = a->ref_ema;
            int32_t dema = a->ref_dev_ema;
            memset(a->x_hist, 0, sizeof(a->x_hist));
            memset(a->w, 0, sizeof(a->w));
            a->x_idx = 0;
            a->x_energy_slow = 1 << 20;
            a->ref_ema = ema;
            a->ref_dev_ema = dema;
            a->ref_active = 1;
            if (!a->logged_active)
            {
                PLOG_I("AEC", "参考通道活跃, NLMS 启用 (dev=%d)", (int)a->ref_dev_ema);
                a->logged_active = 1;
            }
        }
        else
        {
            return mic;                       /* 恒定参考：直通 */
        }
    }
    else if (a->ref_dev_ema < AP_REF_OFF)     /* 参考转恒定：旁路并复位 */
    {
        int32_t ema = a->ref_ema;
        int32_t dema = a->ref_dev_ema;
        memset(a->x_hist, 0, sizeof(a->x_hist));
        memset(a->w, 0, sizeof(a->w));
        a->x_idx = 0;
        a->x_energy_slow = 1 << 20;
        a->ref_ema = ema;
        a->ref_dev_ema = dema;
        a->ref_active = 0;
        if (a->logged_active)
        {
            PLOG_I("AEC", "参考通道恒定(dev=%d), NLMS 旁路直通", (int)a->ref_dev_ema);
            a->logged_active = 0;
        }
        return mic;
    }

    /* ---- 远端估计 y = Σ w_i · x(i) ------------------------------------- */
    int32_t acc = 0;
    uint16_t k = a->x_idx;
    for (int i = 0; i < AP_TAPS; i++)
    {
        int32_t x = a->x_hist[k];              /* Q15 */
        acc += ((int32_t)x * a->w[i]) >> AP_W_Q; /* Q15*Q13>>13 = Q15 级 */
        k = (uint16_t)((k + 1) & (AP_TAPS - 1));
    }
    if (acc > 32767)  acc = 32767;
    if (acc < -32768) acc = -32768;

    int32_t e = (int32_t)mic - acc;            /* 误差 = 近端-远端估计 (Q15) */

    /* ---- 自适应更新（NLMS，平滑能量归一） ------------------------------ */
    int32_t xsq = ((int32_t)ref * ref) >> 10;  /* Q20 级量化，防溢出 */
    a->x_energy_slow += (xsq - a->x_energy_slow) >> 5; /* IIR 平滑 3.1% */

    /* step(Q13) = MU · e / energy ；int64 中间量杜绝 Q33 溢出 */
    int32_t den_q10 = a->x_energy_slow >> 10;
    if (den_q10 < 8)
        den_q10 = 8;
    int64_t num = ((int64_t)AP_MU_Q7 * e) << 13;      /* MU(Q7)*e(Q15)<<13 */
    int32_t step = (int32_t)(num / ((int64_t)den_q10 * 128)); /* 归一 μ'=MU/128 */
    if (step > (1 << 12))  step = (1 << 12);   /* 单 tap 上限，防突发冲击 */
    if (step < -(1 << 12)) step = -(1 << 12);

    /* 双讲检测(DTD): 误差能量远大于远端估计 = 近端(用户语音)强,
     * 冻结权重更新防发散(V2 反馈: 打断插话时 NLMS 被用户语音污染后彻底失效) */
    {
        int32_t e_abs = e < 0 ? -e : e;
        int32_t y_abs = acc < 0 ? -acc : acc;
        a->e_ema += (e_abs - a->e_ema) >> 6;
        a->y_ema += (y_abs - a->y_ema) >> 6;
        if (a->e_ema > 3 * a->y_ema && a->e_ema > 200)
            goto skip_update;    /* 双讲: 跳过权重更新, 仅输出误差 */
    }

    /* 更新与估计同序配对：w[i] 对应 x_hist[(x_idx+i)&mask]，修复原错位 */
    k = a->x_idx;
    for (int i = 0; i < AP_TAPS; i++)
    {
        int32_t x = a->x_hist[k];
        if (x)
            a->w[i] += ((int64_t)step * x) >> 15;
        k = (uint16_t)((k + 1) & (AP_TAPS - 1));
    }

skip_update:

    /* ---- 新样本入环 ----------------------------------------------------- */
    a->x_hist[a->x_idx] = ref;
    a->x_idx = (uint16_t)((a->x_idx + 1) & (AP_TAPS - 1));

    if (e > 32767)  e = 32767;
    if (e < -32768) e = -32768;
    return (int16_t)e;
}
