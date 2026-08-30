/**
 * @file xwplug_files.c
 * @brief 文件管理插件（《09 方案》B1, 首个功能迁移样板）
 *
 * 端点(经 /api/plugin/files/<sub> 或旧路径 /api/files* 改写访问):
 *   GET  /list?path=/var/upgrade  -> 目录清单 JSON
 *   GET  /delete?path=...         -> 删除单个文件
 *   POST /batch-delete {"paths":[...]} -> 批量删除
 *   GET  /cleanup                 -> 清理垃圾文件
 *
 * download/upload(大文件流式)留 xwebd 核心内置, 不迁插件。
 * 安全: validate_path(路径逃逸防护)+is_protected_file(保护清单)与 xwebd 内置同源。
 */

#include "xwplug_common.h"
#include <sys/stat.h>
#include <dirent.h>
#include <limits.h>
#include <fcntl.h>

#define FILES_BASE_DIR    "/var/upgrade"
#define FILES_BASE_DIR_LEN (sizeof(FILES_BASE_DIR) - 1)
#define FILES_RESP_MAX    32768

/* 受保护文件(与 xwebd XWEBD_PROTECT_FILES 同源) */
static const char *k_protect[] = {
    "xwebd", "sair", "sair_backup", "boot_watchdog.sh",
    "test.sh", "test.sh.new", "xiaozhi.log", "xwebd_persist.conf", NULL,
};
/* 清理时截断而非删除 */
static const char *k_truncate[] = { "xwebd.log", NULL };
/* 清理时直接删除 */
static const char *k_cleanup[] = { "sair_new", ".upload_pid", ".api_token", NULL };

static int is_in_list(const char **list, const char *name)
{
    for (int i = 0; list[i]; i++)
        if (strcmp(name, list[i]) == 0) return 1;
    return 0;
}

