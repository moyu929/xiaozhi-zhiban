/**
 * @file diag_module.c
 * @brief 运行时健康检查模块
 *
 * 检查 assistant 运行时的健康状态，包括：
 * - 配置文件读写、日志系统、看门狗、WebSocket连接状态
 *
 * WiFi连接、磁盘空间等部署前置环境检查由 xwebd 的
 * /api/diag 和 /api/assistant/env 接口负责，此处不再重复。
 */

#include "diag_module.h"
#include "app_context.h"
#include "protocol_handler.h"
#include "plog.h"
#include "xiaozhi_config.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <dirent.h>

#define TAG "DIAG"
#define DIAG_TEMP_FILE "/var/upgrade/.diag_test_tmp"

static void diag_add(diag_result_t *r, const char *name, int ok, const char *message)
{
    if (r->count >= DIAG_MAX_ITEMS)
    {
        PLOG_W(TAG, "自检项超过上限 %d，跳过: %s", DIAG_MAX_ITEMS, name);
        return;
    }
    r->items[r->count].name = name;
    r->items[r->count].ok = ok;
    r->items[r->count].message = message;
    r->count++;
}

static void check_websocket(diag_result_t *r)
{
    extern app_context_t g_app;
    if (g_app.proto_initialized && protocol_handler_is_connected(&g_app.proto))
    {
        diag_add(r, "WebSocket", 1, "已连接云端");
    }
    else if (g_app.proto_initialized)
    {
        diag_add(r, "WebSocket", 0, "未连接云端");
    }
    else
    {
        diag_add(r, "WebSocket", 0, "协议未初始化");
    }
}

static void check_config_dir(diag_result_t *r)
{
    const char *test_path = DIAG_TEMP_FILE;
    const char *test_data = "diag_test";
    int fd = open(test_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        diag_add(r, "配置文件", 0, "配置文件读写失败: 无法创建文件");
        return;
    }
    if (write(fd, test_data, strlen(test_data)) < 0)
    {
        close(fd);
        unlink(test_path);
        diag_add(r, "配置文件", 0, "配置文件读写失败: 写入失败");
        return;
    }
    close(fd);

    char read_buf[32] = {0};
    fd = open(test_path, O_RDONLY);
    if (fd < 0)
    {
        unlink(test_path);
        diag_add(r, "配置文件", 0, "配置文件读写失败: 无法读取文件");
        return;
    }
    int n = read(fd, read_buf, sizeof(read_buf) - 1);
    close(fd);
    unlink(test_path);

    if (n > 0 && strcmp(read_buf, test_data) == 0)
    {
        diag_add(r, "配置文件", 1, "配置文件读写正常");
    }
    else
    {
        diag_add(r, "配置文件", 0, "配置文件读写失败: 内容校验不一致");
    }
}

static void check_log(diag_result_t *r)
{
    int fd = open(PLOG_PATH, O_WRONLY | O_APPEND);
    if (fd >= 0)
    {
        close(fd);
        diag_add(r, "日志系统", 1, "日志系统正常");
    }
    else
    {
        diag_add(r, "日志系统", 0, "日志系统异常");
    }
}

static void check_watchdog(diag_result_t *r)
{
    if (access("/usr/lib/libapplib.so", R_OK) == 0)
    {
        diag_add(r, "看门狗", 1, "看门狗接口库可访问");
    }
    else
    {
        diag_add(r, "看门狗", 0, "看门狗接口库不可访问");
    }
}

/* ---- 电池诊断: 用户换大容量电池后续航异常调查 (2026-08-29).
 * atc260x PMU 驱动暴露 /sys/class/power_supply/ 各节点, 百分比/电压/容量
 * 的计算全在内核驱动; 此处全节点读取以定位容量计算方式与阈值. ---- */
