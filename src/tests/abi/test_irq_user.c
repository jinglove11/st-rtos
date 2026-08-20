/**
 * @file test_irq_user.c
 * @brief ABI 层 — 用户任务 IRQ bind syscall 契约
 *
 * 用户任务持 IRQ cap + endpoint cap 调 sys_irq_bind,经 endpoint 收到
 * 通知;rights 不足被拒。
 */

#include "test_framework.h"
#include "board_config.h"
#include "irq.h"
#include "bh.h"
#include "endpoint.h"
#include "task.h"
#include "hal.h"
#include "kernel.h"
#include "trace.h"
#include "stats.h"
#include "capability.h"
#include "user_api.h"

#if TEST_ENABLE
/* P0-5 验证补漏:TEST-off 镜像(dev/release/tiny)不得链入测试代码。
 * TEST_ENABLE 为测试代码链接总门(与既有模块级 TEST_MODULE_* 门互补)。 */

#define TEST_IRQ_USER_BIND      ((int16_t)(BOARD_IRQ_COUNT - 2U))
#define TEST_IRQ_BIND_RIGHTS    ((int16_t)(BOARD_IRQ_COUNT - 1U))


static void irq_user_bind_task(void *arg) {
    (void)arg;
    cap_id_t ep_cap = sys_cap_self_slot(CAP_OBJ_ENDPOINT, 0);
    cap_id_t irq_cap = sys_cap_self_slot(CAP_OBJ_IRQ, 0);
    uint8_t msg_buf[KERN_EP_MSG_SIZE] = {0};
    uint32_t *msg = (uint32_t *)msg_buf;
    int err;

    if (ep_cap <= 0 || irq_cap <= 0) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_PARAM);
    }

    err = sys_irq_bind((int)irq_cap, (int)ep_cap, 0x55514952);
    if (err == KERN_OK) {
        err = sys_ep_recv((int)ep_cap, msg_buf, 1000);
    }
    if (err == KERN_OK && msg[0] != 0x55514952U) {
        err = KERN_ERR_STATE;
    }
    if (err == KERN_OK && msg[1] != (uint32_t)TEST_IRQ_USER_BIND) {
        err = KERN_ERR_STATE;
    }
    sys_task_exit((void *)(intptr_t)err);
}

static void irq_user_bind_once_task(void *arg) {
    (void)arg;
    cap_id_t ep_cap = sys_cap_self_slot(CAP_OBJ_ENDPOINT, 0);
    cap_id_t irq_cap = sys_cap_self_slot(CAP_OBJ_IRQ, 0);
    int err;

    err = sys_irq_bind((int)irq_cap, (int)ep_cap, 0x42495251);
    sys_task_exit((void *)(intptr_t)err);
}

/* Run the producer as a real task instead of polling from test_runner.
 *
 * test_runner has priority 10 while the user task deliberately runs at 13.
 * Repeated task_delay(1) calls from test_runner are not a readiness handshake:
 * with the SMP service tasks active, test_runner can wake every tick before the
 * lower-priority user task has executed sys_irq_bind().  The old test therefore
 * observed NOEXIST for every early notify, then blocked in task_join; only then
 * did the user bind and eventually time out because notification had stopped.
 *
 * This producer is lower priority than the user task.  Once test_runner joins,
 * the user runs first, publishes the IRQ binding and blocks in recv; the
 * producer then delivers the notification.  The retry loop still covers the
 * legitimate cross-core race where both tasks start simultaneously. */
static void irq_user_notify_task(void *arg) {
    int16_t irq = (int16_t)(intptr_t)arg;
    kern_err_t err = KERN_ERR_NOEXIST;

    for (int i = 0; i < 16 && err != KERN_OK; i++) {
        (void)task_delay(1);
        err = irq_notify(irq);
    }
    task_exit((void *)(intptr_t)err);
}


/*============================================================================
 * 测试 11: user task binds IRQ cap to endpoint
 *============================================================================*/

