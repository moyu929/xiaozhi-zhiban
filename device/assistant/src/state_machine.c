/**
 * @file state_machine.c
 * @brief 小智助手状态机实现
 *
 * 本文件实现了小智助手的核心状态机，管理以下状态流转：
 * - Starting -> Activating -> Idle -> Connecting -> Listening -> Speaking -> Cleaning -> Idle
 * 提供状态转换合法性校验、观察者模式通知、线程安全的状态访问等功能。
 */

#include "state_machine.h"
#include "plog.h"
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

/* 状态名称字符串表，用于日志输出 */
static const char* state_names[] = {
    "Starting",
    "Activating",
    "Idle",
    "Connecting",
    "Listening",
    "Speaking",
    "Cleaning",
};

/**
 * @brief 获取状态对应的名称字符串
 * @param state 状态枚举值
 * @return 状态名称字符串，未知状态返回"Unknown"
 */
const char* state_machine_get_state_name(xiaozhi_state_t state) {
    if (state >= 0 && state <= kStateCleaning) {
        return state_names[state];
    }
    return "Unknown";
}

static void state_machine_write_state_file(xiaozhi_state_t state) {
    const char* state_name = state_machine_get_state_name(state);
    char buf[256];
    int len = snprintf(buf, sizeof(buf), "{\"state\":\"%s\"}\n", state_name);
    int fd = open("/tmp/sair_state.json", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, buf, len);
        close(fd);
    }
}

/**
 * @brief 检查状态转换是否合法
 *        定义了状态机的合法转换规则：
 *        - Starting -> Activating
 *        - Activating -> Idle
 *        - Idle -> Connecting
 *        - Connecting -> Listening / Cleaning
 *        - Listening -> Speaking / Cleaning / Connecting
 *        - Speaking -> Listening / Cleaning / Connecting
 *        - Cleaning -> Idle
 * @param from 源状态
 * @param to 目标状态
 * @return 1合法，0非法
 */
static int is_valid_transition(xiaozhi_state_t from, xiaozhi_state_t to) {
    if (from == to) return 1;

    switch (from) {
    case kStateStarting:
        return to == kStateActivating;

    case kStateActivating:
        return to == kStateIdle;

    case kStateIdle:
        return to == kStateConnecting;

    case kStateConnecting:
        return to == kStateListening || to == kStateCleaning;

    case kStateListening:
        return to == kStateSpeaking || to == kStateCleaning || to == kStateConnecting;

    case kStateSpeaking:
        return to == kStateListening || to == kStateCleaning || to == kStateConnecting;

    case kStateCleaning:
        return to == kStateIdle;

    default:
        return 0;
    }
}

/**
 * @brief 初始化状态机
 *        设置初始状态为Starting，初始化互斥锁
 * @param sm 状态机指针
 * @return 0成功，-1参数无效
 */
int state_machine_init(state_machine_t* sm) {
    if (!sm) return -1;
    memset(sm, 0, sizeof(state_machine_t));
    sm->current_state = kStateStarting;
    sm->transitioning = 0;
    sm->observer_count = 0;
    pthread_mutex_init(&sm->mutex, NULL);
    return 0;
}

/**
 * @brief 销毁状态机，释放互斥锁资源
 * @param sm 状态机指针
 */
void state_machine_destroy(state_machine_t* sm) {
    if (!sm) return;
    pthread_mutex_destroy(&sm->mutex);
    sm->observer_count = 0;
}

/**
 * @brief 获取当前状态（线程安全）
 * @param sm 状态机指针
 * @return 当前状态，参数无效时返回kStateStarting
 */
xiaozhi_state_t state_machine_get_state(state_machine_t* sm) {
    if (!sm) return kStateStarting;
    pthread_mutex_lock(&sm->mutex);
    xiaozhi_state_t state = sm->current_state;
    pthread_mutex_unlock(&sm->mutex);
    return state;
}

/**
 * @brief 执行状态转换（线程安全）
 *        校验转换合法性，防止嵌套转换，通知所有匹配的观察者
 * @param sm 状态机指针
 * @param new_state 目标状态
 * @return 0成功，-1参数无效/嵌套转换/非法转换
 */
