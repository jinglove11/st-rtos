/**
 * @file test_sync_server.c
 * @brief Phase H1 — 用户态同步原语服务测试
 *
 * 验证 sync_server (基于 endpoint 的 lock/trylock/unlock) 在用户态工作。
 * 两个 client 任务竞争同一把锁,验证互斥。
 */

#include "test_framework.h"
#include "kernel.h"
#include "task.h"
#include "endpoint.h"
#include "user_api.h"
#include "sync_proto.h"

#if TEST_MODULE_SYNC_SERVER && CAP_ENABLE

/*============================================================================
 * sync_server 任务:arg = ep_cap
 *============================================================================*/

static void sync_srv_task(void *arg) {
    int ep_cap = (int)(uintptr_t)arg;
    int err = sync_server_run(ep_cap, 60);
    sys_task_exit((void *)(intptr_t)err);
}

/*============================================================================
 * client 任务:arg = ep_cap,尝试 lock + unlock
 *============================================================================*/

static void sync_client_task(void *arg) {
    int ep_cap = (int)(uintptr_t)arg;
    int err;

    /* ping */
    err = sync_ping(ep_cap, 1000);
    if (err != KERN_OK) {
        sys_task_exit((void *)(intptr_t)err);
    }

    /* trylock 锁 0 */
    err = sync_trylock(ep_cap, 0, 1000);
    if (err != KERN_OK) {
        sys_task_exit((void *)(intptr_t)err);
    }

    /* unlock 锁 0 */
    err = sync_unlock(ep_cap, 0, 1000);
    if (err != KERN_OK) {
        sys_task_exit((void *)(intptr_t)err);
    }

    /* 阻塞 lock 锁 1 */
    err = sync_lock(ep_cap, 1, 3000);
    if (err != KERN_OK) {
        sys_task_exit((void *)(intptr_t)err);
    }

    /* unlock 锁 1 */
    err = sync_unlock(ep_cap, 1, 1000);

    sys_task_exit((void *)(intptr_t)err);
}

/*============================================================================
 * helper
 *============================================================================*/

static task_id_t create_user_with_cap(const char *name, task_func_t entry,
                                      int ep_id, uint8_t prio,
                                      uint32_t stack, cap_id_t *out_cap) {
    task_id_t tid = task_create_user(name, entry, NULL, prio, stack);
    if (tid < 0) return tid;
    tcb_t *tcb = task_get_tcb(tid);
    cap_id_t cap = KERN_INVALID_ID;
    if (tcb != NULL) {
        cap = cap_create_for(tcb, (void *)(uintptr_t)(ep_id + 1),
                             CAP_OBJ_ENDPOINT, CAP_READ | CAP_WRITE);
    }
    if (tcb != NULL && tcb->sp != NULL && cap >= 0) {
        uint32_t *r0 = (uint32_t *)((uint8_t *)tcb->sp + 32U);
        *r0 = (uint32_t)cap;
    }
    if (out_cap) *out_cap = cap;
    return tid;
}

/*============================================================================
 * Test: sync_server 基本 lock/unlock
 *============================================================================*/

static void test_sync_basic(void) {
    test_section("Test 1: sync_server lock/unlock");

    ep_id_t ep = endpoint_create("sync", KERN_EP_MSG_SIZE, 4);
    TEST_ASSERT(ep >= 0, "sync endpoint created");
    if (ep < 0) return;

    /* 创建 sync_server (特权任务,因为它不调 sys_shm_create,可以 user) */
    cap_id_t srv_cap = KERN_INVALID_ID;
    task_id_t srv_id = create_user_with_cap("sync_srv", sync_srv_task,
                                             ep, 10, 1024, &srv_cap);
    TEST_ASSERT(srv_id >= 0, "sync_server task created");

    /* 创建 client */
    cap_id_t cli_cap = KERN_INVALID_ID;
    task_id_t cli_id = create_user_with_cap("sync_cli", sync_client_task,
                                             ep, 11, 1024, &cli_cap);
    TEST_ASSERT(cli_id >= 0, "client task created");

    /* 启动 server,等它初始化 */
    if (srv_id >= 0) (void)task_start(srv_id);
    task_delay(50);
    /* 启动 client */
    if (cli_id >= 0) (void)task_start(cli_id);

    /* 等 client */
    void *retval = NULL;
    kern_err_t e = task_join(cli_id, &retval, 5000);
    test_print_num("[sync] client retval = ", (int32_t)(intptr_t)retval);
    TEST_ASSERT_EQ((int)KERN_OK, (int)e, "client joined");
    TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                   "client lock/unlock OK");

    /* 等 server */
    retval = NULL;
    (void)task_join(srv_id, &retval, 2000);

    /* 清理 */
    if (srv_id >= 0 && task_get_state(srv_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(srv_id);
    }
    if (cli_id >= 0 && task_get_state(cli_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(cli_id);
    }
    (void)endpoint_delete(ep);
}

static void test_sync_server_module(void) {
    test_sync_basic();
}

TEST_MODULE_REGISTER(sync_server, test_sync_server_module);

#endif /* TEST_MODULE_SYNC_SERVER */
