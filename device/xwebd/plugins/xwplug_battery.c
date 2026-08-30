/**
 * @file xwplug_battery.c
 * @brief 电池监控插件（《09 方案》B6, 自 sair 外移）
 *
 * 职责(自 main.c charge_log/battery_guard 迁入, 逻辑同源):
 * - charge_log: 60s 采样 电压/电流/查表容量/状态 -> /var/upgrade/charge_log.csv
 *   (>2MB 轮转 .old), 供 PC 端 ∫I·dt 积分定容
 * - 伪低电守卫: 30s 检查, 换电池后查表 cap<=15% 但真实电压>=3.4V 且未充电
 *   时写 /tmp/.fake_low_power 标记(配合 poweroff 守卫脚本拦截误关机)
 * - poweroff 守卫脚本: 启动幂等安装 /var/upgrade/poweroff(PATH 优先级拦截)
 * - 低电自护关机: 2026-08-30 实测原生低电链不触发(查表 cap 38->0 跳变,
 *   深放至 3.12V 硬断电)。连续 2 轮(60s) 电压<=3.35V 且未充电 -> 写
 *   /tmp/sair_cmd.json 走 sair 优雅关机链; 150s 后仍未关机则兜底
 *   system("poweroff")。留痕 /var/upgrade/lowbattery.log
 *
 * 端点: GET /status -> 当前电池采样(兼作网关健康探测)
 */

#include "xwplug_common.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>

#define BATT_SYS "/sys/class/power_supply/battery"
#define CHARGE_LOG_PATH "/var/upgrade/charge_log.csv"
#define CHARGE_LOG_MAX  (2 * 1024 * 1024)
#define FAKE_LOW_FLAG   "/tmp/.fake_low_power"
#define GUARD_VOLT_UV   3400000
#define GUARD_LOW_CAP   15
#define LOWBAT_VOLT_UV  3350000
#define LOWBAT_LOG      "/var/upgrade/lowbattery.log"
#define SAIR_CMD_PATH   "/tmp/sair_cmd.json"

static int node_int(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    char buf[32] = {0};
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    return atoi(buf);
}

