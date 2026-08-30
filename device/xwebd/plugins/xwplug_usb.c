/**
 * @file xwplug_usb.c
 * @brief USB 模式插件（《09 方案》B3）
 *
 * 端点(经 /api/plugin/usb/<sub> 或旧路径改写访问):
 *   GET  /mode  -> 当前 USB 模式(functions/enable/lun0)
 *   POST /mode  -> 触发 hotplug 重检测(设备重新弹模式选择)
 *
 * 纯 sysfs 读写, 无持久化依赖。
 */

#include "xwplug_common.h"
#include <fcntl.h>

#define USB_SYS_BASE "/sys/class/android_usb/android0"

static int read_node(const char *path, char *buf, int buf_size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int n = read(fd, buf, buf_size - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
    return 0;
}

int plug_handle(const xwplug_req_t *req, char *resp, int resp_size)
{
    if (strcmp(req->path, "/mode") == 0 && strcmp(req->method, "GET") == 0) {
        char functions[64] = "";
        char enable[8] = "";
        char lun0[128] = "";
        read_node(USB_SYS_BASE "/functions", functions, sizeof(functions));
        read_node(USB_SYS_BASE "/enable", enable, sizeof(enable));
        read_node(USB_SYS_BASE "/f_mass_storage/lun0/file", lun0, sizeof(lun0));

        const char *mode = "disabled";
        if (enable[0] == '1') {
            if (strstr(functions, "mass_storage") && strstr(functions, "adb"))
                mode = "mass_adb";
            else if (strstr(functions, "mass_storage"))
                mode = "charge";
            else if (strstr(functions, "adb"))
                mode = "adb";
        }
        snprintf(resp, resp_size,
                 "{\"mode\":\"%s\",\"functions\":\"%s\",\"enabled\":%s,\"lun0\":\"%s\"}",
                 mode, functions, enable[0] == '1' ? "true" : "false", lun0);
        return 200;
    }

    if (strcmp(req->path, "/mode") == 0 && strcmp(req->method, "POST") == 0) {
        snprintf(resp, resp_size, "{\"ok\":true}");
        int fd = open("/sys/monitor/usb_port/config/run", O_WRONLY);
        if (fd >= 0) {
            write(fd, "1", 1);
            close(fd);
        }
        return 200;
    }

    snprintf(resp, resp_size, "{\"error\":\"not found\",\"path\":\"%s\"}", req->path);
    return 404;
}

XWPLUG_MAIN("usb")