static int diag_read_node(const char *path, char *buf, int buf_size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    int n = read(fd, buf, buf_size - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    /* 去尾部换行 */
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = '\0';
    return 0;
}

static void check_battery(diag_result_t *r)
{
    /* message 为 const char*, 动态内容用静态池 */
    static char msg_pool[DIAG_MAX_ITEMS][96];
    static int pool_idx = 0;

    static const struct
    {
        const char *path;
        const char *label;
    } nodes[] = {
        {"/sys/class/power_supply/battery/status", "电池状态"},
        {"/sys/class/power_supply/battery/capacity", "电量百分比"},
        {"/sys/class/power_supply/battery/voltage_now", "电压uV"},
        {"/sys/class/power_supply/battery/voltage_avg", "平均电压uV"},
        {"/sys/class/power_supply/battery/present", "电池在线"},
        {"/sys/class/power_supply/battery/health", "电池健康"},
        {"/sys/class/power_supply/battery/technology", "电池技术"},
        {"/sys/class/power_supply/battery/temp", "温度"},
        {"/sys/class/power_supply/battery/charge_full", "满充电荷uAh"},
        {"/sys/class/power_supply/battery/charge_full_design", "设计容量uAh"},
        {"/sys/class/power_supply/battery/energy_full", "满充能量uWh"},
        {"/sys/class/power_supply/battery/cycle_count", "循环次数"},
        {"/sys/class/power_supply/battery/voltage_max_design", "设计电压uV"},
        {"/sys/class/power_supply/battery/current_now", "电流uA"},
        {"/sys/class/power_supply/battery/constant_charge_current_max", "最大充电电流uA"},
        {"/sys/class/power_supply/battery/charge_type", "充电类型"},
        {"/sys/class/power_supply/atc260x-usb/online", "USB在线"},
        {"/sys/class/power_supply/atc260x-wall/online", "DC在线"},
        {"/proc/bootmode", "启动模式"},
    };
    int found = 0;
    char val[64];

    for (unsigned int i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++)
    {
        if (diag_read_node(nodes[i].path, val, sizeof(val)) != 0)
            continue; /* 节点不存在, 驱动未实现, 跳过 */
        found++;
        char *msg = msg_pool[pool_idx % DIAG_MAX_ITEMS];
        pool_idx++;
        snprintf(msg, 96, "%s", val);
        diag_add(r, nodes[i].label, 1, msg);
    }

    if (!found)
        diag_add(r, "电池", 0, "无可读的电池节点");

    /* 电池节点可写性探测: 若 capacity 可写, sair 可周期覆写校正值,
     * 一举修正显示/低电判断/自动关机整条链 (不实际写入, 仅 open 测试) */
    {
        static const char *w_paths[] = {
            "/sys/class/power_supply/battery/capacity",
            "/sys/class/power_supply/battery/voltage_now",
        };
        static const char *w_names[] = {"capacity可写", "voltage可写"};
        static char w_msg[2][32];
        for (int i = 0; i < 2; i++)
        {
            int wfd = open(w_paths[i], O_WRONLY);
            if (wfd >= 0)
            {
                close(wfd);
                snprintf(w_msg[i], 32, "是");
            }
            else
            {
                snprintf(w_msg[i], 32, "否(errno=%d)", errno);
            }
            diag_add(r, w_names[i], 1, w_msg[i]);
        }
    }

    /* rootfs 可写性/poweroff 形态: 低电关机拦截方案可行性调查 */
    {
        static char fs_msg[640];
        fs_msg[0] = '\0';
        /* /proc/mounts 关键行: root 与 upgrade 挂载 */
        int fd = open("/proc/mounts", O_RDONLY);
        if (fd >= 0)
        {
            int n;
            char mounts[2048];
            n = read(fd, mounts, sizeof(mounts) - 1);
            close(fd);
            if (n > 0)
            {
                mounts[n] = '\0';
                char *p = mounts;
                while (*p)
                {
                    char *eol = strchr(p, '\n');
                    if (eol)
                        *eol = '\0';
                    /* 只关注根分区和可写挂载 */
                    if (strncmp(p, "/dev/root", 9) == 0 || strstr(p, " rw,") || strstr(p, " rw ") ||
                        strstr(p, "jffs2") || strstr(p, "overlay") || strstr(p, "ubifs"))
                    {
                        int len = strlen(fs_msg);
                        snprintf(fs_msg + len, sizeof(fs_msg) - len, "%.80s | ", p);
                    }
                    if (!eol)
                        break;
                    p = eol + 1;
                }
            }
        }
        if (fs_msg[0])
            diag_add(r, "挂载表", 1, fs_msg);

        /* poweroff 形态: 是否存在/符号链接目标/头字节 */
        {
            static char po_msg[320];
            struct stat st;
            char head[20] = {0};
            char target[128] = "";
            int n = readlink("/sbin/poweroff", target, sizeof(target) - 1);
            if (n > 0)
                target[n] = '\0';
            int pfd = open("/sbin/poweroff", O_RDONLY);
            if (pfd >= 0)
            {
                read(pfd, head, 8);
                close(pfd);
            }
            if (stat("/sbin/poweroff", &st) == 0)
            {
                snprintf(po_msg, sizeof(po_msg), "size=%ld link=%s head=%02x%02x%02x%02x",
                         (long)st.st_size, target[0] ? target : "-",
                         (unsigned char)head[0], (unsigned char)head[1],
                         (unsigned char)head[2], (unsigned char)head[3]);
                diag_add(r, "poweroff", 1, po_msg);
            }
            else
            {
                diag_add(r, "poweroff", 0, "/sbin/poweroff 不存在");
            }
        }
    }

    /* battery 节点全列表: 发现驱动暴露的未知节点 */
    {
        static char node_list[640];
        node_list[0] = '\0';
        DIR *bd = opendir("/sys/class/power_supply/battery");
        if (bd)
        {
            struct dirent *be;
            while ((be = readdir(bd)) != NULL)
            {
                if (be->d_name[0] == '.')
                    continue;
                int n = strlen(node_list);
                snprintf(node_list + n, sizeof(node_list) - n, "%s ", be->d_name);
            }
            closedir(bd);
            if (node_list[0])
                diag_add(r, "电池节点表", 1, node_list);
        }
    }
}

/* ---- msg_server power 插件拉取: 低电阈值(VOL_POWER_OFF)逆向取证.
 * 插件 so 由 msg_server 运行时 dlopen, 快照未收录; 复制到 /var/upgrade
 * 后可经 xwebd /api/files/download 取回本机反汇编. ---- */
static int diag_copy_file(const char *src, const char *dst)
{
    int in = open(src, O_RDONLY);
    if (in < 0)
        return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0)
    {
        close(in);
        return -1;
    }
    char buf[2048];
    int n;
    while ((n = read(in, buf, sizeof(buf))) > 0)
    {
        if (write(out, buf, n) != n)
        {
            close(in);
            close(out);
            return -1;
        }
    }
    close(in);
    close(out);
    return (n < 0) ? -1 : 0;
}

