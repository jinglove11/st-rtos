/**
 * @file init.c
 * @brief Phase G — user-mode init as PID-1 service orchestrator
 *
 * init 拉起基础服务生态:
 *   1. nameserver (服务发现)
 *   2. fs_server (自管 inode池+ramfs, 注册到 nameserver "fs.ramfs")
 *   3. supervisor (故障重启)
 *
 * shell 启动时 lookup "fs.ramfs" 发现 fs_server,ls/cat 自动可用。
 */

#include "kernel_config.h"

#if INIT_PROCESS

#include "supervisor.h"
#include "user_api.h"
#include "fs_proto.h"
#include "nameserver.h"
#include <stdint.h>

#if FAULT_ENDPOINT && SUPERVISOR
#define INIT_HAS_SUPERVISOR 1
#else
#define INIT_HAS_SUPERVISOR 0
#endif

#define SUPERVISOR_PRIORITY   2
#define SUPERVISOR_STACK      2048

#define FS_SERVER_PRIORITY    8
#define FS_SERVER_STACK       2048

#define NS_PRIORITY           6
#define NS_STACK              1536

/* 服务启动后从自己的 CNode 取 cap；Move 后目标 CPtr 与源 CPtr 不同。 */
static void fs_server_entry(void *arg) {
    (void)arg;
    int ep_cap = sys_cap_self_slot(CAP_OBJ_ENDPOINT, 0);
    int err = fs_server_run(ep_cap, 0);
    sys_task_exit((void *)(intptr_t)err);
}

/* nameserver 的任务体:arg = ep_cap */
static void ns_entry(void *arg) {
    (void)arg;
    int ep_cap = sys_cap_self_slot(CAP_OBJ_ENDPOINT, 0);
    int err = nameserver_service_run(ep_cap, 0);
    sys_task_exit((void *)(intptr_t)err);
}

/* helper:创建 user 服务任务 + endpoint + transfer cap。
 * 返回 init 持有的 service ep cap (derived,带 TRANSFER 用于注册)。
 * 失败返回负数。out_task_cap 返回 task cap。 */
static int init_spawn_service(const char *name, task_func_t entry,
                              uint8_t prio, uint32_t stack,
                              int *out_task_cap) {
    /* 创建 endpoint (init 持有 CAP_FULL) */
    int ep_cap = sys_ep_create(name, 128, 4);
    if (ep_cap <= 0) {
        return ep_cap;
    }

    /* derive 一份给 init 自己 (带 TRANSFER,用于注册到 nameserver) */
    int self_cap = sys_cap_derive(ep_cap, CAP_READ | CAP_WRITE | CAP_TRANSFER);
    if (self_cap <= 0) {
        return self_cap;
    }

    /* 创建服务任务；服务从目标 CNode 查询 Move 后的新 CPtr。 */
    int task_cap = sys_task_create(name, entry, NULL,
                                   prio, stack);
    if (task_cap < 0) {
        return task_cap;
    }

    /* 把原始 ep_cap Move 给服务，目标 CNode 会生成新的本地 CPtr。 */
    int xfer = sys_cap_transfer_to(ep_cap, task_cap);
    if (xfer != KERN_OK) {
        return xfer;
    }

    /* 启动服务 */
    (void)sys_task_start(task_cap);
    if (out_task_cap) {
        *out_task_cap = task_cap;
    }
    return self_cap;
}

void init_main(void *arg) {
    (void)arg;

    /* ---- 1. 拉起 nameserver ---- */
    int ns_task_cap = -1;
    int ns_self_cap = init_spawn_service("nameserver", ns_entry,
                                         NS_PRIORITY, NS_STACK, &ns_task_cap);
    if (ns_self_cap <= 0) {
        /* nameserver 是基础服务,失败则降级 */
    }

    /* 等 nameserver 启动 */
    sys_task_delay(50);

    /* ---- 2. 拉起 fs_server ---- */
    int fs_task_cap = -1;
    int fs_self_cap = init_spawn_service("fs_server", fs_server_entry,
                                         FS_SERVER_PRIORITY, FS_SERVER_STACK,
                                         &fs_task_cap);
    if (fs_self_cap <= 0) {
        /* fs_server 失败 */
    } else {
        /* 等 fs_server 初始化 */
        sys_task_delay(100);

        /* 把 fs_server 注册到 nameserver "fs.ramfs" */
        if (ns_self_cap > 0) {
            (void)nameserver_register(ns_self_cap, "fs.ramfs",
                                      fs_self_cap, 0x494E4954U, 1000);
        }
    }

    /* ---- 3. 拉起 supervisor ---- */
#if INIT_HAS_SUPERVISOR
    int sup = sys_task_create("supervisor", supervisor_monitor_loop, NULL,
                              SUPERVISOR_PRIORITY, SUPERVISOR_STACK);
    if (sup >= 0) {
        (void)sys_task_start(sup);
    }
#endif

    /* ---- init 常驻 ---- */
    while (1) {
        sys_task_delay(1000);
    }
}

#endif /* INIT_PROCESS */
