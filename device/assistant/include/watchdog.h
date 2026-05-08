#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <pthread.h>
#include <stdint.h>

#define WD_TIMEOUT_IDLE  30000
#define WD_TIMEOUT_ACTIVE 50000

typedef struct {
    pthread_t thread;
    int running;
    void* sair_app_info;
    int shm_fd;
    void* shm_ptr;
    char mq_name[64];
    int feed_fail_count;
    uint32_t timeout_ms;
} watchdog_t;

int watchdog_init(watchdog_t* wd);
void watchdog_destroy(watchdog_t* wd);
void watchdog_feed(watchdog_t* wd);
void watchdog_set_timeout(watchdog_t* wd, uint32_t timeout_ms);

#endif
