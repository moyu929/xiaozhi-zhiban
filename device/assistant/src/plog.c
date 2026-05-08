/**
 * @file plog.c
 * @brief 小智助手日志模块实现
 *
 * 本文件实现了轻量级的文件日志系统，主要功能：
 * - 日志写入到文件，支持多级别（ERROR/WARNING/INFO/DEBUG）
 * - 日志轮转：文件超过1MB时截断保留最后256KB
 * - 线程安全：通过互斥锁保护文件写入
 * - 格式化输出：时间戳 + 级别 + TAG + 消息内容
 * - 崩溃日志：特殊接口用于非信号处理器的崩溃路径
 * - 日志读取：支持从文件尾部读取最近N行
 */

#include "plog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#include <errno.h>

/* 日志文件描述符 */
static int g_plog_fd = -1;
/* 当前日志级别，低于此级别的日志不输出 */
static int g_plog_level = PLOG_LEVEL_INFO;
/* 日志写入互斥锁 */
static pthread_mutex_t g_plog_mutex = PTHREAD_MUTEX_INITIALIZER;
/* 当前日志文件大小 */
static off_t g_plog_size = 0;
/* 日志文件路径 */
static char g_plog_path[256] = PLOG_PATH_DEFAULT;

/* 日志轮转阈值：文件超过1MB时触发轮转 */
#define PLOG_MAX_SIZE (1024 * 1024)
/* 轮转后保留的日志大小：256KB */
#define PLOG_KEEP_SIZE (256 * 1024)

/**
 * @brief 日志文件轮转
 *        当日志文件超过PLOG_MAX_SIZE时，保留最后PLOG_KEEP_SIZE的数据
 *        从第一个换行符开始保留，确保不截断行
 */
static void plog_rotate(void) {
    if (g_plog_size < PLOG_MAX_SIZE) return;

    close(g_plog_fd);
    g_plog_fd = -1;

    /* 读取日志文件尾部数据 */
    int src_fd = open(g_plog_path, O_RDONLY);
    if (src_fd < 0) {
        g_plog_fd = open(g_plog_path,
                         O_WRONLY | O_CREAT | O_TRUNC | O_SYNC, 0644);
        g_plog_size = 0;
        return;
    }

    off_t file_size = lseek(src_fd, 0, SEEK_END);
    off_t keep_offset = 0;
    if (file_size > PLOG_KEEP_SIZE) {
        keep_offset = file_size - PLOG_KEEP_SIZE;
    }

    char* buf = malloc(PLOG_KEEP_SIZE);
    if (!buf) {
        close(src_fd);
        g_plog_fd = open(g_plog_path,
                         O_WRONLY | O_CREAT | O_TRUNC | O_SYNC, 0644);
        g_plog_size = 0;
        return;
    }

    /* 从keep_offset位置读取数据 */
    lseek(src_fd, keep_offset, SEEK_SET);
    ssize_t total_read = 0;
    while (total_read < PLOG_KEEP_SIZE) {
        ssize_t n = read(src_fd, buf + total_read, PLOG_KEEP_SIZE - total_read);
        if (n <= 0) break;
        total_read += n;
    }
    close(src_fd);

    /* 找到第一个换行符，从完整行开始保留 */
    char* start = buf;
    if (total_read > 0 && keep_offset > 0) {
        char* nl = memchr(buf, '\n', total_read);
        if (nl) {
            start = nl + 1;
            total_read -= (start - buf);
        }
    }

    /* 截断文件并写入保留的数据 */
    g_plog_fd = open(g_plog_path,
                     O_WRONLY | O_CREAT | O_TRUNC | O_SYNC, 0644);
    if (g_plog_fd >= 0 && total_read > 0) {
        write(g_plog_fd, start, total_read);
        g_plog_size = total_read;
    } else {
        g_plog_size = 0;
    }

    free(buf);
}

/* 日志级别名称表 */
static const char* level_names[] = {"E", "W", "I", "D"};

/**
 * @brief 初始化日志系统
 *        打开日志文件（追加模式），获取当前文件大小
 * @param path 日志文件路径，NULL则使用默认路径
 */
void plog_init(const char* path) {
    pthread_mutex_lock(&g_plog_mutex);
    if (g_plog_fd >= 0) {
        close(g_plog_fd);
    }
    if (path) {
        strncpy(g_plog_path, path, sizeof(g_plog_path) - 1);
        g_plog_path[sizeof(g_plog_path) - 1] = '\0';
    } else {
        strncpy(g_plog_path, PLOG_PATH_DEFAULT, sizeof(g_plog_path) - 1);
    }
    g_plog_fd = open(g_plog_path,
                     O_WRONLY | O_CREAT | O_APPEND | O_SYNC, 0644);
    if (g_plog_fd >= 0) {
        struct stat st;
        if (fstat(g_plog_fd, &st) == 0) {
            g_plog_size = st.st_size;
        } else {
            g_plog_size = 0;
        }
    }
    pthread_mutex_unlock(&g_plog_mutex);
}

/**
 * @brief 关闭日志系统，同步并关闭文件描述符
 */
void plog_close(void) {
    pthread_mutex_lock(&g_plog_mutex);
    if (g_plog_fd >= 0) {
        fsync(g_plog_fd);
        close(g_plog_fd);
        g_plog_fd = -1;
    }
    pthread_mutex_unlock(&g_plog_mutex);
}

/**
 * @brief 设置日志输出级别
 * @param level 日志级别（PLOG_LEVEL_ERROR/PLOG_LEVEL_WARN/PLOG_LEVEL_INFO/PLOG_LEVEL_DEBUG）
 */
void plog_set_level(int level) {
    g_plog_level = level;
}

