#ifndef PLOG_H
#define PLOG_H

#include <stdarg.h>
#include <sys/types.h>

#define PLOG_LEVEL_ERROR 0
#define PLOG_LEVEL_WARN  1
#define PLOG_LEVEL_INFO  2
#define PLOG_LEVEL_DEBUG 3

#define PLOG_PATH_DEFAULT "/var/upgrade/xiaozhi.log"

void plog_init(const char* path);
void plog_close(void);
void plog_set_level(int level);
int plog_get_level(void);
void plog_write(int level, const char* tag, const char* fmt, ...);
void plog_vwrite(int level, const char* tag, const char* fmt, va_list ap);
void plog_sync(void);
void plog_crash(const char* fmt, ...);
void plog_flush(void);
off_t plog_get_size(void);
const char* plog_get_path(void);
int plog_read_last_lines(char* buf, int buf_size, int max_lines);

#define PLOG_E(tag, fmt, ...) plog_write(PLOG_LEVEL_ERROR, tag, fmt, ##__VA_ARGS__)
#define PLOG_W(tag, fmt, ...) plog_write(PLOG_LEVEL_WARN,  tag, fmt, ##__VA_ARGS__)
#define PLOG_I(tag, fmt, ...) plog_write(PLOG_LEVEL_INFO,  tag, fmt, ##__VA_ARGS__)
#define PLOG_D(tag, fmt, ...) plog_write(PLOG_LEVEL_DEBUG, tag, fmt, ##__VA_ARGS__)

#endif
