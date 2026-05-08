#ifndef APPLIB_API_H
#define APPLIB_API_H

#include <stdint.h>
#include <pthread.h>
#include <mqueue.h>
#include <semaphore.h>
#include <sys/socket.h>
#include <sys/un.h>

/*============================================================================
 * 常量定义 (从反汇编精确提取)
 *============================================================================*/

/* 共享内存大小 */
#define APPLIB_SHM_SIZE         0x458   /* 1112 bytes - applib_sync_shm_t 大小 */
#define APPLIB_DIR_CFG_SIZE     0x234c  /* 9036 bytes - 目录配置大小 */
#define APP_RUNNING_LIST_SIZE   0x1420  /* 5136 bytes - 含16字节头部 */

/* 结构体大小 */
#define APP_INFO_SIZE           0x188   /* 392 bytes - app_info_t 大小 */
#define MSG_SIZE                0x450   /* 1104 bytes - 消息大小 */

/* 名称长度 */
#define MAX_APP_NAME_LEN        128
#define MAX_SERVICE_NAME_LEN    128
#define MAX_MQ_NAME_LEN         128
#define MAX_SHM_NAME_LEN        128

/* 服务相关常量 */
#define MAX_RETRY_COUNT         30      /* 连接重试次数 */
#define RECV_TIMEOUT_MS         1000    /* 接收超时毫秒 */
#define MAX_SERVICE_COUNT       32      /* 最大服务数量 */
#define MAX_MSG_FILTERS         32      /* 最大消息过滤器数量 */

/* 消息类型 */
#define MSG_TYPE_SYSTEM         0x01    /* 系统消息 */
#define MSG_TYPE_SERVICE        0x02    /* 服务消息 */
#define MSG_TYPE_BROADCAST      0x03    /* 广播消息 */

/* 错误码 */
#define APPLIB_OK               0
#define APPLIB_ERR_INIT         -1
#define APPLIB_ERR_NOT_INIT     -2
#define APPLIB_ERR_INVALID_ARG  -3
#define APPLIB_ERR_NO_MEMORY    -4
#define APPLIB_ERR_TIMEOUT      -5
#define APPLIB_ERR_NOT_FOUND    -6
#define APPLIB_ERR_SOCKET       -7
#define APPLIB_ERR_MQ           -8
#define APPLIB_ERR_SHM          -9

/*============================================================================
 * 数据结构定义 (精确字段偏移)
 *============================================================================*/

/**
 * @brief 应用信息结构体 (392 bytes)
 * 
 * 已确认的字段 (从反汇编和实测验证):
 * - offset 0x00: field_0 (int)
 * - offset 0x04: _pad0 (int) - 填充
 * - offset 0x08: pid (int) - 实测确认
 * - offset 0x0C: name[128] (char数组) - 实测确认
 * - offset 0x8C: mq_name[128] (char数组)
 * - offset 0x10C: sync_shm_name[128] (char数组)
 * - offset 0x70: watchdog_expire (uint64_t, 相对于条目起始)
 * - offset 0x78: soft_watchdog_forbid (1字节, 相对于条目起始)
 * - offset 0x188之后: socket_fd等字段 (偏移待验证)
 */
typedef struct {
    int field_0;                           /* +0x00 */
    int _pad0;                             /* +0x04: 填充 */
    int pid;                               /* +0x08 (实测确认) */
    
    char name[MAX_APP_NAME_LEN];           /* +0x0C (实测确认, 128 bytes) */
    char mq_name[MAX_MQ_NAME_LEN];         /* +0x8C (128 bytes) */
    char sync_shm_name[MAX_SHM_NAME_LEN];  /* +0x10C (128 bytes) */
    
    /* ⚠️ 以下字段偏移未验证，仅按逻辑排列 */
    int socket_fd;                         /* 偏移待验证 */
    int service_ref_count;                 /* 偏移待验证 */
    pthread_mutex_t service_mutex;         /* 偏移待验证 */
    int service_list_head;                 /* 偏移待验证 */
    int mq_flags;                          /* 偏移待验证 */
    int mq_ref_count;                      /* 偏移待验证 */
    int app_type;                          /* 偏移待验证 */
    int msg_filters[MAX_MSG_FILTERS];      /* 偏移待验证 */
    int filter_msg_id;                     /* 偏移待验证 */
    int flags;                             /* 偏移待验证 */
} app_info_t;