/**
 * @brief 获取当前日志级别
 * @return 当前日志级别
 */
int plog_get_level(void) {
    return g_plog_level;
}

/**
 * @brief 日志写入核心函数（va_list版本）
 *        格式：[时:分:秒.毫秒][级别][TAG] 消息内容\n
 *        写入前检查轮转，线程安全
 * @param level 日志级别
 * @param tag 日志标签
 * @param fmt 格式化字符串
 * @param ap 可变参数列表
 */
void plog_vwrite(int level, const char* tag, const char* fmt, va_list ap) {
    if (level > g_plog_level) return;
    if (g_plog_fd < 0) return;

    char buf[512];
    int len = 0;

    /* 生成时间戳和日志头 */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_buf;
    localtime_r(&ts.tv_sec, &tm_buf);

    len = snprintf(buf, sizeof(buf), "[%02d:%02d:%02d.%03d][%s][%s] ",
                   tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                   (int)(ts.tv_nsec / 1000000),
                   (level >= 0 && level <= 3) ? level_names[level] : "?",
                   tag ? tag : "");

    /* 格式化日志消息 */
    if (len > 0 && (size_t)len < sizeof(buf)) {
        int remain = sizeof(buf) - len - 2;
        int fmt_len = vsnprintf(buf + len, remain, fmt, ap);
        if (fmt_len > 0) {
            len += (fmt_len < remain) ? fmt_len : remain;
        }
    }

    /* 添加换行符 */
    if (len > 0 && (size_t)len < sizeof(buf)) {
        buf[len++] = '\n';
    }

    /* 写入日志文件（带轮转检查） */
    pthread_mutex_lock(&g_plog_mutex);
    if (g_plog_fd >= 0) {
        plog_rotate();
        int w = write(g_plog_fd, buf, len);
        if (w > 0) g_plog_size += w;
    }
    pthread_mutex_unlock(&g_plog_mutex);
}

/**
 * @brief 日志写入接口（可变参数版本）
 * @param level 日志级别
 * @param tag 日志标签
 * @param fmt 格式化字符串
 */
void plog_write(int level, const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    plog_vwrite(level, tag, fmt, ap);
    va_end(ap);
}

/**
 * @brief 同步日志文件到磁盘
 */
void plog_sync(void) {
    pthread_mutex_lock(&g_plog_mutex);
    if (g_plog_fd >= 0) {
        fsync(g_plog_fd);
    }
    pthread_mutex_unlock(&g_plog_mutex);
}

/**
 * @brief 崩溃日志写入（非异步信号安全）
 *        警告：此函数使用了vsnprintf和互斥锁，不是异步信号安全的
 *        不要从信号处理器中调用，应使用snprintf+write直接输出
 *        仅保留用于非信号处理的崩溃日志路径
 * @param fmt 格式化字符串
 */
void plog_crash(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    char buf[512];
    int len = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    if (len > 0) {
        buf[len++] = '\n';
        write(STDERR_FILENO, buf, len);

        int fd = g_plog_fd;
        if (fd >= 0) {
            write(fd, buf, len);
            fsync(fd);
        }
    }

    va_end(ap);
}

/**
 * @brief 刷新日志文件缓冲区到磁盘
 */
void plog_flush(void) {
    pthread_mutex_lock(&g_plog_mutex);
    if (g_plog_fd >= 0) {
        fsync(g_plog_fd);
    }
    pthread_mutex_unlock(&g_plog_mutex);
}

/**
 * @brief 获取当前日志文件大小
 * @return 日志文件大小（字节）
 */
off_t plog_get_size(void) {
    pthread_mutex_lock(&g_plog_mutex);
    off_t size = g_plog_size;
    pthread_mutex_unlock(&g_plog_mutex);
    return size;
}

/**
 * @brief 获取当前日志文件路径
 * @return 日志文件路径字符串
 */
const char* plog_get_path(void) {
    return g_plog_path;
}

/**
 * @brief 从日志文件尾部读取最近N行
 *        通过反向扫描换行符定位起始位置，然后顺序读取
 * @param buf 输出缓冲区
 * @param buf_size 缓冲区大小
 * @param max_lines 最大读取行数
 * @return 实际读取的字节数
 */
int plog_read_last_lines(char* buf, int buf_size, int max_lines) {
    if (!buf || buf_size <= 0 || max_lines <= 0) return 0;

    int fd = open(g_plog_path, O_RDONLY);
    if (fd < 0) return 0;

    off_t file_size = lseek(fd, 0, SEEK_END);
    if (file_size <= 0) { close(fd); return 0; }

    /* 反向扫描换行符，统计行数 */
    int line_count = 0;
    off_t pos = file_size;
    char read_buf[1024];

    while (pos > 0 && line_count < max_lines) {
        int chunk = (pos < (off_t)sizeof(read_buf)) ? (int)pos : (int)sizeof(read_buf);
        pos -= chunk;
        lseek(fd, pos, SEEK_SET);
        ssize_t n = read(fd, read_buf, chunk);
        if (n <= 0) break;

        for (ssize_t i = n - 1; i >= 0; i--) {
            if (read_buf[i] == '\n') {
                line_count++;
                if (line_count >= max_lines) {
                    pos += i + 1;
                    break;
                }
            }
        }
    }

    /* 从定位的起始位置顺序读取 */
    if (pos < 0) pos = 0;
    lseek(fd, pos, SEEK_SET);

    int total = 0;
    while (total < buf_size - 1) {
        ssize_t n = read(fd, buf + total, buf_size - 1 - total);
        if (n <= 0) break;
        total += n;
    }

    close(fd);
    buf[total] = '\0';
    return total;
}
