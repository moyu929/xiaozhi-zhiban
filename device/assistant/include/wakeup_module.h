#ifndef WAKEUP_MODULE_H
#define WAKEUP_MODULE_H

#include <stdint.h>
#include "audio_dispatcher.h"
#include "reverse/sair_asr_api.h"

typedef struct {
    void* asr_lib;
    void* asr_handle;
    int initialized;
    int started;
    int paused;
    int asr_init_done;

    asr_set_params_t  f_asr_set_params;
    asr_init_t        f_asr_init;
    asr_finalize_t    f_asr_finalize;
    asr_start_t       f_asr_start;
    asr_stop_t        f_asr_stop;
    asr_feed_data_t   f_asr_feed_data;

    void (*on_wakeup)(int wakeup_type, void* user_data);
    void* wakeup_user_data;

    audio_dispatcher_t* dispatcher;
} wakeup_module_t;

int wakeup_init(wakeup_module_t* mod, audio_dispatcher_t* disp,
                void (*on_wakeup)(int wakeup_type, void* user_data),
                void* user_data);
int wakeup_start(wakeup_module_t* mod);
void wakeup_stop(wakeup_module_t* mod);
void wakeup_close(wakeup_module_t* mod);
void wakeup_pause_feed(wakeup_module_t* mod);
void wakeup_resume_feed(wakeup_module_t* mod);
int wakeup_is_feed_active(wakeup_module_t* mod);
void wakeup_destroy(wakeup_module_t* mod);

#endif
