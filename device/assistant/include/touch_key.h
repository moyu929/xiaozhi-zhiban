#ifndef TOUCH_KEY_H
#define TOUCH_KEY_H

#include <stdint.h>
#include <pthread.h>

typedef struct {
    int fd;
    pthread_t thread;
    int running;
    volatile int pending_key;
    void (*on_key)(int key_code, void* user_data);
    void* user_data;
} touch_key_t;

int touch_key_init(touch_key_t* tk,
                   void (*on_key)(int key_code, void* user_data),
                   void* user_data);
void touch_key_destroy(touch_key_t* tk);

#endif