/**
 * @brief 服务信息结构体
 */
typedef struct service_info {
    char name[MAX_SERVICE_NAME_LEN];       /* 服务名称 */
    int socket_fd;                         /* socket 文件描述符 */
    int ref_count;                         /* 引用计数 */
    pthread_mutex_t mutex;                 /* 服务互斥锁 */
    struct service_info *next;             /* 链表下一个节点 */
    int field_40;                          /* 额外字段 */
    char _pad[52];                         /* 填充到 128 bytes */
} service_info_t;

/**
 * @brief applib 同步共享内存结构 (1112 bytes)
 * 
 * 布局从反汇编提取:
 * - offset 0x00: magic/signature
 * - offset 0x04: mutex (pthread_mutex_t, 约 24 bytes)
 * - offset 0x20: cond (pthread_cond_t, 约 48 bytes)
 */
typedef struct {
    /* 同步原语 */
    int magic;                             /* +0x00: 魔数标识 */
    pthread_mutex_t mutex;                 /* +0x04: 进程间共享互斥锁 */
    char _pad1[4];                         /* 填充到 0x20 */
    pthread_cond_t cond;                   /* +0x20: 进程间共享条件变量 */
    
    /* 状态字段 */
    int state;                             /* +0x50: 状态 */
    int error_code;                        /* +0x54: 错误码 */
    
    /* 数据缓冲区 */
    char data[APPLIB_SHM_SIZE - 0x58];     /* 剩余空间作为数据缓冲区 */
} applib_sync_shm_t;

/**
 * @brief 消息结构体 (1104 bytes)
 */
typedef struct {
    int msg_type;                          /* +0x00: 消息类型 */
    int msg_id;                            /* +0x04: 消息ID */
    int msg_size;                          /* +0x08: 消息体大小 */
    int reserved;                          /* +0x0C: 保留 */
    char sender[MAX_APP_NAME_LEN];         /* +0x10: 发送者名称 */
    char target[MAX_APP_NAME_LEN];         /* +0x90: 目标名称 */
    char data[MSG_SIZE - 0x110];           /* +0x110: 消息数据 */
} applib_msg_t;

/**
 * @brief 服务请求/响应头部结构体 (从send_service_cmd反汇编精确验证)
 *
 * send_service_cmd发送: 从request偏移0开始, 发送16字节头部+payload
 *   发送大小 = 16 + request[12] (即 sizeof(srv_req_header_t) + payload_size)
 *   request[8] (client_fd) 发送时为0, 服务端接收后回填
 *   request[4] (seq) 非0表示同步消息, 需要提供response缓冲区
 *
 * 服务端接收: 先recv 16字节头部, 再recv payload_size字节payload
 *   回填 header[8] = client_fd
 *   调用 callback(header, &resp_result)
 *
 * 响应格式与请求相同: 16字节头部 + payload
 *   响应payload从偏移16开始
 */
typedef struct {
    int cmd;                               /* +0x00: 命令字 (如0x2B75=audio_track_init) */
    int seq;                               /* +0x04: 序列号 (0=异步, 非0=同步/需响应) */
    int client_fd;                         /* +0x08: 客户端fd (发送时为0, 服务端回填) */
    int payload_size;                      /* +0x0C: payload长度 (最大4096) */
    char payload[0];                       /* +0x10: payload数据起始 */
} srv_req_header_t;