static void check_power_plugin(diag_result_t *r)
{
    static char list_msg[640];
    static char copy_msg[320];
    list_msg[0] = '\0';
    copy_msg[0] = '\0';

    DIR *d = opendir("/usr/bin/plugins");
    if (!d)
    {
        diag_add(r, "插件目录", 0, "/usr/bin/plugins 不可读");
        return;
    }
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d)) != NULL && found < 24)
    {
        if (e->d_name[0] == '.')
            continue;
        char sub[192];
        snprintf(sub, sizeof(sub), "/usr/bin/plugins/%.100s", e->d_name);
        DIR *ds = opendir(sub);
        if (ds)
        {
            struct dirent *es;
            while ((es = readdir(ds)) != NULL)
            {
                if (es->d_name[0] == '.')
                    continue;
                int n = strlen(list_msg);
                snprintf(list_msg + n, sizeof(list_msg) - n, "%s/%s ", e->d_name, es->d_name);
                found++;
                /* 复制 power 相关插件供逆向 */
                if (strstr(es->d_name, "power") && !copy_msg[0])
                {
                    char src[320];
                    snprintf(src, sizeof(src), "%s/%.150s", sub, es->d_name);
                    if (diag_copy_file(src, "/var/upgrade/dump_power_plugin.so") == 0)
                    {
                        struct stat st;
                        stat("/var/upgrade/dump_power_plugin.so", &st);
                        snprintf(copy_msg, sizeof(copy_msg),
                                 "已复制 %s (%ldB)", es->d_name, (long)st.st_size);
                    }
                }
            }
            closedir(ds);
        }
        else
        {
            int n = strlen(list_msg);
            snprintf(list_msg + n, sizeof(list_msg) - n, "%s ", e->d_name);
            found++;
        }
    }
    closedir(d);
    diag_add(r, "插件目录", 1, list_msg);
    if (copy_msg[0])
        diag_add(r, "power插件", 1, copy_msg);
}

