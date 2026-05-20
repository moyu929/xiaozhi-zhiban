#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <errno.h>

#define MAX_APP_NAME_LEN 128
#define MSG_SIZE 0x450

#define MSG_TYPE_SYSTEM    0x01
#define MSG_TYPE_BROADCAST 0x03

#define MSG_APP_QUIT 1

typedef struct {
    int msg_type;
    int msg_id;
    int msg_size;
    int reserved;
    char sender[MAX_APP_NAME_LEN];
    char target[MAX_APP_NAME_LEN];
    char data[MSG_SIZE - 0x110];
} applib_msg_t;

int main(int argc, char *argv[]) {
    const char *target = "launcher";
    int msg_id = MSG_APP_QUIT;
    int msg_type = MSG_TYPE_SYSTEM;

    if (argc > 1) {
        if (strcmp(argv[1], "quit") == 0) {
            msg_id = MSG_APP_QUIT;
            msg_type = MSG_TYPE_SYSTEM;
        } else if (strcmp(argv[1], "exit_usb") == 0) {
            msg_id = 0x236;
            msg_type = MSG_TYPE_BROADCAST;
        } else {
            fprintf(stderr, "Usage: %s [quit|exit_usb]\n", argv[0]);
            return 1;
        }
    }

    void *librt = dlopen("librt.so.0", RTLD_LAZY);
    if (!librt) {
        fprintf(stderr, "dlopen librt.so.0 failed: %s\n", dlerror());
        return 1;
    }

    int (*fn_mq_open)(const char *, int, ...) = dlsym(librt, "mq_open");
    int (*fn_mq_send)(int, const char *, unsigned, unsigned) = dlsym(librt, "mq_send");
    int (*fn_mq_close)(int) = dlsym(librt, "mq_close");

    if (!fn_mq_open || !fn_mq_send || !fn_mq_close) {
        fprintf(stderr, "dlsym failed: %s\n", dlerror());
        dlclose(librt);
        return 1;
    }

    applib_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_type = msg_type;
    msg.msg_id = msg_id;
    msg.msg_size = sizeof(msg);
    msg.reserved = 0;
    strncpy(msg.sender, "usb_helper", MAX_APP_NAME_LEN - 1);
    strncpy(msg.target, target, MAX_APP_NAME_LEN - 1);

    char mq_name[256];
    snprintf(mq_name, sizeof(mq_name), "/%s_mq", target);

    int mq = fn_mq_open(mq_name, O_WRONLY | O_NONBLOCK);
    if (mq < 0) {
        fprintf(stderr, "mq_open %s failed: %s\n", mq_name, strerror(errno));
        dlclose(librt);
        return 1;
    }

    if (fn_mq_send(mq, (const char *)&msg, sizeof(msg), 0) != 0) {
        fprintf(stderr, "mq_send failed: %s\n", strerror(errno));
        fn_mq_close(mq);
        dlclose(librt);
        return 1;
    }

    printf("Sent msg_type=%d msg_id=%d to %s\n", msg_type, msg_id, mq_name);
    fn_mq_close(mq);
    dlclose(librt);
    return 0;
}
