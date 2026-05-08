#ifndef STATE_MACHINE_H
#define STATE_MACHINE_H

#include <stdint.h>
#include <pthread.h>

typedef enum {
    kStateStarting,
    kStateActivating,
    kStateIdle,
    kStateConnecting,
    kStateListening,
    kStateSpeaking,
    kStateCleaning,
} xiaozhi_state_t;

#define kStateAny ((xiaozhi_state_t)-1)

typedef void (*state_observer_t)(xiaozhi_state_t from, xiaozhi_state_t to, void* user_data);

typedef struct {
    xiaozhi_state_t target_state;
    int priority;
    state_observer_t callback;
    void* user_data;
} state_observer_entry_t;

#define MAX_STATE_OBSERVERS 16

typedef struct {
    xiaozhi_state_t current_state;
    int transitioning;
    state_observer_entry_t observers[MAX_STATE_OBSERVERS];
    int observer_count;
    pthread_mutex_t mutex;
} state_machine_t;

int state_machine_init(state_machine_t* sm);
void state_machine_destroy(state_machine_t* sm);
xiaozhi_state_t state_machine_get_state(state_machine_t* sm);
int state_machine_transition(state_machine_t* sm, xiaozhi_state_t new_state);
int state_machine_add_observer(state_machine_t* sm, xiaozhi_state_t target, int priority,
                               state_observer_t callback, void* user_data);
void state_machine_remove_observer(state_machine_t* sm, state_observer_t callback);
const char* state_machine_get_state_name(xiaozhi_state_t state);

#endif
