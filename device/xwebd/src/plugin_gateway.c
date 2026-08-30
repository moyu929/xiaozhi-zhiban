/**
 * @file plugin_gateway.c
 * @brief xwebd 插件网关实现（《09-插件化架构设计方案》B0）
 *
 * 职责：
 * - 发现: 周期扫描 XWPLUG_DIR 下 xwplug-* 二进制, 新增即 spawn
 * - 监控: 每 XWPLUG_PROBE_SEC 秒 connect 探测 sock; 插件死亡按
 *   指数退避(5/15/60s)重 spawn, 三连败标记 disabled(面板可手动重启)
 * - 转发: /api/plugin/<name>/<sub> 透传到 <name>.sock, 极简 HTTP 协议:
 *   请求 "<M> /<sub> HTTP/1.0\r\nContent-Length: N\r\n\r\n<json>"
 *   响应 "HTTP/1.0 <code> <msg>\r\nContent-Length: M\r\n\r\n<json>"
 * - 管理: 清单/重启/卸载/安装(从中转目录原子 rename)
 *
 * 隔离性: 插件崩溃只影响自身端点, xwebd 核心与 sair 不受影响。
 */

#include "plugin_gateway.h"
#include "xwebd_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <poll.h>
#include <time.h>

/* ---- xwebd.c 提供的公共工具(非 static 导出) ---- */
int xlog_write(int level, const char *tag, const char *fmt, ...);
int send_response(int fd, int status, const char *content_type, const char *body, int body_len);
int send_json(int fd, int status, const char *json);
int send_error(int fd, int code, const char *message);

#define XLOG_E(tag, fmt, ...) xlog_write(0, tag, fmt, ##__VA_ARGS__)
#define XLOG_W(tag, fmt, ...) xlog_write(1, tag, fmt, ##__VA_ARGS__)
#define XLOG_I(tag, fmt, ...) xlog_write(2, tag, fmt, ##__VA_ARGS__)
#define XLOG_D(tag, fmt, ...) xlog_write(3, tag, fmt, ##__VA_ARGS__)

#define APPEND_PRINTF(buf, pos, size, ...) do { \
    if ((pos) < (size) - 1) { \
        (pos) += snprintf((buf) + (pos), (size) - (pos), __VA_ARGS__); \
        if ((pos) >= (size)) (pos) = (size) - 1; \
    } \
} while (0)

#define TAG "PLUG"

typedef struct {
    char name[32];          /* 插件名(xwplug- 后缀) */
    char bin_path[160];     /* 完整二进制路径 */
    pid_t pid;
    int online;             /* 上次探测 sock 可连 */
    int disabled;           /* 三连退避失败, 停止自动重启 */
    int restarts;           /* 累计重 spawn 次数 */
    int backoff_sec;        /* 当前退避间隔, 0=首次 */
    time_t next_probe;      /* 下次探测时刻 */
    time_t next_spawn;      /* 退避到期时刻 */
    time_t last_seen;       /* 最近一次在线时刻(mem 采样) */
    int mem_kb;             /* 上次采样 RSS */
} xwplug_t;

static xwplug_t g_plugs[XWPLUG_MAX];
static int g_plug_count = 0;
static time_t g_last_scan = 0;

/* ---- 工具 ---- */

static int sock_path_of(const char *name, char *out, int out_size)
{
    return snprintf(out, out_size, "%s/%s.sock", XWPLUG_SOCK_DIR, name);
}

/* connect 探测 sock 是否有监听者; 0=在线 -1=离线 */
static int probe_sock(const char *sock_path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
    struct timeval tv = {1, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int ok = (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) ? 0 : -1;
    close(fd);
    return ok;
}

static int proc_rss_kb(pid_t pid)
{
    char path[48], line[128];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int kb = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VmRSS:", 6) == 0) { sscanf(line + 6, "%d", &kb); break; }
    }
    fclose(f);
    return kb;
}

static xwplug_t *find_plug(const char *name);

/* ---- spawn / 回收 ---- */

