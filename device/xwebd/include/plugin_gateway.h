/**
 * @file plugin_gateway.h
 * @brief xwebd 插件网关接口（《09-插件化架构设计方案》B0 框架）
 *
 * 插件 = 独立进程 ELF，xwplug-<name>，安装于 XWPLUG_DIR；
 * 与 xwebd 经 unix sock /tmp/xwplug/<name>.sock 通信（极简 HTTP）。
 * xwebd 负责发现/spawn/健康监控/指数退避/路由转发。
 */

#ifndef PLUGIN_GATEWAY_H
#define PLUGIN_GATEWAY_H

#define XWPLUG_DIR      "/var/upgrade/plugins"       /* 安装目录(jffs2 可写持久; /usr/local 在只读 rootfs 不可用) */
#define XWPLUG_SOCK_DIR "/tmp/xwplug"                /* 套接字目录(tmpfs) */
#define XWPLUG_MAX      8
#define XWPLUG_PROBE_SEC    5                        /* 健康探测周期 */
#define XWPLUG_BACKOFF_1    5                        /* 退避: 5s/15s/60s/放弃 */
#define XWPLUG_BACKOFF_2    15
#define XWPLUG_BACKOFF_3    60

/* worker_loop 启动时调用: 建目录/扫描/spawn 全部已装插件 */
void xwplug_init(void);

/* worker_loop 主循环周期调用(约1s粒度): 发现新增/探测/退避重spawn/回收僵尸 */
void xwplug_tick(void);

/* 转发 /api/plugin/<name>/<sub> 请求; plugin_path 即 <name>/<sub> 原样.
 * 返回 1=已写响应(连接保持语义同 route_handler), -1=失败已写错误响应 */
int xwplug_forward(int client_fd, const char *method, const char *plugin_path,
                   const char *query, const char *body, int body_len);

/* 管理端点(route_handler 同形) */
int xwplug_handle_list(int fd, const char *body, const char *query);
int xwplug_handle_restart(int fd, const char *body, const char *query);
int xwplug_handle_remove(int fd, const char *body, const char *query);
int xwplug_handle_install(int fd, const char *body, const char *query);

/* 查询某插件是否在线(旧路径改写判定用) */
int xwplug_is_online(const char *name);

/* 内部插件请求(不走 client socket): xwebd 内部功能代调插件;
 * 返回 HTTP 码, resp 收响应体; 失败 -1(调用方回落内置实现) */
int xwplug_request(const char *name, const char *method, const char *sub,
                   const char *body, int body_len, char *resp, int resp_size);

/* 查询某插件是否已安装(不看运行状态; xwebd 职责交接判定用) */
int xwplug_is_installed(const char *name);

/* 进程退出清理: kill 全部插件 */
void xwplug_shutdown(void);

#endif /* PLUGIN_GATEWAY_H */
