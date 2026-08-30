/**
 * @file xwplug_procs.c
 * @brief 进程管理插件（《09 方案》B2）
 *
 * 端点(经 /api/plugin/procs/<sub> 或旧路径改写访问):
 *   GET  /list                      -> 已知进程清单+运行状态/RSS
 *   POST /control {"name","action"} -> stop/start/restart(限可控进程)
 *
 * services(服务开关)不迁: 管理 xwebd 自身/sair 生命周期, 属核心职责。
 * kill 白名单与 xwebd 内置 g_known_processes 同源(controllable 位)。
 */

#include "xwplug_common.h"
#include <sys/wait.h>
#include <dirent.h>

typedef struct {
    const char *name;
    const char *desc;
    int controllable;
    const char *category;
} process_info_t;

static const process_info_t k_known[] = {
    {"launcher",         "UI启动器，管理屏幕界面和音量条",     1, "系统"},
    {"sair_main",        "语音助手(自定义版)",                  0, "核心"},
    {"sair",             "语音助手(原生版)",                    0, "核心"},
    {"audio_service",    "音频服务，提供录音和播放接口",        1, "核心"},
    {"msg_server",       "消息总线，进程间通信",                1, "系统"},
    {"wifiNetd",         "WiFi网络守护进程",                    0, "核心"},
    {"xwebd",            "面板内核HTTP服务",                    0, "核心"},
    {"manager",          "进程管理器，启动和管理系统服务",      0, "核心"},
    {"music_player",     "本地音乐播放器(原版sair依赖)",        1, "可选"},
    {"smart_player",     "智能播放器，整合sair/mqtt/在线音乐",  1, "可选"},
    {"olmedia_service",  "在线媒体服务(原版sair依赖)",          1, "可选"},
    {"mqtt_handle",      "MQTT消息处理(原版sair依赖)",          1, "可选"},
    {"mqtt_custom_server","MQTT自定义服务(原版sair依赖)",       1, "可选"},
    {"xwplug-battery",   "电池守护插件",                        0, "插件"},
    {"xwplug-bat-c",     "电池采样子进程",                      0, "插件"},
    {"xwplug-files",     "文件管理插件",                        0, "插件"},
    {"xwplug-procs",     "进程管理插件(自身)",                  0, "插件"},
    {"xwplug-usb",       "USB模式插件",                         0, "插件"},
    {"xwplug-demo",      "演示插件",                            0, "插件"},
    {"xwplug-lights",    "灯控插件(背光/呼吸灯/按键背光)",      0, "插件"},
    {"xwplug-lights-r",  "灯控插件呼吸灯恢复子进程",            0, "插件"},
    {"telnetd",          "Telnet终端服务",                      0, "插件"},
    {NULL, NULL, 0, NULL}
};

static const process_info_t *find_info(const char *name)
{
    for (int i = 0; k_known[i].name; i++)
        if (strcmp(name, k_known[i].name) == 0) return &k_known[i];
    return NULL;
}