static pid_t xwplug_spawn(xwplug_t *p)
{
    char sock_path[72];
    sock_path_of(p->name, sock_path, sizeof(sock_path));
    unlink(sock_path); /* 残留 sock 防泄漏 */

    pid_t pid = fork();
    if (pid < 0) {
        XLOG_E(TAG, "spawn %s fork失败: %s", p->name, strerror(errno));
        return -1;
    }
    if (pid == 0) {
        /* 插件子进程: 输出重定向到统一日志, 父死随灭 */
        int logfd = open("/var/upgrade/xwplug.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (logfd >= 0) { dup2(logfd, 1); dup2(logfd, 2); close(logfd); }
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        execl(p->bin_path, p->bin_path, (char *)NULL);
        _exit(127);
    }
    p->pid = pid;
    p->online = 0;
    p->next_probe = time(NULL) + 1; /* 1s 后首探(等插件起监听) */
    XLOG_I(TAG, "已启动插件 %s (pid=%d)", p->name, pid);
    return pid;
}

static void stop_plug(xwplug_t *p, int hard)
{
    if (p->pid > 0) {
        kill(p->pid, hard ? SIGKILL : SIGTERM);
        /* 僵尸回收交给 tick 的 waitpid 扫描 */
    }
    char sock_path[72];
    sock_path_of(p->name, sock_path, sizeof(sock_path));
    unlink(sock_path);
    p->online = 0;
}

/* ---- 发现 ---- */

static xwplug_t *find_plug(const char *name)
{
    for (int i = 0; i < g_plug_count; i++)
        if (strcmp(g_plugs[i].name, name) == 0) return &g_plugs[i];
    return NULL;
}

static void scan_plugins(void)
{
    DIR *d = opendir(XWPLUG_DIR);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "xwplug-", 7) != 0) continue;
        const char *name = e->d_name + 7;
        if (!*name || strlen(name) >= sizeof(((xwplug_t *)0)->name)) continue;

        if (find_plug(name)) continue; /* 已登记 */

        if (g_plug_count >= XWPLUG_MAX) {
            XLOG_W(TAG, "插件数达上限 %d, 忽略 %s", XWPLUG_MAX, name);
            continue;
        }

        xwplug_t *p = &g_plugs[g_plug_count++];
        memset(p, 0, sizeof(*p));
        snprintf(p->name, sizeof(p->name), "%s", name);
        snprintf(p->bin_path, sizeof(p->bin_path), "%s/%.130s", XWPLUG_DIR, e->d_name);
        /* 安装时已 chmod +x, 防御性补一次 */
        chmod(p->bin_path, 0755);
        XLOG_I(TAG, "发现插件 %s, 启动", name);
        xwplug_spawn(p);
    }
    closedir(d);
}

/* ---- 周期维护(worker_loop 每 1s 调用) ---- */

void xwplug_tick(void)
{
    time_t now = time(NULL);

    if (now - g_last_scan >= XWPLUG_PROBE_SEC) {
        g_last_scan = now;
        scan_plugins();
    }

    for (int i = 0; i < g_plug_count; i++) {
        xwplug_t *p = &g_plugs[i];

        /* 僵尸回收: 进程死了 pid 清零 */
        if (p->pid > 0) {
            int status;
            pid_t r = waitpid(p->pid, &status, WNOHANG);
            if (r == p->pid) {
                XLOG_W(TAG, "插件 %s 退出(status=%d)", p->name, status);
                p->pid = 0;
                p->online = 0;
            } else if (r < 0) {
                p->pid = 0;
                p->online = 0;
            }
        }

        /* 探测 */
        if (now >= p->next_probe) {
            p->next_probe = now + XWPLUG_PROBE_SEC;
            char sock_path[72];
            sock_path_of(p->name, sock_path, sizeof(sock_path));
            int was_online = p->online;
            p->online = (probe_sock(sock_path) == 0);
            if (p->online) {
                p->last_seen = now;
                p->mem_kb = p->pid > 0 ? proc_rss_kb(p->pid) : 0;
                p->backoff_sec = 0; /* 在线即清退避 */
                p->disabled = 0;
                if (!was_online) XLOG_I(TAG, "插件 %s 上线", p->name);
            } else if (was_online) {
                XLOG_W(TAG, "插件 %s 掉线", p->name);
            }
        }

        /* 离线恢复: 退避到期重 spawn */
        if (!p->online && !p->disabled && now >= p->next_spawn && p->pid <= 0) {
            if (xwplug_spawn(p) > 0) {
                p->restarts++;
                if (p->backoff_sec == 0) p->backoff_sec = XWPLUG_BACKOFF_1;
                else if (p->backoff_sec == XWPLUG_BACKOFF_1) p->backoff_sec = XWPLUG_BACKOFF_2;
                else if (p->backoff_sec == XWPLUG_BACKOFF_2) p->backoff_sec = XWPLUG_BACKOFF_3;
                else { /* 三连败: 停止自动重启 */
                    p->disabled = 1;
                    XLOG_E(TAG, "插件 %s 三连败, 停止自动重启(面板可手动重启)", p->name);
                }
                p->next_spawn = now + (p->backoff_sec > 0 ? p->backoff_sec : XWPLUG_BACKOFF_1);
            }
        }
    }
}

