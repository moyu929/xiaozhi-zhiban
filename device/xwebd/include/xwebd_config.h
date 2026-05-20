/**
 * xwebd_config.h - xwebd 配置头文件
 *
 * 定义xwebd HTTP服务器的所有配置常量，包括：
 * - 服务器端口、缓冲区大小等基础配置
 * - 日志路径和大小限制
 * - 文件上传限制
 * - 音量、亮度等硬件控制参数
 * - 助手程序(sair)相关路径
 * - 文件保护和清理规则
 */
#ifndef XWEBD_CONFIG_H
#define XWEBD_CONFIG_H

#define XWEBD_VERSION "1.0.0" /* 版本号 */

#define XWEBD_DEFAULT_PORT    8080    /* 默认监听端口 */
#define XWEBD_REQ_BUF_SIZE    8192    /* HTTP请求缓冲区大小(字节) */
#define XWEBD_RESP_BUF_SIZE   32768   /* HTTP响应缓冲区大小(字节) */
#define XWEBD_FILE_BUF_SIZE   32768   /* 文件传输缓冲区大小(字节) */
#define XWEBD_REQUEST_TIMEOUT 10      /* 请求超时时间(秒) */

#define XWEBD_LOG_PATH        "/var/upgrade/xwebd.log" /* 日志文件路径 */
#define XWEBD_LOG_MAX_SIZE    (128 * 1024)             /* 日志文件最大大小(128KB)，超过后触发轮转 */
#define XWEBD_LOG_KEEP_SIZE   (64 * 1024)              /* 日志轮转时保留的尾部大小(64KB) */

#define XWEBD_UPLOAD_MAX_MB_DEFAULT  10                         /* 默认上传文件大小限制(MB) */
#define XWEBD_UPLOAD_PID_FILE        "/var/upgrade/.upload_pid" /* 上传进程PID记录文件路径 */

#define XWEBD_BASE_DIR       "/var/upgrade" /* 文件操作根目录，所有文件读写均限制在此目录下 */
#define XWEBD_BASE_DIR_LEN   (sizeof(XWEBD_BASE_DIR) - 1) /* 根目录字符串长度，用于路径安全校验 */

#define XWEBD_WATCHDOG_CRASH_LIMIT  5  /* 看门狗: 允许的最大崩溃次数 */
#define XWEBD_WATCHDOG_CRASH_WINDOW 60 /* 看门狗: 崩溃统计时间窗口(秒) */

#define XWEBD_HEALTH_CHECK_INTERVAL 30  /* 健康检查间隔(秒) */
#define XWEBD_HEALTH_FAIL_LIMIT     3   /* 连续健康检查失败次数阈值 */
#define XWEBD_MAX_RSS_KB           8192  /* 工作进程最大允许RSS(KB) */

#define XWEBD_TEST_SH        "/var/upgrade/test.sh"     /* 自启动脚本路径 */
#define XWEBD_TEST_SH_NEW    "/var/upgrade/test.sh.new" /* 自启动脚本更新时的临时文件 */

#define XWEBD_WATCHDOG_SH    "/var/upgrade/boot_watchdog.sh" /* 开机看门狗脚本路径 */
#define XWEBD_PERSIST_CONF   "/var/upgrade/xwebd_persist.conf" /* 持久化配置文件路径 */
#define XWEBD_SAIR_LOG       "/var/upgrade/xiaozhi.log"  /* 助手程序(sair)日志文件路径 */
#define XWEBD_SAIR_BIN       "/var/upgrade/sair"         /* 助手程序(sair)二进制文件路径(sair为Assistant的二进制文件名，平台约束不可改名) */
#define XWEBD_SAIR_BACKUP    "/var/upgrade/sair_backup"  /* 助手程序(sair)备份文件路径 */

/* 受保护文件列表: 这些文件禁止通过API删除，以\0分隔 */
#define XWEBD_PROTECT_FILES  "xwebd\0sair\0sair_backup\0boot_watchdog.sh\0test.sh\0test.sh.new\0xiaozhi.log\0xwebd_persist.conf\0"
/* 清理时截断(清空内容)而非删除的文件列表，以\0分隔 */
#define XWEBD_TRUNCATE_FILES "xwebd.log\0"
/* 清理时直接删除的文件名模式列表，以\0分隔 */
#define XWEBD_CLEANUP_PATTERNS "sair_new\0.upload_pid\0.api_token\0"

#endif
