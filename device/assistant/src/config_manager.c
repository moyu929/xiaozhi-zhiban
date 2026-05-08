/**
 * @file config_manager.c
 * @brief 小智助手配置管理模块实现
 *
 * 本文件实现了设备配置管理功能：
 * - 读取平台配置（唤醒词、唤醒阈值、监听模式）
 * - WiFi连接状态检测（优先使用WiFi库，回退到sysfs）
 * - 设备激活状态检查（OTA接口，获取WebSocket配置）
 * - WebSocket配置缓存（本地文件读写）
 * - 设备MAC地址获取
 * - 客户端ID生成与持久化（UUID v4格式）
 * - 配置热重载
 */

#include "config_manager.h"
#include "xiaozhi_config.h"
#include "plog.h"
#include "http_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dlfcn.h>
#include <time.h>
#include <fcntl.h>

/* 平台SDK外部接口：获取键值配置 */
extern int get_config(const char* key, char* value, int value_size);

/**
 * @brief 初始化配置管理器
 *        从平台配置中读取唤醒词、阈值、监听模式等
 *        获取设备MAC地址和客户端ID
 * @param cfg 配置管理器指针
 * @return 0成功，-1参数无效
 */
int config_manager_init(config_manager_t* cfg) {
    if (!cfg) return -1;
    memset(cfg, 0, sizeof(config_manager_t));

    /* 读取唤醒词配置，未设置则使用默认值 */
    if (get_config("MAIN_WAKE_WORD", cfg->wake_word, sizeof(cfg->wake_word)) != 0 ||
        cfg->wake_word[0] == '\0') {
        strncpy(cfg->wake_word,
                "zhi ban zhi ban,sheng yin da yi dian,sheng yin xiao yi dian,"
                "ting zhi bo fang,ji xu bo fang,shang yi shou,xia yi shou,ni hao zhi dao",
                sizeof(cfg->wake_word) - 1);
    }
    PLOG_I("CFG", "唤醒词: %s", cfg->wake_word);

    /* 读取唤醒阈值配置 */
    if (get_config("MAIN_WAKE_THRESH", cfg->wake_thresh, sizeof(cfg->wake_thresh)) != 0) {
        cfg->wake_thresh[0] = '\0';
    }

    /* 读取监听模式配置：realtime=实时模式, 其他=自动停止模式 */
    {
        char mode_str[32] = {0};
        if (get_config("XIAOZHI_LISTEN_MODE", mode_str, sizeof(mode_str)) == 0 && mode_str[0]) {
            if (strcmp(mode_str, "realtime") == 0) {
                cfg->realtime_mode = 1;
            } else {
                cfg->realtime_mode = 0;
            }
        } else {
            cfg->realtime_mode = 0;
        }
    }

    /* 获取设备MAC地址和客户端ID */
    config_manager_get_mac(cfg->device_mac, sizeof(cfg->device_mac));
    config_manager_get_or_create_client_id(cfg->client_id, sizeof(cfg->client_id));

    PLOG_I("CFG", "device_mac: %s", cfg->device_mac);
    PLOG_I("CFG", "client_id: %s", cfg->client_id);
    PLOG_I("CFG", "realtime_mode: %d", cfg->realtime_mode);

    return 0;
}

/**
 * @brief 销毁配置管理器（当前无资源需释放）
 * @param cfg 配置管理器指针
 */
void config_manager_destroy(config_manager_t* cfg) {
    (void)cfg;
}

/**
 * @brief 检查WiFi连接状态
 *        优先使用libwifi_client.so库函数，回退到读取sysfs文件
 * @return 1已连接，0未连接
 */
int config_manager_check_wifi(void) {
    typedef int (*wifi_get_wifi_info_func_t)(void* info);
    static wifi_get_wifi_info_func_t wifi_func = NULL;
    static int func_checked = 0;

    /* 延迟加载WiFi库（仅首次调用时dlopen） */
    if (!func_checked) {
        func_checked = 1;
        void* lib = dlopen("libwifi_client.so", RTLD_LAZY);
        if (lib) {
            wifi_func = (wifi_get_wifi_info_func_t)dlsym(lib, "wifi_get_wifi_info");
        }
    }

    if (wifi_func) {
        /* 通过WiFi库获取连接状态 */
        char buf[WIFI_INFO_SIZE];
        memset(buf, 0, sizeof(buf));
        int ret = wifi_func(buf);
        if (ret != 0) return 0;
        int status = *(int*)(buf + 4);
        return (status == WIFI_STATE_CONNECTED);
    }

    /* 回退方案：读取wlan0操作状态 */
    FILE* fp = fopen("/sys/class/net/wlan0/operstate", "r");
    if (fp) {
        char state[32] = {0};
        if (fgets(state, sizeof(state), fp)) {
            fclose(fp);
            if (strstr(state, "up") || strstr(state, "unknown")) {
                return 1;
            }
        } else {
            fclose(fp);
        }
    }
    return 0;
}