int state_machine_transition(state_machine_t* sm, xiaozhi_state_t new_state) {
    if (!sm) return -1;

    pthread_mutex_lock(&sm->mutex);

    /* 防止嵌套转换（观察者回调中可能再次调用transition） */
    if (sm->transitioning) {
        pthread_mutex_unlock(&sm->mutex);
        PLOG_W("SM", "嵌套转换被阻止: %s -> %s",
               state_machine_get_state_name(sm->current_state),
               state_machine_get_state_name(new_state));
        return -1;
    }

    xiaozhi_state_t old_state = sm->current_state;

    /* 相同状态不转换 */
    if (old_state == new_state) {
        pthread_mutex_unlock(&sm->mutex);
        return 0;
    }

    /* 校验转换合法性 */
    if (!is_valid_transition(old_state, new_state)) {
        PLOG_W("SM", "非法转换: %s -> %s",
               state_machine_get_state_name(old_state),
               state_machine_get_state_name(new_state));
        pthread_mutex_unlock(&sm->mutex);
        return -1;
    }

    sm->transitioning = 1;
    sm->current_state = new_state;

    /* 快照观察者列表，避免持锁期间回调中修改列表导致死锁 */
    state_observer_entry_t obs_snapshot[MAX_STATE_OBSERVERS];
    int obs_count = sm->observer_count;
    memcpy(obs_snapshot, sm->observers, obs_count * sizeof(obs_snapshot[0]));

    pthread_mutex_unlock(&sm->mutex);

    PLOG_I("SM", "%s -> %s", state_machine_get_state_name(old_state),
           state_machine_get_state_name(new_state));

    state_machine_write_state_file(new_state);

    /* 按优先级顺序通知匹配的观察者 */
    for (int i = 0; i < obs_count; i++) {
        if (!obs_snapshot[i].callback) continue;
        if (obs_snapshot[i].target_state != kStateAny &&
            obs_snapshot[i].target_state != new_state) continue;
        obs_snapshot[i].callback(old_state, new_state, obs_snapshot[i].user_data);
    }

    pthread_mutex_lock(&sm->mutex);
    sm->transitioning = 0;
    pthread_mutex_unlock(&sm->mutex);

    return 0;
}

/**
 * @brief 观察者优先级比较函数（用于qsort排序）
 * @param a 观察者条目a
 * @param b 观察者条目b
 * @return 负数a优先，正数b优先，0相等
 */
static int observer_priority_cmp(const void* a, const void* b) {
    const state_observer_entry_t* ea = (const state_observer_entry_t*)a;
    const state_observer_entry_t* eb = (const state_observer_entry_t*)b;
    return ea->priority - eb->priority;
}

/**
 * @brief 添加状态变更观察者
 *        观察者按优先级排序，优先级值越小越先被通知
 * @param sm 状态机指针
 * @param target 目标状态，kStateAny表示监听所有状态变更
 * @param priority 优先级（值越小越先执行）
 * @param callback 观察者回调函数
 * @param user_data 传递给回调的用户数据
 * @return 0成功，-1参数无效或观察者数量已满
 */
int state_machine_add_observer(state_machine_t* sm, xiaozhi_state_t target,
                               int priority, state_observer_t callback, void* user_data) {
    if (!sm || !callback) return -1;

    pthread_mutex_lock(&sm->mutex);
    if (sm->observer_count >= MAX_STATE_OBSERVERS) {
        pthread_mutex_unlock(&sm->mutex);
        return -1;
    }

    state_observer_entry_t* entry = &sm->observers[sm->observer_count];
    entry->target_state = target;
    entry->priority = priority;
    entry->callback = callback;
    entry->user_data = user_data;
    sm->observer_count++;

    /* 按优先级重新排序 */
    qsort(sm->observers, sm->observer_count, sizeof(sm->observers[0]), observer_priority_cmp);

    pthread_mutex_unlock(&sm->mutex);
    return 0;
}

/**
 * @brief 移除状态变更观察者
 * @param sm 状态机指针
 * @param callback 要移除的回调函数指针
 */
void state_machine_remove_observer(state_machine_t* sm, state_observer_t callback) {
    if (!sm || !callback) return;

    pthread_mutex_lock(&sm->mutex);
    for (int i = 0; i < sm->observer_count; i++) {
        if (sm->observers[i].callback == callback) {
            /* 将后续观察者前移填补空位 */
            for (int j = i; j < sm->observer_count - 1; j++) {
                sm->observers[j] = sm->observers[j + 1];
            }
            sm->observer_count--;
            break;
        }
    }
    pthread_mutex_unlock(&sm->mutex);
}