/* ---- 转发 ---- */

/* 从插件读响应头, 解析状态码与 Content-Length */
static int parse_plug_response(char *buf, int len, int *code, int *body_off, int *body_len)
{
    /* "HTTP/1.0 <code> ..." */
    if (strncmp(buf, "HTTP/1.", 7) != 0) return -1;
    *code = atoi(buf + 9);
    if (*code <= 0) return -1;

    char *cl = strcasestr(buf, "Content-Length:");
    *body_len = cl ? atoi(cl + 15) : 0;

    char *sep = strstr(buf, "\r\n\r\n");
    if (!sep) return -1;
    *body_off = (int)(sep - buf) + 4;
    if (*body_off > len) return -1;
    return 0;
}

int xwplug_forward(int client_fd, const char *method, const char *plugin_path,
                   const char *query, const char *body, int body_len)
{
    /* plugin_path = "<name>/<sub...>"; 无 '/' 则无 sub */
    char name[32] = {0};
    const char *sub = "";
    const char *slash = strchr(plugin_path, '/');
    if (slash) {
        size_t nlen = slash - plugin_path;
        if (nlen == 0 || nlen >= sizeof(name)) return -1;
        memcpy(name, plugin_path, nlen);
        sub = slash + 1;
    } else {
        if (!*plugin_path) return -1;
        snprintf(name, sizeof(name), "%s", plugin_path);
    }

    xwplug_t *p = find_plug(name);
    char err[160];

    if (!p || p->disabled) {
        snprintf(err, sizeof(err), "{\"error\":\"plugin %s not installed\"}", name);
        return send_json(client_fd, 404, err), 1;
    }
    if (!p->online) {
        snprintf(err, sizeof(err), "{\"error\":\"plugin %s offline\",\"plugin\":\"%s\",\"restarts\":%d}",
                 name, name, p->restarts);
        return send_json(client_fd, 503, err), 1;
    }

    char sock_path[72];
    sock_path_of(name, sock_path, sizeof(sock_path));
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return send_json(client_fd, 502, "{\"error\":\"gateway socket fail\"}"), 1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        p->online = 0; /* 转发失败立即降级, 下轮 tick 重探 */
        return send_json(client_fd, 503, "{\"error\":\"plugin connect fail\"}"), 1;
    }

    /* 组请求: sub 带 query 拼回 */
    char req_head[512];
    int rh = snprintf(req_head, sizeof(req_head), "%s /%s%s%s HTTP/1.0\r\nContent-Length: %d\r\n\r\n",
                      method, sub, query ? "?" : "", query ? query : "", body ? body_len : 0);

    struct timeval tv = {XWEBD_REQUEST_TIMEOUT, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (write(fd, req_head, rh) != rh) goto fwd_fail;
    if (body && body_len > 0 && write(fd, body, body_len) != body_len) goto fwd_fail;

    /* 读响应(头+体) */
    {
        static char rbuf[XWEBD_RESP_BUF_SIZE];
        int total = 0;
        while (total < (int)sizeof(rbuf) - 1) {
            struct pollfd pfd = {fd, POLLIN, 0};
            int pret = poll(&pfd, 1, 1000);
            if (pret <= 0) continue; /* RCVTIMEO 兜底 */
            int n = read(fd, rbuf + total, sizeof(rbuf) - 1 - total);
            if (n <= 0) break;
            total += n;
            rbuf[total] = '\0';
            int code, body_off, body_len2;
            if (parse_plug_response(rbuf, total, &code, &body_off, &body_len2) == 0 &&
                total >= body_off + body_len2)
                break; /* 头体收齐 */
        }
        close(fd);

        int code, body_off, body_len2;
        if (total == 0 || parse_plug_response(rbuf, total, &code, &body_off, &body_len2) != 0) {
            XLOG_W(TAG, "插件 %s 响应不可解析(%dB)", name, total);
            return send_json(client_fd, 502, "{\"error\":\"plugin bad response\"}"), 1;
        }
        int avail = total - body_off;
        if (body_len2 > avail) body_len2 = avail;
        return send_response(client_fd, code, "application/json", rbuf + body_off, body_len2) == 0 ? 1 : -1;
    }

fwd_fail:
    close(fd);
    return send_json(client_fd, 502, "{\"error\":\"plugin write fail\"}"), 1;
}