/* ---- msg_server 运行时 so 列表: 低电关机逻辑可能在 dlopen 的插件 so 中
 * (静态快照未收录), 读 /proc/PID/maps 提取全部 .so 路径 ---- */
static int diag_find_pid_by_comm(const char *comm)
{
    DIR *p = opendir("/proc");
    if (!p)
        return -1;
    struct dirent *e;
    int found = -1;
    while ((e = readdir(p)) != NULL)
    {
        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        char path[64];
        char name[64] = {0};
        snprintf(path, sizeof(path), "/proc/%.12s/comm", e->d_name);
        int fd = open(path, O_RDONLY);
        if (fd < 0)
            continue;
        int n = read(fd, name, sizeof(name) - 1);
        close(fd);
        if (n > 0)
        {
            name[n] = '\0';
            char *nl = strchr(name, '\n');
            if (nl)
                *nl = '\0';
            if (strcmp(name, comm) == 0)
            {
                found = atoi(e->d_name);
                break;
            }
        }
    }
    closedir(p);
    return found;
}

static void check_msg_server_maps(diag_result_t *r)
{
    static char so_list[2048];
    so_list[0] = '\0';

    int pid = diag_find_pid_by_comm("msg_server");
    if (pid < 0)
    {
        diag_add(r, "msg_server", 0, "进程未找到");
        return;
    }

    char path[48];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        diag_add(r, "msg_server", 0, "maps 不可读");
        return;
    }

    char buf[4096];
    int n;
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0)
    {
        buf[n] = '\0';
        char *p = buf;
        while (p && *p)
        {
            char *eol = strchr(p, '\n');
            if (eol)
                *eol = '\0';
            char *so = strstr(p, ".so");
            if (so)
            {
                /* 提取行内路径(最后一个 '/' 到 .so+3) */
                char *slash = NULL;
                for (char *q = so; q >= p; q--)
                    if (*q == '/')
                    {
                        slash = q;
                        break;
                    }
                if (slash)
                {
                    char fullpath[128];
                    int len = (int)(so + 3 - slash);
                    if (len < (int)sizeof(fullpath))
                    {
                        memcpy(fullpath, slash, len);
                        fullpath[len] = '\0';
                        /* 去重: so_list 中未出现才追加 */
                        if (!strstr(so_list, fullpath))
                        {
                            int cl = strlen(so_list);
                            snprintf(so_list + cl, sizeof(so_list) - cl, "%s ", fullpath);
                        }
                    }
                }
            }
            if (!eol)
                break;
            p = eol + 1;
        }
    }
    close(fd);

    if (so_list[0])
        diag_add(r, "msg_server so", 1, so_list);
    else
        diag_add(r, "msg_server so", 0, "无 so 映射");
}

/* ---- 开机每日推送动画 (bot_push) 取证: 播放链为
 * olmedia_service 请求 cloud.zhibankeji.com/push/v1/push/pushBot
 *   -> 下载 swf 到 /tmp/bot_push.swf (download_service 完成事件 msg 440)
 *   -> olmedia 广播 MSG_POWER_ON_PUSH(372) 载荷为 swf 路径
 *   -> manager set_config(APPLIB_NEXT_APP,"boot_push /tmp/bot_push.swf") 并 exec /usr/lib/boot_push.so 播放
 * 此处 stat 该文件 + 扫描相关进程, 确认开关方案的拦截点 ---- */