static void test_irq_user_cap_endpoint_bind(void) {
    test_section("Test 11: user IRQ cap endpoint bind syscall");

#if CAP_ENABLE && IPC_ENDPOINT && SYSCALL_ENABLE
    uint16_t cap_free_before = cap_free_count();
    ep_id_t ep = endpoint_create("irq_user_bind_ep", sizeof(uint32_t) * 2U, 2);
    TEST_ASSERT(ep >= 0, "user IRQ bind endpoint created");
    if (ep < 0) {
        return;
    }

    cap_id_t irq_cap = KERN_INVALID_ID;
    kern_err_t err = kirq_create_cap(TEST_IRQ_USER_BIND,
                                     CAP_READ | CAP_WRITE | CAP_MANAGE |
                                         CAP_TRANSFER,
                                     &irq_cap);
    TEST_ASSERT_EQ(KERN_OK, err, "user IRQ bind cap created");
    TEST_ASSERT(irq_cap >= 0, "user IRQ bind cap valid");

    task_id_t user_id = task_create_user("irq_user_bind",
                                         irq_user_bind_task,
                                         NULL, 13, 768);
    TEST_ASSERT(user_id >= 0, "user IRQ bind task created");

    cap_id_t user_ep_cap = KERN_INVALID_ID;
    cap_id_t user_irq_cap = KERN_INVALID_ID;
    task_id_t notifier_id = KERN_INVALID_ID;
    tcb_t *user = task_get_tcb(user_id);
    if (user != NULL) {
        user_ep_cap = cap_create_for(user, endpoint_obj_for_cap(ep), CAP_OBJ_ENDPOINT,
                                     CAP_READ | CAP_WRITE);
        user_irq_cap = cap_copy_to(NULL, irq_cap, user,
                                   CAP_READ | CAP_WRITE | CAP_TRANSFER);
    }
    TEST_ASSERT(user_ep_cap >= 0, "user receives endpoint cap");
    TEST_ASSERT(user_irq_cap >= 0, "user receives IRQ cap");

    if (user_id >= 0 && user_ep_cap >= 0 && user_irq_cap >= 0) {
        notifier_id = task_create("irq_user_notify", irq_user_notify_task,
                                  (void *)(intptr_t)TEST_IRQ_USER_BIND,
                                  14, 512);
        TEST_ASSERT(notifier_id >= 0, "user IRQ notifier task created");
        err = task_start(user_id);
        TEST_ASSERT_EQ(KERN_OK, err, "user IRQ bind task started");
        if (notifier_id >= 0) {
            err = task_start(notifier_id);
            TEST_ASSERT_EQ(KERN_OK, err, "user IRQ notifier task started");
        }
    }

    void *retval = NULL;
    if (user_id >= 0) {
        err = task_join(user_id, &retval, 2000);
        TEST_ASSERT_EQ(KERN_OK, err, "user IRQ bind task joined");
        TEST_ASSERT_EQ(KERN_OK, (kern_err_t)(intptr_t)retval,
                       "user IRQ bind recv returned OK");
    }

    retval = NULL;
    if (notifier_id >= 0) {
        err = task_join(notifier_id, &retval, 2000);
        TEST_ASSERT_EQ(KERN_OK, err, "user IRQ notifier task joined");
        TEST_ASSERT_EQ(KERN_OK, (kern_err_t)(intptr_t)retval,
                       "user-bound IRQ notification sent");
    }

    if (user_id >= 0 &&
        task_get_state(user_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(user_id);
    }
    if (notifier_id >= 0 &&
        task_get_state(notifier_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(notifier_id);
    }
    if (irq_cap > 0) {
        TEST_ASSERT_EQ(KERN_OK, kirq_delete_cap(irq_cap),
                       "user IRQ bind cap deleted");
    }
    endpoint_delete(ep);
    TEST_ASSERT_EQ((int)cap_free_before, (int)cap_free_count(),
                   "user IRQ bind cleanup restored cap count");
#else
    test_skip("capability, endpoint, or syscall disabled");
#endif
}

/*============================================================================
 * 测试 12: user IRQ bind syscall rejects missing rights
 *============================================================================*/

static void test_irq_user_cap_endpoint_bind_rights(void) {
    test_section("Test 12: user IRQ bind syscall rights");

#if CAP_ENABLE && IPC_ENDPOINT && SYSCALL_ENABLE
    uint16_t cap_free_before = cap_free_count();
    ep_id_t ep = endpoint_create("irq_bind_rights_ep",
                                 sizeof(uint32_t) * 2U, 2);
    TEST_ASSERT(ep >= 0, "IRQ bind-rights endpoint created");
    if (ep < 0) {
        return;
    }

    cap_id_t irq_cap = KERN_INVALID_ID;
    kern_err_t err = kirq_create_cap(TEST_IRQ_BIND_RIGHTS,
                                     CAP_READ | CAP_WRITE | CAP_MANAGE |
                                         CAP_TRANSFER,
                                     &irq_cap);
    TEST_ASSERT_EQ(KERN_OK, err, "IRQ bind-rights cap created");

    task_id_t no_irq_write = task_create_user("irq_bind_ro_irq",
                                              irq_user_bind_once_task,
                                              NULL, 13, 768);
    TEST_ASSERT(no_irq_write >= 0, "read-only IRQ bind task created");
    cap_id_t ep_rw = KERN_INVALID_ID;
    cap_id_t irq_ro = KERN_INVALID_ID;
    tcb_t *task = task_get_tcb(no_irq_write);
    if (task != NULL) {
        ep_rw = cap_create_for(task, endpoint_obj_for_cap(ep), CAP_OBJ_ENDPOINT, CAP_READ | CAP_WRITE);
        irq_ro = cap_copy_to(NULL, irq_cap, task,
                             CAP_READ | CAP_TRANSFER);
    }
    TEST_ASSERT(ep_rw >= 0, "read-only IRQ bind task receives endpoint cap");
    TEST_ASSERT(irq_ro >= 0, "read-only IRQ bind task receives IRQ cap");
    if (no_irq_write >= 0 && ep_rw >= 0 && irq_ro >= 0) {
        err = task_start(no_irq_write);
        TEST_ASSERT_EQ(KERN_OK, err, "read-only IRQ bind task started");
    }

    void *retval = NULL;
    if (no_irq_write >= 0) {
        err = task_join(no_irq_write, &retval, 1000);
        TEST_ASSERT_EQ(KERN_OK, err, "read-only IRQ bind task joined");
        TEST_ASSERT_EQ(KERN_ERR_CAP, (kern_err_t)(intptr_t)retval,
                       "read-only IRQ cap bind rejected");
    }

    task_id_t no_ep_write = task_create_user("irq_bind_ro_ep",
                                             irq_user_bind_once_task,
                                             NULL, 13, 768);
    TEST_ASSERT(no_ep_write >= 0, "read-only endpoint bind task created");
    cap_id_t ep_ro = KERN_INVALID_ID;
    cap_id_t irq_rw = KERN_INVALID_ID;
    task = task_get_tcb(no_ep_write);
    if (task != NULL) {
        ep_ro = cap_create_for(task, endpoint_obj_for_cap(ep), CAP_OBJ_ENDPOINT, CAP_READ);
        irq_rw = cap_copy_to(NULL, irq_cap, task,
                             CAP_READ | CAP_WRITE | CAP_TRANSFER);
    }
    TEST_ASSERT(ep_ro >= 0, "read-only endpoint bind task receives endpoint cap");
    TEST_ASSERT(irq_rw >= 0, "read-only endpoint bind task receives IRQ cap");
    if (no_ep_write >= 0 && ep_ro >= 0 && irq_rw >= 0) {
        err = task_start(no_ep_write);
        TEST_ASSERT_EQ(KERN_OK, err, "read-only endpoint bind task started");
    }

    retval = NULL;
    if (no_ep_write >= 0) {
        err = task_join(no_ep_write, &retval, 1000);
        TEST_ASSERT_EQ(KERN_OK, err, "read-only endpoint bind task joined");
        TEST_ASSERT_EQ(KERN_ERR_CAP, (kern_err_t)(intptr_t)retval,
                       "read-only endpoint cap bind rejected");
    }

    if (no_irq_write >= 0 &&
        task_get_state(no_irq_write) != TASK_STATE_TERMINATED) {
        (void)task_delete(no_irq_write);
    }
    if (no_ep_write >= 0 &&
        task_get_state(no_ep_write) != TASK_STATE_TERMINATED) {
        (void)task_delete(no_ep_write);
    }
    if (irq_cap > 0) {
        TEST_ASSERT_EQ(KERN_OK, kirq_delete_cap(irq_cap),
                       "IRQ bind-rights cap deleted");
    }
    endpoint_delete(ep);
    TEST_ASSERT_EQ((int)cap_free_before, (int)cap_free_count(),
                   "IRQ bind-rights cleanup restored cap count");
#else
    test_skip("capability, endpoint, or syscall disabled");
#endif
}

static void test_irq_user_module(void) {
    test_irq_user_cap_endpoint_bind();
    test_irq_user_cap_endpoint_bind_rights();
}

TEST_ABI_MODULE(irq_user, test_irq_user_module);
#endif /* TEST_ENABLE */