static int ep_list(const xwplug_req_t *req, char *resp, int resp_size)
{
    (void)req;
    char running_names[32][64];
    int running_pids[32];
    int running_rss[32];
    int running_count = 0;

    /* 直接扫 /proc(不 shell out, 插件自包含) */
    DIR *d = opendir("/proc");
    if (!d) { snprintf(resp, resp_size, "{\"error\":\"cannot open proc\"}"); return 500; }
    struct dirent *e;
    while ((e = readdir(d)) != NULL && running_count < 32) {
        char *end;
        long pid = strtol(e->d_name, &end, 10);
        if (*end != '\0' || pid <= 0) continue;

        char path[64], line[256];
        snprintf(path, sizeof(path), "/proc/%ld/status", pid);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        char name[64] = "";
        int rss = 0;
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "Name:", 5) == 0) {
                char *p = line + 5;
                while (*p == ' ' || *p == '\t') p++;
                sscanf(p, "%63s", name);
            } else if (strncmp(line, "VmRSS:", 6) == 0) {
                rss = atoi(line + 6);
            }
        }
        fclose(f);
        if (!name[0] || rss <= 0) continue;
        if (!find_info(name)) continue;

        snprintf(running_names[running_count], 64, "%s", name);
        running_pids[running_count] = (int)pid;
        running_rss[running_count] = rss;
        running_count++;
    }
    closedir(d);

    int pos = 0;
    APPEND_PRINTF(resp, pos, resp_size, "{\"processes\":[");
    int first = 1;
    for (int i = 0; k_known[i].name; i++) {
        const char *pname = k_known[i].name;
        int found = 0;
        for (int j = 0; j < running_count; j++) {
            if (strcmp(pname, running_names[j]) != 0) continue;
            APPEND_PRINTF(resp, pos, resp_size, "%s", first ? "" : ",");
            first = 0;
            APPEND_PRINTF(resp, pos, resp_size,
                "{\"name\":\"%s\",\"pid\":%d,\"rss\":%d,\"desc\":\"%s\","
                "\"category\":\"%s\",\"controllable\":%s,\"running\":true}",
                pname, running_pids[j], running_rss[j],
                k_known[i].desc, k_known[i].category,
                k_known[i].controllable ? "true" : "false");
            found = 1;
            break;
        }
        if (!found) {
            APPEND_PRINTF(resp, pos, resp_size, "%s", first ? "" : ",");
            first = 0;
            APPEND_PRINTF(resp, pos, resp_size,
                "{\"name\":\"%s\",\"pid\":0,\"rss\":0,\"desc\":\"%s\","
                "\"category\":\"%s\",\"controllable\":%s,\"running\":false}",
                pname, k_known[i].desc, k_known[i].category,
                k_known[i].controllable ? "true" : "false");
        }
    }
    APPEND_PRINTF(resp, pos, resp_size, "]}");
    return 200;
}

static int ep_control(const xwplug_req_t *req, char *resp, int resp_size)
{
    char name[64] = "";
    char action[16] = "";
    xwplug_json_str(req->body, "name", name, sizeof(name));
    xwplug_json_str(req->body, "action", action, sizeof(action));

    if (!name[0] || !action[0]) {
        snprintf(resp, resp_size, "{\"error\":\"Missing name or action\"}");
        return 400;
    }
    const process_info_t *info = find_info(name);
    if (!info) { snprintf(resp, resp_size, "{\"error\":\"Unknown process\"}"); return 404; }
    if (!info->controllable) { snprintf(resp, resp_size, "{\"error\":\"Process not controllable\"}"); return 403; }

    if (strcmp(action, "stop") == 0) {
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "killall %s 2>/dev/null", name);
        int rc = system(cmd);
        snprintf(resp, resp_size, "{\"ok\":true,\"action\":\"stop\",\"name\":\"%s\",\"rc\":%d}", name, rc);
        return 200;
    }
    if (strcmp(action, "start") == 0 || strcmp(action, "restart") == 0) {
        if (strcmp(action, "restart") == 0) {
            char cmd[128];
            snprintf(cmd, sizeof(cmd), "killall %s 2>/dev/null", name);
            system(cmd);
            usleep(500000);
        }
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "/usr/bin/%s &", name);
        int rc = system(cmd);
        snprintf(resp, resp_size, "{\"ok\":%s,\"action\":\"%s\",\"name\":\"%s\",\"rc\":%d}",
                 rc == 0 ? "true" : "false", action, name, rc);
        return 200;
    }
    snprintf(resp, resp_size, "{\"error\":\"Invalid action\"}");
    return 400;
}

int plug_handle(const xwplug_req_t *req, char *resp, int resp_size)
{
    if (strcmp(req->path, "/list") == 0 && strcmp(req->method, "GET") == 0)
        return ep_list(req, resp, resp_size);
    if (strcmp(req->path, "/control") == 0 && strcmp(req->method, "POST") == 0)
        return ep_control(req, resp, resp_size);
    snprintf(resp, resp_size, "{\"error\":\"not found\",\"path\":\"%s\"}", req->path);
    return 404;
}

XWPLUG_MAIN("procs")