static int node_str(const char *path, char *buf, int size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int n = read(fd, buf, size - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
    return 0;
}

/* ---- charge_log (原 main.c charge_log_tick) ---- */

static void charge_log_once(void)
{
    int volt = node_int(BATT_SYS "/voltage_now");
    int cur = node_int(BATT_SYS "/current_now");
    int cap = node_int(BATT_SYS "/capacity");
    char status[32] = {0};
    node_str(BATT_SYS "/status", status, sizeof(status));
    if (volt < 0 && cur < 0) return; /* 节点全缺失则不记 */

    struct stat st;
    if (stat(CHARGE_LOG_PATH, &st) == 0 && st.st_size > CHARGE_LOG_MAX)
        rename(CHARGE_LOG_PATH, CHARGE_LOG_PATH ".old");

    int fd = open(CHARGE_LOG_PATH, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd < 0) return;
    char line[96];
    int len = snprintf(line, sizeof(line), "%ld,%d,%d,%d,%s\n",
                       (long)time(NULL), volt, cur, cap, status);
    if (len > 0) write(fd, line, len);
    close(fd);
}

/* ---- 伪低电守卫 (原 main.c battery_guard_tick) ---- */

static void battery_guard_once(int *last_flagged)
{
    int volt = node_int(BATT_SYS "/voltage_now");
    int cap = node_int(BATT_SYS "/capacity");
    if (volt < 0 || cap < 0) return;

    /* 充电中不标记(msg_server 低电监控仅放电时触发, 且充电期用户关机不应被误拦) */
    int usb_on = node_int("/sys/class/power_supply/atc260x-usb/online");
    int wall_on = node_int("/sys/class/power_supply/atc260x-wall/online");
    int charging = (usb_on > 0 || wall_on > 0);

    int flagged = (!charging && cap <= GUARD_LOW_CAP && volt >= GUARD_VOLT_UV);
    if (flagged != *last_flagged) {
        if (flagged) {
            int fd = open(FAKE_LOW_FLAG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) close(fd);
        } else {
            unlink(FAKE_LOW_FLAG);
        }
        *last_flagged = flagged;
    }
}

/* ---- 低电自护关机 ---- */

static void lowbattery_guard_once(int *low_streak, int *fired, int *fired_rounds)
{
    int volt = node_int(BATT_SYS "/voltage_now");
    if (volt < 0) return;

    int usb_on = node_int("/sys/class/power_supply/atc260x-usb/online");
    int wall_on = node_int("/sys/class/power_supply/atc260x-wall/online");
    if (usb_on > 0 || wall_on > 0) {
        *low_streak = 0; /* 充电即解除, 插线瞬间不关机 */
        return;
    }

    if (volt <= LOWBAT_VOLT_UV) (*low_streak)++; else *low_streak = 0;
    if (*low_streak < 2) return;

    if (!*fired) {
        *fired = 1;
        int cur = node_int(BATT_SYS "/current_now");
        int cap = node_int(BATT_SYS "/capacity");
        int fd = open(LOWBAT_LOG, O_WRONLY | O_APPEND | O_CREAT, 0644);
        if (fd >= 0) {
            char line[96];
            int len = snprintf(line, sizeof(line), "LOWBAT v=%d i=%d cap=%d -> sair poweroff\n", volt, cur, cap);
            if (len > 0) write(fd, line, len);
            close(fd);
        }
    }

    /* 命令不在位则补写(sair 读取后删除, panel 命令可能覆盖) */
    if (access(SAIR_CMD_PATH, F_OK) != 0) {
        int fd = open(SAIR_CMD_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            write(fd, "{\"cmd\":\"poweroff\"}", 19);
            close(fd);
        }
    }

    /* 5 轮(150s)后仍在运行 = sair 未执行(崩溃/丢命令), 兜底裸关机 */
    if (++*fired_rounds >= 5) {
        int fd = open(LOWBAT_LOG, O_WRONLY | O_APPEND | O_CREAT, 0644);
        if (fd >= 0) {
            write(fd, "FALLBACK poweroff (sair not responsive)\n", 40);
            close(fd);
        }
        system("poweroff");
        sleep(60); /* poweroff 生效前不再重复触发 */
        *fired_rounds = 0;
    }
}

/* ---- poweroff 守卫脚本 (原 main.c install_poweroff_guard) ---- */

static void install_poweroff_guard(void)
{
    const char *guard_path = "/var/upgrade/poweroff";

    int fd = open(guard_path, O_RDONLY);
    if (fd >= 0) {
        char head[64] = {0};
        int n = read(fd, head, sizeof(head) - 1);
        close(fd);
        if (n > 9 && strncmp(head, "#!/bin/sh", 9) == 0) return; /* 已安装 */
    }

    const char *guard =
        "#!/bin/sh\n"
        "# poweroff_guard: 伪低电关机拦截 (battery 插件自动安装, 删本文件即卸载)\n"
        "V=$(cat /sys/class/power_supply/battery/voltage_now 2>/dev/null)\n"
        "P=$(cat /proc/$PPID/comm 2>/dev/null)\n"
        "if [ \"$P\" = \"sh\" ] || [ \"$P\" = \"ash\" ]; then\n"
        "  GP=$(awk '{print $4}' /proc/$PPID/stat 2>/dev/null)\n"
        "  [ -n \"$GP\" ] && P=$(cat /proc/$GP/comm 2>/dev/null)\n"
        "fi\n"
        "if [ \"$V\" -ge 3400000 ] 2>/dev/null; then\n"
        "  if [ \"$P\" = \"msg_server\" ]; then\n"
        "    echo \"$(date '+%m-%d %H:%M:%S') BLOCK msg_server v=$V\" >> /var/upgrade/poweroff_guard.log\n"
        "    exit 0\n"
        "  fi\n"
        "  if [ \"$P\" = \"manager\" ] && [ -f /tmp/.fake_low_power ]; then\n"
        "    rm -f /tmp/.fake_low_power\n"
        "    echo \"$(date '+%m-%d %H:%M:%S') BLOCK manager-fakelow v=$V\" >> /var/upgrade/poweroff_guard.log\n"
        "    exit 0\n"
        "  fi\n"
        "fi\n"
        "exec /sbin/poweroff \"$@\"\n";

    fd = open(guard_path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) return;
    int len = (int)strlen(guard);
    if (write(fd, guard, len) == len) chmod(guard_path, 0755);
    else unlink(guard_path);
    close(fd);
}

/* ---- 后台采集子进程: 30s 循环, charge_log 每 2 轮写一次 ---- */

static void sampler_child(void)
{
    prctl(PR_SET_NAME, "xwplug-bat-c");
    /* 插件进程被网关重启/卸载时随之终止, 防采样进程泄漏
     * (2026-08-31 实测: 插件 reinstall 后旧子进程变孤儿继续跑, 双份 charge_log) */
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (getppid() == 1) _exit(0); /* fork 后父已死的竞态兜底 */
    int last_flagged = -1;
    int low_streak = 0, fired = 0, fired_rounds = 0;
    int cycle = 0;
    while (1) {
        battery_guard_once(&last_flagged);
        lowbattery_guard_once(&low_streak, &fired, &fired_rounds);
        if (++cycle % 2 == 1) charge_log_once(); /* 60s 一点 */
        sleep(30);
    }
}

int plug_init(void)
{
    install_poweroff_guard();
    pid_t pid = fork();
    if (pid == 0) {
        sampler_child();
        _exit(0);
    }
    return 0;
}

/* ---- 端点 ---- */

int plug_handle(const xwplug_req_t *req, char *resp, int resp_size)
{
    if (strcmp(req->path, "/status") == 0 && strcmp(req->method, "GET") == 0) {
        int volt = node_int(BATT_SYS "/voltage_now");
        int cur = node_int(BATT_SYS "/current_now");
        int cap = node_int(BATT_SYS "/capacity");
        char status[32] = {0};
        node_str(BATT_SYS "/status", status, sizeof(status));
        int usb_on = node_int("/sys/class/power_supply/atc260x-usb/online");
        int wall_on = node_int("/sys/class/power_supply/atc260x-wall/online");
        snprintf(resp, resp_size,
                 "{\"voltage_uv\":%d,\"current_ua\":%d,\"capacity\":%d,\"status\":\"%s\","
                 "\"charging\":%s,\"fake_low_flag\":%s}",
                 volt, cur, cap, status,
                 (usb_on > 0 || wall_on > 0) ? "true" : "false",
                 access(FAKE_LOW_FLAG, F_OK) == 0 ? "true" : "false");
        return 200;
    }
    snprintf(resp, resp_size, "{\"error\":\"not found\",\"path\":\"%s\"}", req->path);
    return 404;
}

XWPLUG_MAIN("battery")
