/**
 * @file init.c
 * @brief Phase C4 — user-mode init as service orchestrator
 *
 * init 现在是常驻编排器 (不再 spawn supervisor 就退出):
 *   1. sys_ep_create 创建 fs_server 的 endpoint (CAP_FULL)
 *   2. sys_task_create 创建 fs_server 用户任务,arg=ep_cap
 *   3. sys_cap_transfer_to 把 ep_cap 装进 fs_server cspace
 *   4. sys_task_start 启动 fs_server
 *   5. spawn supervisor (监控)
 *   6. init 常驻 (作为服务树根,持有各服务的 task cap)
 *
 * 关键:cap_transfer_to 保持 cap_id 不变 (cap_move_to 用原 slot+generation),
 * 所以 init 用 sys_task_create 的 arg=ep_cap,子任务 r0 收到的就是它的 ep_cap。
 *
 * 拉起位置:test_framework.c bootstrap 段 (test 后,shell 前)。
 * 生产 (TEST_ENABLE=n) 时 init 是唯一 bootstrap 拉起的任务。
 */

#include "kernel_config.h"

#if INIT_PROCESS

#include "supervisor.h"
#include "user_api.h"
#include "fs_proto.h"
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

/* fs_server 的任务体 (复用 fs_server.c 的 fs_server_run)。
 * arg = ep_cap (通过 r0 传入)。 */
static void fs_server_entry(void *arg) {
    int ep_cap = (int)(uintptr_t)arg;
    int err = fs_server_run(ep_cap, 0);  /* 0 = 永久循环 */
    sys_task_exit((void *)(intptr_t)err);
}

void init_main(void *arg) {
    (void)arg;

    /* ---- 拉起 fs_server ---- */
    /* 1. 创建 endpoint (init 持有 CAP_FULL ep_cap) */
    int fs_ep_cap = sys_ep_create("fs_server", 128, 4);
    if (fs_ep_cap <= 0) {
        /* endpoint 创建失败,系统降级 (无 fs_server) */
    } else {
        /* 1b. derive 一份 ep_cap 给 init 自己 (用于 ping/管理)。
         *    derive 的 cap_id 与父不同,transfer 用原始 fs_ep_cap。 */
        int fs_self_cap = sys_cap_derive(fs_ep_cap,
                                         CAP_READ | CAP_WRITE);
        /* 2. 创建 fs_server 任务,arg=ep_cap (r0 收到 ep_cap) */
        int fs_task_cap = sys_task_create("fs_server", fs_server_entry,
                                          (void *)(uintptr_t)fs_ep_cap,
                                          FS_SERVER_PRIORITY, FS_SERVER_STACK);
        if (fs_task_cap >= 0) {
            /* 3. 把原始 ep_cap 转移给 fs_server (保持 cap_id 不变,
             *    fs_server r0 收到的 arg 就是它的 ep_cap) */
            int xfer = sys_cap_transfer_to(fs_ep_cap, fs_task_cap);
            if (xfer == KERN_OK) {
                /* 4. 启动 fs_server */
                (void)sys_task_start(fs_task_cap);

                /* 4b. 等 fs_server 初始化,然后 ping 验存活 */
                sys_task_delay(100);
                if (fs_self_cap > 0) {
                    (void)fs_ping(fs_self_cap, 500);
                }
            }
            /* init 保留 fs_task_cap (管理) + fs_self_cap (ping) */
        }
    }

    /* ---- 拉起 supervisor ---- */
#if INIT_HAS_SUPERVISOR
    int sup = sys_task_create("supervisor", supervisor_monitor_loop, NULL,
                              SUPERVISOR_PRIORITY, SUPERVISOR_STACK);
    if (sup >= 0) {
        (void)sys_task_start(sup);
    }
#endif

    /* ---- init 常驻 ----
     * init 作为服务树根,持有各服务的 task cap。
     * 它睡在一个无限循环里 (未来可扩展为监控/管理逻辑)。 */
    while (1) {
        sys_task_delay(1000);
    }
}

#endif /* INIT_PROCESS */