/**
 * @brief 消息分发回调函数类型
 *
 * dispatch_msg调用: blx r3 (r0=msg)
 * 只传1个参数r0=msg指针, r1未设置
 */
typedef void (*sys_msg_callback_t)(void* msg);

/**
 * @brief 服务分发回调函数类型
 *
 * handle_client_request调用: blx r8 (r0=header, r1=resp_data)
 * 传2个参数: r0=请求头部指针, r1=响应结果指针(int*)
 * 回调通过 *resp_result = value 设置响应结果
 */
typedef void (*srv_callback_t)(void* req_header, int* resp_result);

/*============================================================================
 * 全局变量声明
 *============================================================================*/

/* 当前应用信息指针 (外部可访问) */
extern app_info_t* g_this_app_info;

/*============================================================================
 * API 函数声明
 *============================================================================*/

/*----------------------------------------------------------------------------
 * 初始化与清理
 *----------------------------------------------------------------------------*/

/**
 * @brief 初始化 applib
 * @param argc 参数个数
 * @param argv 参数数组
 * @return 非0 成功(1=首次初始化成功, 更大值=已初始化), 0 失败
 * @note 从原版sair反汇编验证: 返回0=失败, 非0=成功
 */
int applib_init(int argc, char** argv);

/**
 * @brief 清理 applib
 */
void applib_quit(void);

/**
 * @brief 检查 applib 是否已初始化
 * @return 1 已初始化, 0 未初始化
 */
int is_applib_inited(void);

/*----------------------------------------------------------------------------
 * 进程管理
 *----------------------------------------------------------------------------*/

/**
 * @brief 初始化进程ID
 * @return 进程ID
 */
int pid_init(void);

/**
 * @brief 获取进程ID
 * @return 进程ID
 */
int GetPid(void);

/*----------------------------------------------------------------------------
 * 应用运行列表管理
 *----------------------------------------------------------------------------*/

/**
 * @brief 获取应用运行列表
 * @return 列表指针, NULL 失败
 */
void* app_running_list_get_list(void);

/**
 * @brief 根据名称获取应用信息
 * @param appname 应用名称
 * @return 应用信息指针, NULL 未找到
 */
void* app_running_list_get_app_info(const char* appname);

/**
 * @brief 设置应用信息
 * @param app_info 目标应用信息
 * @param new_info 新的应用信息
 * @return 0 成功, -1 失败
 */
int app_running_list_set_app_info(void* app_info, void* new_info);

/**
 * @brief 获取当前应用信息
 * @return 当前应用信息指针
 */
void* app_running_list_get_this_app_info(void);

/**
 * @brief 设置当前应用信息
 * @param new_info 新的应用信息
 * @return 0 成功, -1 失败
 */
int app_running_list_set_this_app_info(void* new_info);

/**
 * @brief 添加应用到运行列表
 * @param appname 应用名称
 * @return 0 成功, -1 失败
 */
int app_running_list_add(const char* appname);

/**
 * @brief 从运行列表删除应用
 * @param appname 应用名称
 * @return 0 成功, -1 失败
 */
int app_running_list_delete(const char* appname);

/**
 * @brief 检查应用是否存在
 * @param app_name 应用名称
 * @return 1 存在, 0 不存在
 */
int is_app_exist(const char* app_name);

/*----------------------------------------------------------------------------
 * 服务通信
 *----------------------------------------------------------------------------*/

/**
 * @brief 发送服务命令 (同步)
 * @param service_name 服务名称
 * @param request 请求结构体
 * @param response 响应结构体 (可为NULL)
 * @return 0 成功, -1 失败
 */
int send_service_cmd(const char* service_name, void* request, void* response);

/**
 * @brief 发送异步消息
 * @param service_name 目标服务名称
 * @param msg 消息结构体
 * @param msg_size 消息大小
 * @return 0 成功, -1 失败
 */
int send_async_msg(const char* service_name, void* msg, int msg_size);

