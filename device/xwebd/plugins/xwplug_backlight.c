/**
 * @file xwplug_backlight.c
 * @brief 背光控制插件（《09 方案》B4）
 *
 * 端点(经 /api/plugin/backlight/<sub> 或旧路径改写访问):
 *   GET /state                    -> {"enable":N,"brightness":当前}
 *   PUT /state {"enable":1,"brightness":120} -> 应用+持久化
 *
 * 持久化走插件自有配置 xwplug_backlight.conf(与 xwebd persist 隔离);
 * 启动时应用持久值(开机背光恢复, 替代 xwebd 的 apply_backlight_from_persist)。
 */

#include "xwplug_common.h"
#include <fcntl.h>

#define BL_SYSFS_BASE  "/sys/class/backlight/backlight/"
#define BL_DEFVAL      150

static int sysfs_write(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    ssize_t n = write(fd, val, strlen(val));
    close(fd);
    return n > 0 ? 0 : -1;
}

static int bl_read_cur(void)
{
    int cur = BL_DEFVAL;
    FILE *f = fopen(BL_SYSFS_BASE "brightness", "r");
    if (f) { if (fscanf(f, "%d", &cur) != 1) cur = BL_DEFVAL; fclose(f); }
    return cur;
}

static void bl_apply(int enable, int value)
{
    if (!enable) return;
    if (value < 10 || value > 255) value = BL_DEFVAL;
    char vb[16];
    snprintf(vb, sizeof(vb), "%d", value);
    if (sysfs_write(BL_SYSFS_BASE "brightness", vb) == 0)
        sysfs_write(BL_SYSFS_BASE "bl_power", "0");
}

int plug_init(void)
{
    /* 开机恢复持久背光(替代 xwebd 内置 apply_backlight_from_persist) */
    int enable = xwplug_conf_get("backlight", "enable", 0);
    int value = xwplug_conf_get("backlight", "value", -1);
    if (enable)
        bl_apply(1, value);
    return 0;
}

int plug_handle(const xwplug_req_t *req, char *resp, int resp_size)
{
    int enable = xwplug_conf_get("backlight", "enable", 0);

    if (strcmp(req->path, "/state") == 0 && strcmp(req->method, "GET") == 0) {
        snprintf(resp, resp_size, "{\"enable\":%d,\"brightness\":%d}", enable, bl_read_cur());
        return 200;
    }

    if (strcmp(req->path, "/state") == 0 && strcmp(req->method, "PUT") == 0) {
        char en_s[16] = "", val_s[16] = "";
        xwplug_json_str(req->body, "enable", en_s, sizeof(en_s));
        xwplug_json_str(req->body, "brightness", val_s, sizeof(val_s));
        if (!en_s[0] && !val_s[0]) {
            snprintf(resp, resp_size, "{\"error\":\"Missing enable/brightness field\"}");
            return 400;
        }
        if (en_s[0]) {
            enable = atoi(en_s) ? 1 : 0;
            xwplug_conf_set("backlight", "enable", enable);
        }
        if (val_s[0]) {
            int v = atoi(val_s);
            if (v < 10 || v > 255) {
                snprintf(resp, resp_size, "{\"error\":\"brightness must be 10..255\"}");
                return 400;
            }
            xwplug_conf_set("backlight", "value", v);
            bl_apply(1, v);
        } else if (enable) {
            bl_apply(enable, xwplug_conf_get("backlight", "value", -1));
        }
        /* 回读当前态 */
        snprintf(resp, resp_size, "{\"enable\":%d,\"brightness\":%d}", enable, bl_read_cur());
        return 200;
    }

    snprintf(resp, resp_size, "{\"error\":\"not found\",\"path\":\"%s\"}", req->path);
    return 404;
}

XWPLUG_MAIN("backlight")
