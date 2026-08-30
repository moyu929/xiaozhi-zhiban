/**
 * @file xwplug_lights.c
 * @brief 灯控插件（三灯聚合: 屏幕背光 + 呼吸灯 + 按键背光）
 *
 * 用户诉求(2026-08-31): 呼吸灯/屏幕背光/按键背光语义同族, 聚合单插件管理。
 * 屏幕背光自 xwplug-backlight 迁入(迁移后旧插件卸载)。
 *
 * 端点(经 /api/plugin/lights/<sub> 或旧路径改写访问):
 *   GET /state   -> {"backlight":{...},"led":{"enabled":B},"key_backlight":{"enabled":B|null}}
 *   PUT /backlight {"enable":N,"brightness":N} -> 屏幕背光, 持久化
 *   PUT /led      {"enable":N} -> 呼吸灯(unix socket 0x2cec -> mqtt_custom_server), 持久化
 *   PUT /key_backlight {"enable":N} -> 按键背光(sysfs), 不持久化(同原内置行为)
 *
 * 持久化走插件自有配置 xwplug_lights.conf;
 * 开机恢复: backlight 直接应用; led 由 fork 子进程重试(mqtt_custom_server
 * 启动时序晚于插件, plug_init 内 3s 探测窗口不允许阻塞等待)。
 */

#include "xwplug_common.h"
#include <fcntl.h>

#define BL_SYSFS_BASE  "/sys/class/backlight/owl_backlight/"
#define BL_DEFVAL      400
#define BL_MAXVAL      900
/* 路径/范围修正(2026-08-31): 实测 owl_backlight, max_brightness=900;
 * 旧插件路径 backlight/ 不存在(GET 一直兜底假值 150, PUT 从未写成功) */
#define KEYBL_SYSFS    "/sys/class/input/input2/bl_onoff"
#define LED_SOCK       "/tmp/service/mqtt_custom_server"
#define PLUG           "lights"

static int sysfs_write(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    ssize_t n = write(fd, val, strlen(val));
    close(fd);
    return n > 0 ? 0 : -1;
}

static int sysfs_read_int(const char *path, int def)
{
    int v = def;
    FILE *f = fopen(path, "r");
    if (f) { if (fscanf(f, "%d", &v) != 1) v = def; fclose(f); }
    return v;
}

/* ---- 屏幕背光 (自 xwplug_backlight.c 迁移, 逻辑同源) ---- */

static int bl_read_cur(void)
{
    return sysfs_read_int(BL_SYSFS_BASE "brightness", BL_DEFVAL);
}

static void bl_apply(int enable, int value)
{
    if (!enable) return;
    if (value < 10 || value > BL_MAXVAL) value = BL_DEFVAL;
    char vb[16];
    snprintf(vb, sizeof(vb), "%d", value);
    if (sysfs_write(BL_SYSFS_BASE "brightness", vb) == 0)
        sysfs_write(BL_SYSFS_BASE "bl_power", "0");
}

/* ---- 呼吸灯 (自 xwebd.c send_led_cmd 迁移, 逻辑同源) ----
 * 消息: {0x2cec, need_resp=1, data_size=4, data=enable} -> mqtt_custom_server */

static int led_send(int enable)
{
    if (access(LED_SOCK, F_OK) != 0) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval tv = {3, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_un me;
    memset(&me, 0, sizeof(me));
    me.sun_family = AF_UNIX;
    snprintf(me.sun_path, sizeof(me.sun_path), "/tmp/xwplug/lights_led_%d", getpid());
    unlink(me.sun_path);
    if (bind(fd, (struct sockaddr *)&me, sizeof(me)) < 0) { close(fd); return -1; }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, LED_SOCK, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd); unlink(me.sun_path); return -1;
    }
    struct {
        int msg_type;
        int need_resp;
        int unknown;
        int data_size;
        int data;
    } msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_type = 0x2cec;
    msg.need_resp = 1;
    msg.data_size = 4;
    msg.data = enable;
    int ret = send(fd, &msg, sizeof(msg), 0);
    if (ret == (int)sizeof(msg) && msg.need_resp) {
        char resp[256];
        recv(fd, resp, sizeof(resp), 0); /* 尽力收, 超时不算失败 */
    }
    close(fd);
    unlink(me.sun_path);
    return 0;
}

/* ---- 开机恢复 ---- */

static void led_restore_child(int enable)
{
    /* mqtt_custom_server(manager 拉起)可能晚于本插件启动, 重试最多 10 次 x 3s */
    for (int i = 0; i < 10; i++) {
        if (led_send(enable) == 0) _exit(0);
        sleep(3);
    }
    _exit(1);
}

int plug_init(void)
{
    int bl_en = xwplug_conf_get(PLUG, "backlight_enable", 0);
    int bl_val = xwplug_conf_get(PLUG, "backlight_value", -1);
    if (bl_en)
        bl_apply(1, bl_val);

    int led_en = xwplug_conf_get(PLUG, "led_enable", -1);
    if (led_en >= 0) {
        if (led_send(led_en) != 0) {
            pid_t pid = fork();
            if (pid == 0) {
                prctl(PR_SET_NAME, "xwplug-lights-r");
                prctl(PR_SET_PDEATHSIG, SIGKILL);
                if (getppid() == 1) _exit(0);
                led_restore_child(led_en);
            }
        }
    }
    return 0;
}