static int isxdigit_(int c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static void url_decode(const char *src, char *dst, int dst_size)
{
    int o = 0;
    for (int i = 0; src[i] && o < dst_size - 1; i++) {
        if (src[i] == '%' && isxdigit_((unsigned char)src[i + 1]) && isxdigit_((unsigned char)src[i + 2])) {
            char hex[3] = {src[i + 1], src[i + 2], 0};
            dst[o++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else if (src[i] == '+') {
            dst[o++] = ' ';
        } else {
            dst[o++] = src[i];
        }
    }
    dst[o] = '\0';
}

/* 路径校验: 解码+..拒绝+realpath 收敛到 BASE_DIR 内 */
static int validate_path(const char *raw_path, char *resolved, int resolved_size)
{
    char decoded[PATH_MAX];
    url_decode(raw_path, decoded, sizeof(decoded));
    if (strstr(decoded, "..")) return -1;

    char real_resolved[PATH_MAX];
    if (realpath(decoded, real_resolved) == NULL) {
        if (errno != ENOENT) return -1;
        snprintf(resolved, resolved_size, "%s", decoded);
    } else {
        snprintf(resolved, resolved_size, "%s", real_resolved);
    }
    if (strncmp(resolved, FILES_BASE_DIR, FILES_BASE_DIR_LEN) != 0) return -1;
    char next_char = resolved[FILES_BASE_DIR_LEN];
    if (next_char != '/' && next_char != '\0') return -1;
    return 0;
}

static void json_escape(const char *src, char *dst, int dst_size)
{
    int j = 0;
    for (int i = 0; src[i] && j < dst_size - 2; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') { dst[j++] = '\\'; dst[j++] = c; }
        else if (c == '\n') { dst[j++] = '\\'; dst[j++] = 'n'; }
        else if (c == '\r') { dst[j++] = '\\'; dst[j++] = 'r'; }
        else if (c == '\t') { dst[j++] = '\\'; dst[j++] = 't'; }
        else if (c < 0x20) { j += snprintf(dst + j, dst_size - j, "\\u%04x", c); }
        else dst[j++] = c;
    }
    dst[j] = '\0';
}

/* 从 query 中取 "key=value"(到 & 或串尾) */
static int query_get(const char *query, const char *key, char *out, int out_size)
{
    if (!query || !*query) return -1;
    char pat[16];
    snprintf(pat, sizeof(pat), "%s=", key);
    const char *p = strstr(query, pat);
    if (!p) return -1;
    p += strlen(pat);
    const char *end = strchr(p, '&');
    int n = end ? (int)(end - p) : (int)strlen(p);
    if (n >= out_size) n = out_size - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return 0;
}

/* ---- 各端点 ---- */

static int ep_list(const xwplug_req_t *req, char *resp, int resp_size)
{
    char path_val[PATH_MAX] = "";
    query_get(req->query, "path", path_val, sizeof(path_val));

    char resolved[PATH_MAX];
    if (validate_path(path_val, resolved, sizeof(resolved)) != 0) {
        snprintf(resp, resp_size, "{\"error\":\"Invalid or unsafe path\"}");
        return 400;
    }

    DIR *dir = opendir(resolved);
    if (!dir) {
        snprintf(resp, resp_size, "{\"error\":\"Directory not found\"}");
        return 404;
    }

    char esc_path[PATH_MAX];
    json_escape(resolved, esc_path, sizeof(esc_path));

    int len = 0;
    APPEND_PRINTF(resp, len, resp_size, "{\"path\":\"%s\",\"files\":[", esc_path);

    struct dirent *ent;
    int first = 1;
    while ((ent = readdir(dir)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;

        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", resolved, ent->d_name);
        struct stat st;
        if (stat(full_path, &st) != 0) continue;

        if (!first) APPEND_PRINTF(resp, len, resp_size, ",");
        first = 0;

        char esc_name[512];
        json_escape(ent->d_name, esc_name, sizeof(esc_name));
        APPEND_PRINTF(resp, len, resp_size,
            "{\"name\":\"%s\",\"size\":%ld,\"is_dir\":%s,\"mtime\":%ld}",
            esc_name, (long)st.st_size, S_ISDIR(st.st_mode) ? "true" : "false", (long)st.st_mtime);

        if (len >= resp_size - 256) break; /* 防溢出截断(与内置同策略) */
    }
    closedir(dir);
    APPEND_PRINTF(resp, len, resp_size, "]}");
    return 200;
}

static int ep_delete(const xwplug_req_t *req, char *resp, int resp_size)
{
    char path_val[PATH_MAX] = "";
    query_get(req->query, "path", path_val, sizeof(path_val));

    char resolved[PATH_MAX];
    if (validate_path(path_val, resolved, sizeof(resolved)) != 0) {
        snprintf(resp, resp_size, "{\"error\":\"Invalid or unsafe path\"}");
        return 400;
    }
    const char *fname = strrchr(resolved, '/');
    fname = fname ? fname + 1 : resolved;
    if (is_in_list(k_protect, fname)) {
        snprintf(resp, resp_size, "{\"error\":\"Cannot delete protected file\"}");
        return 400;
    }
    if (remove(resolved) != 0) {
        if (errno == ENOENT) { snprintf(resp, resp_size, "{\"error\":\"File not found\"}"); return 404; }
        snprintf(resp, resp_size, "{\"error\":\"Delete failed\"}");
        return 500;
    }
    snprintf(resp, resp_size, "{\"ok\":true}");
    return 200;
}

static int ep_batch_delete(const xwplug_req_t *req, char *resp, int resp_size)
{
    int deleted = 0;
    const char *p = strstr(req->body, "\"paths\"");
    if (!p) { snprintf(resp, resp_size, "{\"error\":\"Missing paths field\"}"); return 400; }
    p = strchr(p, '[');
    if (!p) { snprintf(resp, resp_size, "{\"error\":\"Invalid paths format\"}"); return 400; }
    p++;

    while (*p && *p != ']') {
        while (*p && (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
        if (*p != '"') { p++; continue; }
        p++;
        char path_val[PATH_MAX];
        int i = 0;
        while (*p && *p != '"' && i < PATH_MAX - 1) {
            if (*p == '\\' && *(p + 1)) { p++; path_val[i++] = *p++; }
            else path_val[i++] = *p++;
        }
        path_val[i] = '\0';
        if (*p == '"') p++;

        char resolved[PATH_MAX];
        if (validate_path(path_val, resolved, sizeof(resolved)) == 0) {
            const char *fname = strrchr(resolved, '/');
            fname = fname ? fname + 1 : resolved;
            if (!is_in_list(k_protect, fname) && remove(resolved) == 0) deleted++;
        }
    }
    snprintf(resp, resp_size, "{\"deleted\":%d}", deleted);
    return 200;
}

static int ep_cleanup(const xwplug_req_t *req, char *resp, int resp_size)
{
    (void)req;
    long cleaned_bytes = 0;
    int cleaned_files = 0;

    DIR *dir = opendir(FILES_BASE_DIR);
    if (!dir) { snprintf(resp, resp_size, "{\"error\":\"Cannot open base directory\"}"); return 500; }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;

        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", FILES_BASE_DIR, ent->d_name);
        struct stat st;
        if (stat(full_path, &st) != 0 || S_ISDIR(st.st_mode)) continue;
        if (is_in_list(k_protect, ent->d_name)) continue;

        if (is_in_list(k_truncate, ent->d_name)) {
            long old_size = st.st_size;
            int tfd = open(full_path, O_WRONLY | O_TRUNC);
            if (tfd >= 0) { close(tfd); cleaned_bytes += old_size; cleaned_files++; }
            continue;
        }

        if (!strcmp(ent->d_name, ".upload_pid")) {
            /* 上传 pid 文件: 进程不在或非 xwebd 才删 */
            char pid_buf[16] = {0};
            int fd = open(full_path, O_RDONLY);
            if (fd >= 0) {
                int n = read(fd, pid_buf, sizeof(pid_buf) - 1);
                close(fd);
                while (n > 0 && (pid_buf[n-1]=='\n'||pid_buf[n-1]=='\r'||pid_buf[n-1]==' ')) n--;
                pid_buf[n] = '\0';
                int pid = atoi(pid_buf);
                char comm_path[64], comm[64] = {0};
                snprintf(comm_path, sizeof(comm_path), "/proc/%d/comm", pid);
                int cfd = open(comm_path, O_RDONLY);
                if (cfd >= 0) { read(cfd, comm, sizeof(comm)-1); close(cfd); }
                if (pid <= 0 || !strstr(comm, "xwebd")) {
                    if (remove(full_path) == 0) { cleaned_bytes += st.st_size; cleaned_files++; }
                }
            }
            continue;
        }

        if (is_in_list(k_cleanup, ent->d_name)) {
            if (remove(full_path) == 0) { cleaned_bytes += st.st_size; cleaned_files++; }
            continue;
        }

        size_t namelen = strlen(ent->d_name);
        if (namelen >= 4 && !strcmp(ent->d_name + namelen - 4, ".tmp")) {
            if (remove(full_path) == 0) { cleaned_bytes += st.st_size; cleaned_files++; }
        }
    }
    closedir(dir);

    snprintf(resp, resp_size, "{\"ok\":true,\"cleaned_files\":%d,\"cleaned_bytes\":%ld}",
             cleaned_files, cleaned_bytes);
    return 200;
}

int plug_handle(const xwplug_req_t *req, char *resp, int resp_size)
{
    if (strcmp(req->path, "/list") == 0 && strcmp(req->method, "GET") == 0)
        return ep_list(req, resp, resp_size);
    if (strcmp(req->path, "/delete") == 0 && strcmp(req->method, "GET") == 0)
        return ep_delete(req, resp, resp_size);
    if (strcmp(req->path, "/batch-delete") == 0 && strcmp(req->method, "POST") == 0)
        return ep_batch_delete(req, resp, resp_size);
    if (strcmp(req->path, "/cleanup") == 0 && strcmp(req->method, "GET") == 0)
        return ep_cleanup(req, resp, resp_size);

    snprintf(resp, resp_size, "{\"error\":\"not found\",\"path\":\"%s\"}", req->path);
    return 404;
}

/* files 插件响应较大(list 可达 32KB), 覆盖骨架默认缓冲 */
#undef XWPLUG_RESP_MAX
#define XWPLUG_RESP_MAX 32768
XWPLUG_MAIN("files")