/* ---- 管理端点 ---- */

int xwplug_handle_list(int fd, const char *body, const char *query)
{
    (void)body; (void)query;
    char buf[XWEBD_RESP_BUF_SIZE];
    int pos = 0;
    APPEND_PRINTF(buf, pos, sizeof(buf), "{\"plugins\":[");
    for (int i = 0; i < g_plug_count; i++) {
        xwplug_t *p = &g_plugs[i];
        APPEND_PRINTF(buf, pos, sizeof(buf),
                      "%s{\"name\":\"%s\",\"online\":%s,\"pid\":%d,\"restarts\":%d,"
                      "\"mem_kb\":%d,\"disabled\":%s}",
                      i ? "," : "", p->name,
                      p->online ? "true" : "false", p->pid, p->restarts,
                      p->mem_kb, p->disabled ? "true" : "false");
    }
    APPEND_PRINTF(buf, pos, sizeof(buf), "]}");
    return send_json(fd, 200, buf);
}

/* body: {"name":"files"} */
static int get_name_from_body(const char *body, char *name, int name_size)
{
    if (!body) return -1;
    const char *p = strstr(body, "\"name\"");
    if (!p) return -1;
    p += 6;
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p != '"') return -1;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < name_size - 1) name[i++] = *p++;
    name[i] = '\0';
    return i > 0 ? 0 : -1;
}

int xwplug_handle_restart(int fd, const char *body, const char *query)
{
    char name[32];
    if (get_name_from_body(body, name, sizeof(name)) != 0)
        return send_error(fd, 400, "missing name");
    xwplug_t *p = find_plug(name);
    if (!p) return send_error(fd, 404, "plugin not found");
    stop_plug(p, 1);
    p->disabled = 0;
    p->backoff_sec = 0;
    p->next_spawn = 0;
    sleep(1); /* 等 kill 生效 */
    xwplug_spawn(p);
    char resp[128];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"name\":\"%s\",\"pid\":%d}", name, p->pid);
    return send_json(fd, 200, resp);
}

int xwplug_handle_remove(int fd, const char *body, const char *query)
{
    char name[32];
    if (get_name_from_body(body, name, sizeof(name)) != 0)
        return send_error(fd, 400, "missing name");
    xwplug_t *p = find_plug(name);
    if (!p) return send_error(fd, 404, "plugin not found");
    stop_plug(p, 1);
    if (p->pid > 0) {
        int waited = 0;
        while (waitpid(p->pid, NULL, WNOHANG) <= 0 && waited < 3) { usleep(200000); waited++; }
        kill(p->pid, SIGKILL);
    }
    unlink(p->bin_path);
    XLOG_I(TAG, "已卸载插件 %s", name);
    /* 从表移除 */
    int idx = p - g_plugs;
    memmove(&g_plugs[idx], &g_plugs[idx + 1], (g_plug_count - idx - 1) * sizeof(xwplug_t));
    g_plug_count--;
    return send_json(fd, 200, "{\"ok\":true}");
}