/* ---- 端点 ---- */

static int handle_backlight(const xwplug_req_t *req, char *resp, int resp_size)
{
    int enable = xwplug_conf_get(PLUG, "backlight_enable", 0);

    if (strcmp(req->method, "GET") == 0) {
        snprintf(resp, resp_size, "{\"enable\":%d,\"brightness\":%d}", enable, bl_read_cur());
        return 200;
    }

    if (strcmp(req->method, "PUT") == 0) {
        char en_s[16] = "", val_s[16] = "";
        xwplug_json_str(req->body, "enable", en_s, sizeof(en_s));
        xwplug_json_str(req->body, "brightness", val_s, sizeof(val_s));
        /* 数字值回退(2026-08-31): json_str 只认 "带引号" 值, 数字形式
         * {"enable":1} 提取失败致 PUT 400——旧 backlight 插件继承的老缺陷,
         * panel 前端一直发数字形式(背光滑条曾静默失效) */
        if (!en_s[0]) {
            int v = xwplug_json_int(req->body, "enable", -1);
            if (v >= 0) snprintf(en_s, sizeof(en_s), "%d", v);
        }
        if (!val_s[0]) {
            int v = xwplug_json_int(req->body, "brightness", -1);
            if (v >= 0) snprintf(val_s, sizeof(val_s), "%d", v);
        }
        if (!en_s[0] && !val_s[0]) {
            snprintf(resp, resp_size, "{\"error\":\"Missing enable/brightness field\"}");
            return 400;
        }
        if (en_s[0]) {
            enable = atoi(en_s) ? 1 : 0;
            xwplug_conf_set(PLUG, "backlight_enable", enable);
        }
        if (val_s[0]) {
            int v = atoi(val_s);
            if (v < 10 || v > BL_MAXVAL) {
                snprintf(resp, resp_size, "{\"error\":\"brightness must be 10..900\"}");
                return 400;
            }
            xwplug_conf_set(PLUG, "backlight_value", v);
            bl_apply(1, v);
        } else if (enable) {
            bl_apply(enable, xwplug_conf_get(PLUG, "backlight_value", -1));
        }
        snprintf(resp, resp_size, "{\"enable\":%d,\"brightness\":%d}", enable, bl_read_cur());
        return 200;
    }

    snprintf(resp, resp_size, "{\"error\":\"method not allowed\"}");
    return 405;
}

static int handle_led(const xwplug_req_t *req, char *resp, int resp_size)
{
    if (strcmp(req->method, "PUT") == 0) {
        int enable = xwplug_json_int(req->body, "enable", -1);
        if (enable != 0 && enable != 1) {
            snprintf(resp, resp_size, "{\"error\":\"Missing enable field\"}");
            return 400;
        }
        if (led_send(enable) != 0) {
            snprintf(resp, resp_size, "{\"error\":\"led service unreachable\"}");
            return 502;
        }
        xwplug_conf_set(PLUG, "led_enable", enable);
        snprintf(resp, resp_size, "{\"enabled\":%s}", enable ? "true" : "false");
        return 200;
    }
    snprintf(resp, resp_size, "{\"error\":\"method not allowed\"}");
    return 405;
}

static int handle_key_backlight(const xwplug_req_t *req, char *resp, int resp_size)
{
    if (strcmp(req->method, "PUT") == 0) {
        int enable = xwplug_json_int(req->body, "enable", -1);
        if (enable != 0 && enable != 1) {
            snprintf(resp, resp_size, "{\"error\":\"Missing enable field\"}");
            return 400;
        }
        if (sysfs_write(KEYBL_SYSFS, enable ? "1" : "0") != 0) {
            snprintf(resp, resp_size, "{\"error\":\"sysfs write fail\"}");
            return 500;
        }
        snprintf(resp, resp_size, "{\"enabled\":%s}", enable ? "true" : "false");
        return 200;
    }
    snprintf(resp, resp_size, "{\"error\":\"method not allowed\"}");
    return 405;
}

int plug_handle(const xwplug_req_t *req, char *resp, int resp_size)
{
    if (strcmp(req->path, "/state") == 0 && strcmp(req->method, "GET") == 0) {
        int bl_en = xwplug_conf_get(PLUG, "backlight_enable", 0);
        int led_en = xwplug_conf_get(PLUG, "led_enable", -1);
        int kb = sysfs_read_int(KEYBL_SYSFS, -1);
        snprintf(resp, resp_size,
                 "{\"backlight\":{\"enable\":%d,\"brightness\":%d},"
                 "\"led\":{\"enabled\":%s},"
                 "\"key_backlight\":{\"enabled\":%s}}",
                 bl_en, bl_read_cur(),
                 led_en < 0 ? "null" : (led_en ? "true" : "false"),
                 kb < 0 ? "null" : (kb ? "true" : "false"));
        return 200;
    }
    if (strcmp(req->path, "/backlight") == 0)
        return handle_backlight(req, resp, resp_size);
    if (strcmp(req->path, "/led") == 0)
        return handle_led(req, resp, resp_size);
    if (strcmp(req->path, "/key_backlight") == 0)
        return handle_key_backlight(req, resp, resp_size);

    snprintf(resp, resp_size, "{\"error\":\"not found\",\"path\":\"%s\"}", req->path);
    return 404;
}

XWPLUG_MAIN(PLUG)