/**
 * @brief 从缓存文件加载WebSocket配置
 * @param cfg 配置管理器指针
 * @return 0成功，-1缓存文件不存在或内容无效
 */
static int load_ws_config_cache(config_manager_t* cfg) {
    FILE* fp = fopen("/var/upgrade/.ws_config", "r");
    if (!fp) return -1;

    char line1[512] = {0};
    char line2[512] = {0};
    if (fgets(line1, sizeof(line1), fp)) {
        int len = strlen(line1);
        while (len > 0 && (line1[len-1] == '\n' || line1[len-1] == '\r')) line1[--len] = '\0';
    }
    if (fgets(line2, sizeof(line2), fp)) {
        int len = strlen(line2);
        while (len > 0 && (line2[len-1] == '\n' || line2[len-1] == '\r')) line2[--len] = '\0';
    }
    fclose(fp);

    if (line1[0] && line2[0]) {
        strncpy(cfg->ws_url, line1, sizeof(cfg->ws_url) - 1);
        strncpy(cfg->ws_token, line2, sizeof(cfg->ws_token) - 1);
        cfg->has_ws_config = 1;
        PLOG_I("CFG", "已加载ws配置缓存: url=%s", cfg->ws_url);
        return 0;
    }

    return -1;
}

/**
 * @brief 保存WebSocket配置到缓存文件
 * @param url WebSocket URL
 * @param token 认证令牌
 * @return 0成功，-1文件打开失败
 */
static int save_ws_config_cache(const char* url, const char* token) {
    FILE* fp = fopen("/var/upgrade/.ws_config", "w");
    if (!fp) return -1;
    fprintf(fp, "%s\n%s\n", url, token);
    fclose(fp);
    PLOG_I("CFG", "已保存ws配置缓存");
    return 0;
}

/**
 * @brief 从JSON字符串中提取指定键的字符串值
 *        简易JSON解析，支持转义字符
 * @param json JSON数据指针
 * @param len JSON数据长度
 * @param key 要查找的键名
 * @param out 输出缓冲区
 * @param out_size 输出缓冲区大小
 * @return 输出缓冲区指针（成功），NULL（未找到）
 */
static const char* find_json_str(const char* json, size_t len, const char* key, char* out, int out_size) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char* p = memmem(json, len, search, strlen(search));
    if (!p) return NULL;
    p += strlen(search);
    while (p < json + len && (*p == ' ' || *p == ':' || *p == '\t')) p++;
    if (p >= json + len || *p != '"') return NULL;
    p++;
    int i = 0;
    while (p < json + len && *p != '"' && i < out_size - 1) {
        if (*p == '\\' && p + 1 < json + len) { p++; out[i++] = *p++; }
        else { out[i++] = *p++; }
    }
    out[i] = '\0';
    return out;
}

/**
 * @brief 检查设备激活状态并获取WebSocket配置
 *        向OTA服务器发送POST请求，解析返回的websocket配置和激活码
 *        请求失败时尝试使用缓存或默认URL
 * @param cfg 配置管理器指针
 * @return 0成功（无论是否获取到配置），-1参数无效
 */