static void check_boot_push(diag_result_t *r)
{
    static char f_msg[160];
    struct stat st;

    if (stat("/tmp/bot_push.swf", &st) == 0)
    {
        long age_s = (long)time(NULL) - (long)st.st_mtime;
        snprintf(f_msg, sizeof(f_msg), "存在 size=%ldB mtime距今=%lds",
                 (long)st.st_size, age_s);
    }
    else
    {
        snprintf(f_msg, sizeof(f_msg), "不存在(errno=%d)", errno);
    }
    diag_add(r, "bot_push.swf", 1, f_msg);

    /* 相关进程存活扫描 */
    {
        static const char *names[] = {"boot_push", "olmedia_service", "launcher"};
        static char p_msg[96];
        p_msg[0] = '\0';
        for (unsigned int i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        {
            int pid = diag_find_pid_by_comm(names[i]);
            int n = strlen(p_msg);
            snprintf(p_msg + n, sizeof(p_msg) - n, "%s=%d ", names[i], pid);
        }
        diag_add(r, "push进程", 1, p_msg);
    }

    /* /tmp 根目录文件速览(找 swf/推送残留) */
    {
        static char t_msg[512];
        t_msg[0] = '\0';
        DIR *td = opendir("/tmp");
        if (td)
        {
            struct dirent *te;
            int shown = 0;
            while ((te = readdir(td)) != NULL && shown < 20)
            {
                if (te->d_name[0] == '.')
                    continue;
                if (strstr(te->d_name, ".swf") || strstr(te->d_name, "push") ||
                    strstr(te->d_name, ".cfg") || strstr(te->d_name, "wpa"))
                {
                    int n = strlen(t_msg);
                    snprintf(t_msg + n, sizeof(t_msg) - n, "%s ", te->d_name);
                    shown++;
                }
            }
            closedir(td);
        }
        if (t_msg[0])
            diag_add(r, "tmp关键文件", 1, t_msg);
    }
}

diag_result_t diag_run_all(void)
{
    diag_result_t result;
    memset(&result, 0, sizeof(result));

    PLOG_I(TAG, "开始执行运行时健康检查...");

    check_config_dir(&result);
    check_log(&result);
    check_watchdog(&result);
    check_websocket(&result);
    check_battery(&result);
    check_power_plugin(&result);
    check_msg_server_maps(&result);
    check_boot_push(&result);

    int ok_count = 0, fail_count = 0;
    for (int i = 0; i < result.count; i++)
    {
        if (result.items[i].ok)
            ok_count++;
        else
            fail_count++;
    }
    snprintf(result.summary, sizeof(result.summary), "%d项正常, %d项异常", ok_count, fail_count);

    PLOG_I(TAG, "运行时健康检查完成: %s", result.summary);
    return result;
}

static void json_escape(const char *src, char *dst, int dst_size)
{
    int j = 0;
    for (int i = 0; src[i] && j < dst_size - 2; i++)
    {
        if (src[i] == '"' || src[i] == '\\')
        {
            if (j + 2 >= dst_size)
                break;
            dst[j++] = '\\';
            dst[j++] = src[i];
        }
        else if (src[i] == '\n')
        {
            if (j + 2 >= dst_size)
                break;
            dst[j++] = '\\';
            dst[j++] = 'n';
        }
        else if (src[i] == '\r')
        {
            if (j + 2 >= dst_size)
                break;
            dst[j++] = '\\';
            dst[j++] = 'r';
        }
        else
        {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
}

int diag_result_to_json(const diag_result_t *result, char *buf, int buf_size)
{
    int ok_count = 0, fail_count = 0;
    for (int i = 0; i < result->count; i++)
    {
        if (result->items[i].ok)
            ok_count++;
        else
            fail_count++;
    }

    int pos = 0;
    pos += snprintf(buf + pos, buf_size - pos,
                    "{\"ok_count\":%d,\"fail_count\":%d,\"total\":%d,\"summary\":\"%s\",\"items\":[",
                    ok_count, fail_count, result->count, result->summary);

    for (int i = 0; i < result->count; i++)
    {
        if (i > 0)
            pos += snprintf(buf + pos, buf_size - pos, ",");
        char esc_msg[256];
        json_escape(result->items[i].message, esc_msg, sizeof(esc_msg));
        pos += snprintf(buf + pos, buf_size - pos,
                        "{\"name\":\"%s\",\"ok\":%s,\"message\":\"%s\"}",
                        result->items[i].name,
                        result->items[i].ok ? "true" : "false",
                        esc_msg);
        if (pos >= buf_size - 1)
            break;
    }

    pos += snprintf(buf + pos, buf_size - pos, "]}");
    if (pos >= buf_size)
        pos = buf_size - 1;
    return pos;
}
