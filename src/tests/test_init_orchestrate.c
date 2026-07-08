/**
 * @file test_init_orchestrate.c
 * @brief Phase C — sys_ep_sender 测试
 *
 * 验证服务端能拿到最近一次 recv 的 sender task id (sys_ep_sender)。
 * 这是 fs_server fd 归属追踪 + 客户端死亡精确清理的基础。
 *
 * 服务任务通过 r0 接收 ep_cap,sender id 通过 exit retval 返回
 * (user 任务不能用全局 .bss,MPU 禁止)。
 */

#include "test_framework.h"
#include "kernel.h"
#include "task.h"
#include "endpoint.h"
#include "user_api.h"

#if TEST_MODULE_INIT_ORCHESTRATE && CAP_ENABLE

/*============================================================================
 * 服务任务 (user):recv 后用 sys_ep_sender 拿 sender,sender 通过 retval 返回
 *============================================================================*/

static void sender_svc_task(void *arg) {
    int ep_cap = (int)(uintptr_t)arg;
    uint8_t buf[KERN_EP_MSG_SIZE];
    uint32_t i;
    for (i = 0; i < KERN_EP_MSG_SIZE; i++) buf[i] = 0;

    int err = sys_ep_recv(ep_cap, buf, 2000);
    if (err != KERN_OK) {
        sys_task_exit((void *)(intptr_t)err);
    }

    /* 核心测试:sys_ep_sender 拿 sender id */
    int sender = sys_ep_sender(ep_cap);

    (void)sys_ep_reply(ep_cap, buf);
    sys_task_exit((void *)(intptr_t)sender);  /* sender 通过 retval 返回 */
}

/*============================================================================
 * Test 1: sys_ep_sender 返回正确的 sender task id
 *============================================================================*/

static void test_ep_sender(void) {
    test_section("Test 1: sys_ep_sender returns sender task id");

    ep_id_t ep = endpoint_create("sndr_ep", KERN_EP_MSG_SIZE, 2);
    TEST_ASSERT(ep >= 0, "endpoint created");
    if (ep < 0) return;

    /* 创建 user 服务任务 */
    task_id_t svc_id = task_create_user("sndr_svc", sender_svc_task,
                                        NULL, 11, 1024);
    TEST_ASSERT(svc_id >= 0, "service task created");
    if (svc_id < 0) { (void)endpoint_delete(ep); return; }

    /* 内核态给服务任务装 ep cap */
    tcb_t *svc_tcb = task_get_tcb(svc_id);
    cap_id_t svc_ep_cap = KERN_INVALID_ID;
    if (svc_tcb != NULL) {
        svc_ep_cap = cap_create_for(svc_tcb, (void *)(uintptr_t)(ep + 1),
                                    CAP_OBJ_ENDPOINT, CAP_READ | CAP_WRITE);
    }
    TEST_ASSERT(svc_ep_cap > 0, "service got ep cap");

    /* 给 client (本测试任务) 装 ep cap */
    cap_id_t cli_ep_cap = cap_create_for(sched_get_current(),
                                         (void *)(uintptr_t)(ep + 1),
                                         CAP_OBJ_ENDPOINT, CAP_READ | CAP_WRITE);
    TEST_ASSERT(cli_ep_cap > 0, "client got ep cap");

    /* 服务任务通过 r0 接收 svc_ep_cap */
    if (svc_tcb != NULL && svc_tcb->sp != NULL) {
        uint32_t *r0 = (uint32_t *)((uint8_t *)svc_tcb->sp + 32U);
        *r0 = (uint32_t)svc_ep_cap;
    }

    /* 启动服务 */
    kern_err_t e = task_start(svc_id);
    TEST_ASSERT_EQ((int)KERN_OK, (int)e, "service started");

    /* client send */
    uint8_t msg[KERN_EP_MSG_SIZE];
    for (int i = 0; i < KERN_EP_MSG_SIZE; i++) msg[i] = 0;
    msg[0] = 0x55;
    int send_err = sys_ep_send((int)cli_ep_cap, msg, 2000);
    TEST_ASSERT_EQ((int)KERN_OK, send_err, "client send OK");

    /* 等 service,sender id 在 retval 里 */
    void *retval = NULL;
    e = task_join(svc_id, &retval, 2000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)e, "service joined (no fault)");

    /* 核心:service 看到的 sender 应该是 client (本测试任务) */
    tcb_t *me = sched_get_current();
    int svc_sender = (int)(intptr_t)retval;
    test_print_num("[sndr] service saw sender = ", (int32_t)svc_sender);
    test_print_num("[sndr] client task id     = ",
                   (int32_t)(me ? me->id : -1));
    if (me != NULL) {
        TEST_ASSERT_EQ((int)me->id, svc_sender,
                       "service saw client as sender");
    }

    /* 清理 */
    if (cli_ep_cap > 0) (void)sys_cap_revoke((int)cli_ep_cap);
    if (svc_id >= 0 && task_get_state(svc_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(svc_id);
    }
    (void)endpoint_delete(ep);
}

static void test_init_orchestrate_module(void) {
    test_ep_sender();
}

TEST_MODULE_REGISTER(init_orchestrate, test_init_orchestrate_module);

#endif /* TEST_MODULE_INIT_ORCHESTRATE */
