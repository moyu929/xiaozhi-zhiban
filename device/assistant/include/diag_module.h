#ifndef DIAG_MODULE_H
#define DIAG_MODULE_H

#define DIAG_MAX_ITEMS 24

typedef struct {
    const char *name;
    int ok;
    const char *message;
} diag_item_t;

typedef struct {
    int count;
    diag_item_t items[DIAG_MAX_ITEMS];
    char summary[256];
} diag_result_t;

diag_result_t diag_run_all(void);

int diag_result_to_json(const diag_result_t *result, char *buf, int buf_size);

#endif