/* body: {"name":"files","path":"/var/upgrade/xwplug-files"} 从中转目录原子安装 */
int xwplug_handle_install(int fd, const char *body, const char *query)
{
    char name[32], src[192];
    if (get_name_from_body(body, name, sizeof(name)) != 0)
        return send_error(fd, 400, "missing name");
    /* path 提取 */
    const char *p = strstr(body, "\"path\"");
    if (!p) return send_error(fd, 400, "missing path");
    p += 6;
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p != '"') return send_error(fd, 400, "bad path");
    p++;
    int i = 0;
    while (*p && *p != '"' && i < (int)sizeof(src) - 1) src[i++] = *p++;
    src[i] = '\0';
    if (access(src, R_OK) != 0) return send_error(fd, 404, "source not found");

    /* 同名插件先卸(幂等升级) */
    xwplug_t *old = find_plug(name);
    if (old) {
        stop_plug(old, 1);
        unlink(old->bin_path);
        old->pid = 0;
        old->online = 0;
        old->disabled = 0;
        old->backoff_sec = 0;
    }

    char dst[192];
    snprintf(dst, sizeof(dst), "%s/xwplug-%.130s", XWPLUG_DIR, name);
    /* 原子安装: 先拷 .tmp 再 rename */
    char tmp[200];
    snprintf(tmp, sizeof(tmp), "%s.tmp", dst);
    FILE *in = fopen(src, "rb");
    if (!in) return send_error(fd, 500, "open source fail");
    FILE *out = fopen(tmp, "wb");
    if (!out) { fclose(in); return send_error(fd, 500, "open dest fail"); }
    char cp[4096];
    size_t n;
    while ((n = fread(cp, 1, sizeof(cp), in)) > 0) fwrite(cp, 1, n, out);
    fclose(in); fclose(out);
    chmod(tmp, 0755);
    if (rename(tmp, dst) != 0) { unlink(tmp); return send_error(fd, 500, "rename fail"); }

    if (old) {
        xwplug_spawn(old); /* 复用槽位 */
    } else if (g_plug_count < XWPLUG_MAX) {
        xwplug_t *np = &g_plugs[g_plug_count++];
        memset(np, 0, sizeof(*np));
        snprintf(np->name, sizeof(np->name), "%s", name);
        snprintf(np->bin_path, sizeof(np->bin_path), "%s", dst);
        xwplug_spawn(np);
    } else {
        return send_error(fd, 507, "plugin table full");
    }

    XLOG_I(TAG, "已安装插件 %s", name);
    char resp[128];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"name\":\"%s\"}", name);
    return send_json(fd, 200, resp);
}

/* ---- 生命周期 ---- */

void xwplug_init(void)
{
    mkdir(XWPLUG_SOCK_DIR, 0755);
    mkdir(XWPLUG_DIR, 0755);
    mkdir(XWPLUG_TMP_DIR, 0755); /* 上传中转目录(安装端点从此取件) */
    /* tmpfs 残留 sock 清扫(插件未起时先清自家目录) */
    DIR *d = opendir(XWPLUG_SOCK_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char sp[96];
            snprintf(sp, sizeof(sp), "%s/%.80s", XWPLUG_SOCK_DIR, e->d_name);
            unlink(sp);
        }
        closedir(d);
    }
    XLOG_I(TAG, "插件网关初始化 (目录 %s)", XWPLUG_DIR);
    scan_plugins();
}

void xwplug_shutdown(void)
{
    for (int i = 0; i < g_plug_count; i++)
        stop_plug(&g_plugs[i], 1);
}