int config_manager_check_activation(config_manager_t* cfg) {
    if (!cfg) return -1;

    /* 构造OTA请求头 */
    char headers[1024];
    snprintf(headers, sizeof(headers),
        "Activation-Version: 1\r\n"
        "Device-Id: %s\r\n"
        "Client-Id: %s\r\n"
        "User-Agent: gs705b/1.0.0\r\n"
        "Accept-Language: zh-CN\r\n",
        cfg->device_mac, cfg->client_id);

    const char* body = "{\"version\":2,\"chip_model_name\":\"gs705b\"}";

    PLOG_I("CFG", "OTA检查: POST %s", DEFAULT_OTA_URL);

    http_response_t response;
    memset(&response, 0, sizeof(response));

    int ret = http_post(DEFAULT_OTA_URL, headers, body, &response);
    if (ret != 0 || response.status_code != 200 || !response.body) {
        PLOG_W("CFG", "OTA检查失败: ret=%d status=%d", ret, response.status_code);

        /* 请求失败时使用缓存配置 */
        if (cfg->has_ws_config) {
            PLOG_I("CFG", "使用缓存的ws配置");
            cfg->needs_activation = 0;
        } else {
            /* 无缓存则使用默认URL */
            strncpy(cfg->ws_url, DEFAULT_WS_URL, sizeof(cfg->ws_url) - 1);
            cfg->ws_token[0] = '\0';
            cfg->has_ws_config = 1;
            cfg->needs_activation = 1;
            PLOG_W("CFG", "无ws配置可用, 使用默认URL");
        }

        http_response_free(&response);
        return 0;
    }

    PLOG_I("CFG", "OTA响应: %s", response.body);

    /* 解析OTA响应中的websocket配置和激活码 */
    char ws_url[512] = {0};
    char ws_token[512] = {0};
    char activation_code[64] = {0};

    const char* ws_start = memmem(response.body, response.body_len, "\"websocket\"", 11);
    if (ws_start) {
        find_json_str(ws_start, response.body_len - (ws_start - response.body), "url", ws_url, sizeof(ws_url));
        find_json_str(ws_start, response.body_len - (ws_start - response.body), "token", ws_token, sizeof(ws_token));
    }

    const char* act_start = memmem(response.body, response.body_len, "\"activation\"", 12);
    if (act_start) {
        find_json_str(act_start, response.body_len - (act_start - response.body), "code", activation_code, sizeof(activation_code));
    }

    if (ws_url[0] && ws_token[0]) {
        /* 设备已激活，获取到WebSocket配置 */
        strncpy(cfg->ws_url, ws_url, sizeof(cfg->ws_url) - 1);
        strncpy(cfg->ws_token, ws_token, sizeof(cfg->ws_token) - 1);
        cfg->has_ws_config = 1;
        cfg->needs_activation = 0;

        save_ws_config_cache(ws_url, ws_token);

        PLOG_I("CFG", "OTA: 设备已激活, ws_url=%s", ws_url);
    } else if (activation_code[0]) {
        /* 设备未激活，需要用户输入激活码 */
        cfg->needs_activation = 1;
        PLOG_W("CFG", "OTA: 设备未激活, 激活码=%s", activation_code);

        if (cfg->has_ws_config) {
            PLOG_I("CFG", "使用缓存的ws配置");
        }
    } else {
        /* 响应格式异常 */
        PLOG_W("CFG", "OTA: 响应格式异常");

        if (!cfg->has_ws_config) {
            strncpy(cfg->ws_url, DEFAULT_WS_URL, sizeof(cfg->ws_url) - 1);
            cfg->ws_token[0] = '\0';
            cfg->has_ws_config = 1;
        }
    }

    http_response_free(&response);
    return 0;
}

/**
 * @brief 获取设备MAC地址
 *        依次尝试从wlan0和eth0读取，跳过全零地址
 * @param buf 输出缓冲区
 * @param buf_size 缓冲区大小
 * @return 0成功，-1未获取到有效MAC地址
 */
int config_manager_get_mac(char* buf, int buf_size) {
    const char* paths[] = {
        "/sys/class/net/wlan0/address",
        "/sys/class/net/eth0/address",
        NULL
    };

    for (int i = 0; paths[i]; i++) {
        FILE* fp = fopen(paths[i], "r");
        if (!fp) continue;
        if (fgets(buf, buf_size, fp)) {
            int len = strlen(buf);
            while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
                buf[--len] = '\0';
            }
            fclose(fp);
            /* 跳过全零MAC地址 */
            if (len > 0 && strncmp(buf, "00:00:00:00:00:00", 17) != 0) {
                return 0;
            }
        } else {
            fclose(fp);
        }
    }

    /* 未获取到有效MAC，使用全零默认值 */
    strncpy(buf, "00:00:00:00:00:00", buf_size - 1);
    return -1;
}

/**
 * @brief 获取或创建客户端ID
 *        从本地文件读取已有的客户端ID，如不存在则生成新的UUID v4格式ID并保存
 *        ID格式: xxxxxxxx-xxxx-4xxx-8xxx-xxxxxxxxxxxx
 * @param buf 输出缓冲区
 * @param buf_size 缓冲区大小
 * @return 0成功
 */
int config_manager_get_or_create_client_id(char* buf, int buf_size) {
    const char* path = "/var/upgrade/.client_id";
    /* 尝试读取已有的客户端ID */
    FILE* fp = fopen(path, "r");
    if (fp) {
        if (fgets(buf, buf_size, fp)) {
            int len = strlen(buf);
            while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' ')) {
                buf[--len] = '\0';
            }
            /* UUID长度至少36字符（含连字符） */
            if (len >= 36) {
                fclose(fp);
                return 0;
            }
        }
        fclose(fp);
    }

    /* 生成新的UUID v4格式客户端ID */
    int fd = open("/dev/urandom", O_RDONLY);
    unsigned int r1 = 0, r2 = 0, r3 = 0, r4 = 0, r5 = 0, r6 = 0;
    if (fd >= 0) {
        read(fd, &r1, 4); read(fd, &r2, 4); read(fd, &r3, 4);
        read(fd, &r4, 4); read(fd, &r5, 4); read(fd, &r6, 4);
        close(fd);
    } else {
        /* urandom不可用时使用伪随机数 */
        srand(time(NULL));
        r1 = rand(); r2 = rand(); r3 = rand();
        r4 = rand(); r5 = rand(); r6 = rand();
    }

    snprintf(buf, buf_size, "%08x-%04x-%04x-%04x-%08x%04x",
             r1, r2 & 0xFFFF, (r3 & 0x0FFF) | 0x4000,
             (r4 & 0x3FFF) | 0x8000, r5, r6 & 0xFFFF);

    /* 保存到文件 */
    fp = fopen(path, "w");
    if (fp) {
        fprintf(fp, "%s\n", buf);
        fclose(fp);
    }
    return 0;
}