/**
 * @brief 广播消息
 * @param msg 消息结构体
 * @return 0 成功, -1 失败
 */
int broadcast_msg(void* msg);

/*----------------------------------------------------------------------------
 * 消息处理
 *----------------------------------------------------------------------------*/

/**
 * @brief 分发消息
 * @return 0 成功, -1 失败
 */
int dispatch_msg(void* msg);

/**
 * @brief 获取消息
 * @param msg 消息缓冲区
 * @return 消息大小, -1 失败, 0 无消息
 */
int get_msg(void* msg);

/**
 * @brief 退出消息循环
 */
void exit_msg_loop(void);

/**
 * @brief 清除系统消息队列
 * @param msg_id 消息ID (-1 清除所有)
 * @return 0 成功, -1 失败
 */
int clear_sys_msg_queue(int msg_id);

/*----------------------------------------------------------------------------
 * 服务注册与分发
 *----------------------------------------------------------------------------*/

/**
 * @brief 注册服务分发器
 *
 * ⚠️ 参数是回调函数指针，不是服务名称！
 * 从sair.asm反汇编验证: ldr r0,[pc,#220] -> add r0,pc,r0 -> bl register_srv_dispatcher@plt
 * r0指向0x129b4处(push {r4,r5,r6,r7,lr})，是proc_srv_msg函数指针
 *
 * register_srv_dispatcher内部:
 *   1. 将callback存入global_data+0x4b4
 *   2. 从applib全局数据获取app_name作为socket路径
 *   3. 调用srv_socket_init创建Unix域socket
 *   4. 将socket_struct存入global_data+0x10
 *
 * 当服务请求到达时，get_msg -> srv_check_cmd -> handle_client_request
 * 会调用global_data+0x4b4处存储的callback
 *
 * @param callback 服务请求回调函数 (srv_callback_t类型)
 * @return 0 成功, -1 失败
 */
int register_srv_dispatcher(void* callback);

/**
 * @brief 注销服务分发器
 * @param service_name 服务名称
 * @return 0 成功, -1 失败
 */
int unregister_srv_dispatcher(const char* service_name);

/**
 * @brief 注册系统分发器
 * @param msg_type 消息类型
 * @param callback 回调函数
 * @param user_data 用户数据
 * @return 0 成功, -1 失败
 */
int register_sys_dispatcher(void* callback);

/**
 * @brief 注销系统分发器
 * @param msg_type 消息类型
 * @return 0 成功, -1 失败
 */
int unregister_sys_dispatcher(void);

/**
 * @brief 获取系统分发器
 * @return 分发器指针
 */
void* get_sys_dispatcher(void);

/*----------------------------------------------------------------------------
 * 服务生命周期
 *----------------------------------------------------------------------------*/

/**
 * @brief 启动服务
 * @param service_name 服务名称
 * @return 0 成功, -1 失败
 */
int start_service(const char* service_name);

/**
 * @brief 停止服务
 * @param service_name 服务名称
 * @return 0 成功, -1 失败
 */
int stop_service(const char* service_name);

/*----------------------------------------------------------------------------
 * 消息过滤
 *----------------------------------------------------------------------------*/

/**
 * @brief 抢占消息
 * @param msg_id 消息ID
 * @return 0 成功, -1 失败
 */
int grab_msg(int msg_id);

/**
 * @brief 释放消息
 * @param msg_id 消息ID
 * @return 0 成功, -1 失败
 */
int release_msg(int msg_id);

/**
 * @brief 过滤消息
 * @param msg_id 消息ID
 * @return 0 成功, -1 失败
 */
int filter_msg(int msg_id);

/**
 * @brief 取消过滤消息
 * @param msg_id 消息ID
 * @return 0 成功, -1 失败
 */
int unfilter_msg(int msg_id);

/*----------------------------------------------------------------------------
 * 配置管理
 *----------------------------------------------------------------------------*/

