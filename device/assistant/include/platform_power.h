/**
 * @file platform_power.h
 * @brief 平台电源/音效能力封装（隔离一切对原生进程的 RPC / dlopen 白名单）
 *
 * 依据《逆向专项》定论：
 *  - 优雅关机 = 触发 libsystime 的定时关机通道（P3 §178）：libsystime 插件在
 *    msg_server 进程内广播 MSG_SHORTCUT_POWER(msg 63, 子码 200)，由 launcher/GUI
 *    电源逻辑执行动画关机链（sync_config + stop_watchdog + poweroff -f，M1 §8）。
 *    入口 API 即 libmsg_server_api 家族同源的 libsystime_api!set_timed_shutdown_time(x)，
 *    x<=0 表示关闭。
 *  - 音效 = libmsg_server_api!sound_tts_play(id) -> RPC cmd 1119（分歧四.4），
 *    id->wav 映射在 msg_server 服务端内部；id=10 为“下次再见”类结束语【占位经验值】。
 *
 * 安全红线（R-16）：任何对原生服务的写操作只允许经本文件白名单发出。
 */

#ifndef PLATFORM_POWER_H
#define PLATFORM_POWER_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 优雅关机（走原生动画链）
 *
 * 实现次序：
 *  1. dlopen("libsystime_api.so") 取 set_timed_shutdown_time；
 *  2. 设定 1 s 定时关机 → systime 500ms tick 内广播 (63,200)，GUI 接管动画与关机；
 *  3. 兜底线程 8s 后仍未退出（说明链路失效），回落 system("poweroff -f")。
 *
 * @return 0=已触发（含兜底线程启动），-1=完全失败（调用方可自选硬关机）
 */
int platform_power_shutdown_elegant(void);

/**
 * @brief 立即重启（原生亦无重启动画链，system("reboot") 为等价行为）
 */
void platform_power_reboot(void);

/**
 * @brief 播放系统预置音效（走 msg_server RPC 1119 通道，不占自家播放轨道）
 * @param id Package.dat 音效索引（如 10=再见类）；<0 忽略
 */
void platform_tts_play(int id);

/**
 * @brief 播放本地音频文件（原生 music_player 服务，mp3 等，后台异步）
 *
 * 走 libmusic_player_api 的 mp_open/mp_set_file/mp_play 链（alarm_play 同款
 * 原生路径），播完自动 mp_close（music_player 进程随之退出）。
 * @param path 文件绝对路径；不存在返回 -1
 * @return 0=已触发后台播放，-1=文件不可用
 */
int platform_media_play_file(const char *path);

/**
 * @brief 提示音是否仍在播放(music_player 后台线程存续期)
 *
 * use_limit 提示音走原生通道, ASR 无 AEC 听到提示音人声会误判唤醒;
 * 调用方在播放期+播后冷却窗内应静默拦截唤醒, 否则提示音自我触发
 * 成 5s 一轮的重播死循环(2026-08-31 实测)。
 * @return 1=播放线程在跑, 0=空闲
 */
int platform_media_is_playing(void);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_POWER_H */
