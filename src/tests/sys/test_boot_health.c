/**
 * @file test_boot_health.c
 * @brief SYS 层 — 启动健康检查
 *
 * 在 init/supervisor 经 root bootstrap 启动后运行,断言系统编排接线:
 *   1. supervisor 拿到 fault ep 初始授权并阻塞在其 recv 队列
 *      (直接抓 H1:sys_fault_subscribe 拒绝用户任务导致的静默睡死)
 *   2. init 存活(阻塞在主循环 delay 上)
 *
 * SYS 层红的含义:内核与用户态各自可能都对,是接缝断了。
 */

#include "test_framework.h"
#include "kernel.h"
#include "task.h"
#include "scheduler.h"
#if CAP_ENABLE
#include "capability.h"
#endif
#if FAULT_ENDPOINT
#include "fault_endpoint.h"
#include "endpoint.h"
#endif

#if INIT_PROCESS && CAP_ENABLE && FAULT_ENDPOINT

#include <string.h>

/* 按名找任务(任务池线性扫描);找不到返回 KERN_INVALID_ID */
static task_id_t boot_health_find_task(const char *name) {
    for (int i = 0; i < KERNEL_MAX_TASKS; i++) {
        tcb_t *tcb = task_get_tcb((task_id_t)i);
        if (tcb == NULL) {
            continue;
        }
        const char *n = task_get_name((task_id_t)i);
        if (n != NULL && strcmp(n, name) == 0) {
            return (task_id_t)i;
        }
    }
    return KERN_INVALID_ID;
}

static void test_boot_supervisor_subscribed(void) {
    test_section("boot: supervisor subscribed to fault ep");

    /* supervisor 启动需要一点时间走到 sys_ep_recv;轮询等待 */
    task_id_t sup = KERN_INVALID_ID;
    int subscribed = 0;
    for (int i = 0; i < 100 && !subscribed; i++) {
        task_delay(5);
        sup = boot_health_find_task("supervisor");
        if (sup != KERN_INVALID_ID &&
            endpoint_recv_has_waiter(kern_fault_ep, sup)) {
            subscribed = 1;
        }
    }

    TEST_ASSERT_NE(KERN_INVALID_ID, sup, "supervisor task exists");
    TEST_ASSERT_EQ(1, subscribed,
                   "supervisor blocked on fault ep recv (H1 wiring)");
}

static void test_boot_init_alive(void) {
    test_section("boot: init alive");

    task_id_t init_tid = KERN_INVALID_ID;
    int alive = 0;
    for (int i = 0; i < 50 && !alive; i++) {
        task_delay(5);
        init_tid = boot_health_find_task("init");
        if (init_tid != KERN_INVALID_ID) {
            task_state_t state = task_get_state(init_tid);
            /* init 主循环 delay:应处于 BLOCKED;CREATED=没启动,TERMINATED=死了 */
            alive = (state != TASK_STATE_CREATED &&
                     state != TASK_STATE_TERMINATED);
        }
    }

    TEST_ASSERT_NE(KERN_INVALID_ID, init_tid, "init task exists");
    TEST_ASSERT_EQ(1, alive, "init alive (blocked in main loop)");
}

static void test_boot_health_module(void) {
    test_boot_supervisor_subscribed();
    test_boot_init_alive();
}

TEST_SYS_MODULE(boot_health, test_boot_health_module);

#endif /* INIT_PROCESS && CAP_ENABLE && FAULT_ENDPOINT */