/* 配置文件路径约定 */
#define CONFIG_FILE_PATH        "/etc/config/appconfig.cfg"
#define CONFIG_DIR_PATH         "/etc/config/"
#define CONFIG_BACKUP_PATH      "/var/config/"
#define CONFIG_MAX_KEY_LEN      128
#define CONFIG_MAX_VALUE_LEN    256
#define CONFIG_MAX_ENTRIES      256
#define CONFIG_CACHE_SIZE       0x234c  /* 9036 bytes - 与 APPLIB_DIR_CFG_SIZE 一致 */

/* 配置项类型 */
typedef enum {
    CONFIG_TYPE_INT = 0,
    CONFIG_TYPE_STRING = 1,
    CONFIG_TYPE_BOOL = 2,
    CONFIG_TYPE_FLOAT = 3
} config_type_e;

/* 配置项状态 */
typedef enum {
    CONFIG_STATE_VALID = 0,
    CONFIG_STATE_INVALID = 1,
    CONFIG_STATE_PENDING = 2
} config_state_e;

/**
 * @brief 配置项结构体
 */
typedef struct {
    char key[CONFIG_MAX_KEY_LEN];       /* 配置键名 */
    char value[CONFIG_MAX_VALUE_LEN];   /* 配置值 */
    config_type_e type;                 /* 值类型 */
    config_state_e state;               /* 状态 */
    uint32_t hash;                      /* 键哈希值 (用于快速查找) */
    uint32_t timestamp;                 /* 最后更新时间戳 */
    uint16_t flags;                     /* 标志位 */
    uint16_t ref_count;                 /* 引用计数 */
} config_entry_t;

/**
 * @brief 配置缓存结构体 (共享内存布局)
 * 
 * ⚠️ 注意: 此结构体定义与实际共享内存大小严重不符:
 * - 共享内存大小: 0x234c (9036 bytes)
 * - 当前定义计算: mutex(24) + magic(4) + version(4) + entry_count(4) + 
 *   last_update(4) + flags(4) + config_file[256](256) + 
 *   entries[256](256 * sizeof(config_entry_t)) >> 9036
 * - config_entry_t 每项404字节 * 256 = 103424，远超9036
 * - 实际的 config_entry_t 必须远小于当前定义，或entries数量远小于256
 * - 需要通过hexdump共享内存 /.dir_cfg 来逆向实际布局
 * 
 * 共享内存名称: /.dir_cfg
 * 大小: 0x234c (9036 bytes)
 */
typedef struct {
    pthread_mutex_t mutex;              /* +0x00: 配置访问互斥锁 */
    uint32_t magic;                     /* 偏移待验证 */
    uint32_t version;                   /* 偏移待验证 */
    uint32_t entry_count;               /* 偏移待验证 */
    uint32_t last_update;               /* 偏移待验证 */
    uint32_t flags;                     /* 偏移待验证 */
    char config_file[256];              /* 偏移待验证 */
    /* ⚠️ entries数组的元素大小和数量待验证 */
    char entries_data[0];               /* 占位，实际布局待确认 */
} config_cache_t;

/**
 * @brief 配置变更回调函数类型
 * @param key 配置键名
 * @param old_value 旧值
 * @param new_value 新值
 * @param user_data 用户数据
 * @return 0 成功, -1 失败
 */
typedef int (*config_change_callback_t)(const char* key, const char* old_value, 
                                         const char* new_value, void* user_data);

/**
 * @brief 配置监听器结构体
 */
typedef struct {
    char key_pattern[CONFIG_MAX_KEY_LEN];   /* 键模式 (支持通配符) */
    config_change_callback_t callback;       /* 回调函数 */
    void* user_data;                         /* 用户数据 */
    int enabled;                             /* 是否启用 */
} config_listener_t;

/**
 * @brief 初始化配置
 * @return 0 成功, -1 失败
 */
