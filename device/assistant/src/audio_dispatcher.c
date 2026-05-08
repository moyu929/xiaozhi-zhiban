/**
 * @file audio_dispatcher.c
 * @brief 音频数据分发器
 *
 * 本模块实现音频数据的分发机制，将采集到的音频数据
 * 广播给所有已注册的回调函数。使用原子操作保证线程安全，
 * 支持动态注册和注销回调，适用于多模块共享同一音频源的场景。
 */

#include "audio_dispatcher.h"
#include <string.h>
#include <stdatomic.h>

/**
 * @brief 初始化音频分发器
 *
 * 将分发器结构体清零，包括回调数组、用户数据数组、
 * 回调计数等，使其处于初始状态。
 *
 * @param disp 音频分发器实例指针
 */
void audio_dispatcher_init(audio_dispatcher_t* disp) {
    memset(disp, 0, sizeof(audio_dispatcher_t));
}

/**
 * @brief 分发音频数据给所有已注册的回调
 *
 * 遍历回调列表，依次调用每个已注册的回调函数，
 * 将音频数据传递给各消费者。使用原子读取保证线程安全。
 *
 * @param disp 音频分发器实例指针
 * @param data 音频数据（16位PCM采样点）
 * @param len  采样点数
 */
void audio_dispatcher_dispatch(audio_dispatcher_t* disp, const int16_t* data, int len) {
    int count = atomic_load(&disp->count);
    for (int i = 0; i < count && i < AUDIO_DISPATCHER_MAX_CALLBACKS; i++) {
        audio_data_callback_t cb = atomic_load((_Atomic(audio_data_callback_t)*)&disp->callbacks[i]);
        void* ud = atomic_load((_Atomic(void*)*)&disp->user_datas[i]);
        if (cb) {
            cb(data, len, ud);
        }
    }
}

/**
 * @brief 注册音频数据回调
 *
 * 将回调函数及其用户数据添加到分发器的回调列表末尾。
 * 使用原子操作写入，保证与分发过程的线程安全。
 *
 * @param disp      音频分发器实例指针
 * @param cb        音频数据回调函数
 * @param user_data 传递给回调的用户数据
 * @return 0=成功，-1=回调为空或已达到最大注册数
 */
int audio_dispatcher_register(audio_dispatcher_t* disp, audio_data_callback_t cb, void* user_data) {
    if (!cb) return -1;
    int count = atomic_load(&disp->count);
    if (count >= AUDIO_DISPATCHER_MAX_CALLBACKS) return -1;

    atomic_store((_Atomic(audio_data_callback_t)*)&disp->callbacks[count], cb);
    atomic_store((_Atomic(void*)*)&disp->user_datas[count], user_data);
    atomic_store(&disp->count, count + 1);
    return 0;
}

/**
 * @brief 注销音频数据回调
 *
 * 从分发器的回调列表中移除指定回调。采用"末尾填充"策略：
 * 将最后一个回调移到被移除的位置，保持列表紧凑无空洞。
 * 使用原子操作保证线程安全。
 *
 * @param disp 音频分发器实例指针
 * @param cb   要注销的回调函数
 */
void audio_dispatcher_unregister(audio_dispatcher_t* disp, audio_data_callback_t cb) {
    if (!cb) return;
    int count = atomic_load(&disp->count);
    for (int i = 0; i < count; i++) {
        audio_data_callback_t existing = atomic_load((_Atomic(audio_data_callback_t)*)&disp->callbacks[i]);
        if (existing == cb) {
            int last = count - 1;
            if (i < last) {
                /* 将末尾回调移到当前位置，保持列表紧凑 */
                audio_data_callback_t last_cb = atomic_load((_Atomic(audio_data_callback_t)*)&disp->callbacks[last]);
                void* last_ud = atomic_load((_Atomic(void*)*)&disp->user_datas[last]);
                atomic_store((_Atomic(audio_data_callback_t)*)&disp->callbacks[i], last_cb);
                atomic_store((_Atomic(void*)*)&disp->user_datas[i], last_ud);
                atomic_store((_Atomic(audio_data_callback_t)*)&disp->callbacks[last], NULL);
                atomic_store((_Atomic(void*)*)&disp->user_datas[last], NULL);
            } else {
                /* 被移除的恰好是最后一个，直接清空 */
                atomic_store((_Atomic(audio_data_callback_t)*)&disp->callbacks[i], NULL);
                atomic_store((_Atomic(void*)*)&disp->user_datas[i], NULL);
            }
            atomic_store(&disp->count, last);
            return;
        }
    }
}