/**
 * @brief 重新加载配置
 *        从平台配置和缓存文件重新读取所有配置项
 *        检测各项配置是否有变化
 * @param cfg 配置管理器指针
 * @return 变更的配置项数量，0表示无变更，-1参数无效
 */
int config_manager_reload(config_manager_t* cfg) {
    if (!cfg) return -1;

    int changed = 0;

    /* 重新加载唤醒词 */
    char new_wake_word[256] = {0};
    if (get_config("MAIN_WAKE_WORD", new_wake_word, sizeof(new_wake_word)) != 0 ||
        new_wake_word[0] == '\0') {
        strncpy(new_wake_word,
                "zhi ban zhi ban,sheng yin da yi dian,sheng yin xiao yi dian,"
                "ting zhi bo fang,ji xu bo fang,shang yi shou,xia yi shou,ni hao zhi dao",
                sizeof(new_wake_word) - 1);
    }
    if (strcmp(cfg->wake_word, new_wake_word) != 0) {
        PLOG_I("CFG", "唤醒词变更: [%s] -> [%s]", cfg->wake_word, new_wake_word);
        strncpy(cfg->wake_word, new_wake_word, sizeof(cfg->wake_word) - 1);
        changed = 1;
    }

    /* 重新加载唤醒阈值 */
    char new_wake_thresh[64] = {0};
    if (get_config("MAIN_WAKE_THRESH", new_wake_thresh, sizeof(new_wake_thresh)) != 0) {
        new_wake_thresh[0] = '\0';
    }
    if (strcmp(cfg->wake_thresh, new_wake_thresh) != 0) {
        PLOG_I("CFG", "唤醒阈值变更: [%s] -> [%s]", cfg->wake_thresh, new_wake_thresh);
        strncpy(cfg->wake_thresh, new_wake_thresh, sizeof(cfg->wake_thresh) - 1);
        changed = 1;
    }

    /* 重新加载监听模式 */
    {
        char mode_str[32] = {0};
        int new_mode = 0;
        if (get_config("XIAOZHI_LISTEN_MODE", mode_str, sizeof(mode_str)) == 0 && mode_str[0]) {
            if (strcmp(mode_str, "realtime") == 0) {
                new_mode = 1;
            }
        }
        if (cfg->realtime_mode != new_mode) {
            PLOG_I("CFG", "监听模式变更: %d -> %d", cfg->realtime_mode, new_mode);
            cfg->realtime_mode = new_mode;
            changed = 1;
        }
    }

    /* 重新加载WebSocket配置缓存 */
    {
        FILE* fp = fopen("/var/upgrade/.ws_config", "r");
        if (fp) {
            char line1[512] = {0}, line2[512] = {0};
            if (fgets(line1, sizeof(line1), fp)) {
                int len = strlen(line1);
                while (len > 0 && (line1[len-1] == '\n' || line1[len-1] == '\r')) line1[--len] = '\0';
            }
            if (fgets(line2, sizeof(line2), fp)) {
                int len = strlen(line2);
                while (len > 0 && (line2[len-1] == '\n' || line2[len-1] == '\r')) line2[--len] = '\0';
            }
            fclose(fp);
            if (line1[0] && line2[0]) {
                if (strcmp(cfg->ws_url, line1) != 0 || strcmp(cfg->ws_token, line2) != 0) {
                    PLOG_I("CFG", "ws配置变更: url=[%s]->[%s]", cfg->ws_url, line1);
                    strncpy(cfg->ws_url, line1, sizeof(cfg->ws_url) - 1);
                    strncpy(cfg->ws_token, line2, sizeof(cfg->ws_token) - 1);
                    cfg->has_ws_config = 1;
                    changed = 1;
                }
            }
        }
    }

    if (changed) {
        PLOG_I("CFG", "配置已重载 (%d 项变更)", changed);
    } else {
        PLOG_D("CFG", "配置已重载, 无变更");
    }

    return changed;
}