int config_so_init(void);

/**
 * @brief 清理配置
 */
void config_so_exit(void);

/**
 * @brief 获取配置值
 * @param key 配置键
 * @param value 值缓冲区
 * @param value_size 缓冲区大小
 * @return 0 成功, -1 失败
 */
int get_config(const char* key, char* value, int value_size);

/**
 * @brief 设置配置值
 * @param key 配置键
 * @param value 配置值
 * @param value_size 值大小
 * @return 0 成功, -1 失败
 */
int set_config(const char* key, const char* value, int value_size);

/**
 * @brief 重置配置到默认值
 * @return 0 成功, -1 失败
 */
int reset_config(void);

/**
 * @brief 获取配置整数值
 * @param key 配置键
 * @param default_value 默认值
 * @return 配置值
 */
int get_config_int(const char* key, int default_value);

/**
 * @brief 设置配置整数值
 * @param key 配置键
 * @param value 配置值
 * @return 0 成功, -1 失败
 */
int set_config_int(const char* key, int value);

/**
 * @brief 获取配置字符串值
 * @param key 配置键
 * @param default_value 默认值
 * @param buf 输出缓冲区
 * @param buf_size 缓冲区大小
 * @return 0 成功, -1 失败
 */
int get_config_string(const char* key, const char* default_value, 
                      char* buf, int buf_size);

/**
 * @brief 设置配置字符串值
 * @param key 配置键
 * @param value 配置值
 * @return 0 成功, -1 失败
 */
int set_config_string(const char* key, const char* value);

/**
 * @brief 注册配置变更监听器
 * @param key_pattern 键模式 (支持 * 通配符)
 * @param callback 回调函数
 * @param user_data 用户数据
 * @return 监听器ID, -1 失败
 */
int register_config_listener(const char* key_pattern, 
                             config_change_callback_t callback, 
                             void* user_data);

/**
 * @brief 注销配置变更监听器
 * @param listener_id 监听器ID
 * @return 0 成功, -1 失败
 */
int unregister_config_listener(int listener_id);

/**
 * @brief 重新加载配置文件
 * @return 0 成功, -1 失败
 */
int reload_config(void);

/**
 * @brief 保存配置到文件
 * @return 0 成功, -1 失败
 */
int save_config(void);

/**
 * @brief 启动配置热更新线程
 * @return 0 成功, -1 失败
 */
int start_config_hot_reload(void);

/**
 * @brief 停止配置热更新线程
 */
void stop_config_hot_reload(void);

/**
 * @brief 目录配置初始化
 * @return 0 成功, -1 失败
 */
int applib_dir_cfg_init(void);

/**
 * @brief 目录配置清理
 */
void applib_dir_cfg_deinit(void);

/**
 * @brief 读取目录配置
 * @param key 配置键
 * @param value 输出缓冲区
 * @param buf_size 缓冲区大小
 * @return 0 成功, -1 失败
 */
int applib_dir_cfg_read(const char* key, char* value, int buf_size);

/**
 * @brief 写入目录配置
 * @param key 配置键
 * @param value 配置值
 * @return 0 成功, -1 失败
 */
int applib_dir_cfg_write(const char* key, const char* value);

/*----------------------------------------------------------------------------
 * 辅助函数
 *----------------------------------------------------------------------------*/

/**
 * @brief 获取应用名称
 * @return 应用名称字符串
 */
const char* get_app_name(void);

/**
 * @brief 获取应用类型
 * @param appname 应用名称
 * @param type 类型输出
 * @return 0 成功, -1 失败
 */
int get_app_type(const char* appname, int* type);

/**
 * @brief 获取应用PID
 * @param appname 应用名称
 * @return PID, -1 失败
 */
int get_app_pid(const char* appname);

/**
 * @brief 打印应用运行列表 (调试用)
 */
void print_app_running_list(void);

#endif /* APPLIB_API_H */
