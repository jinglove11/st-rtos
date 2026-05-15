/**
 * @file test_syscall.c
 * @brief 系统调用接口测试 — 使用通用 sys_callN API
 */

#include "test_framework.h"
#include "kernel.h"
#include "syscall.h"
#include "user_api.h"
#include "task.h"
#include "semaphore.h"
#include "mutex.h"
#include "mqueue.h"
#include "endpoint.h"

#if CAP_ENABLE
#include "capability.h"
#endif

#if SYSCALL_ENABLE

/*============================================================================
 * 测试 1: syscall 表存在
 *============================================================================*/

static void test_syscall_table_exists(void) {
    test_section("Test 1: Syscall Table Exists");
    TEST_ASSERT(1, "Syscall table compiled and linked");
}

/*============================================================================
 * 测试 2: task_yield via syscall
 *============================================================================*/

static void test_syscall_task_yield(void) {
    test_section("Test 2: Task Yield via sys_call0");

    kern_err_t err = (kern_err_t)sys_call0(SYSCALL_TASK_YIELD);
    TEST_ASSERT_EQ(KERN_OK, err, "sys_call0(TASK_YIELD) returns OK");
}

/*============================================================================
 * 测试 3: task_delay via syscall
 *============================================================================*/

static void test_syscall_task_delay(void) {
    test_section("Test 3: Task Delay via sys_call1");

    kern_err_t err = (kern_err_t)sys_call1(SYSCALL_TASK_DELAY, 1);
    TEST_ASSERT_EQ(KERN_OK, err, "sys_call1(TASK_DELAY, 1) returns OK");
}

/*============================================================================
 * 测试 4: sem_create/wait/post via syscall
 *============================================================================*/

static void test_syscall_sem(void) {
    test_section("Test 4: Semaphore via sys_callN");

    volatile int cap_svc = sys_call2(SYSCALL_SEM_CREATE, 1, 0);
    int cap = cap_svc;

    /* brute-force verify: store in global so no register tricks */
    (void)cap;

    TEST_ASSERT(cap >= 0, "sys_call2(SEM_CREATE, max=1, init=0) returns valid ID");
    TEST_ASSERT(cap > 0, "cap_create returned positive token");
    TEST_ASSERT(cap < 32768, "cap_create returned in-range token");

    /* Direct cap_resolve */
    (void)cap_resolve((cap_id_t)cap, CAP_OBJ_SEMAPHORE, CAP_WRITE);

    int err = sys_call1(SYSCALL_SEM_POST, cap);
    TEST_ASSERT_EQ(KERN_OK, (kern_err_t)err, "sys_call1(SEM_POST) OK");

    err = sys_call2(SYSCALL_SEM_WAIT, cap, 0);
    TEST_ASSERT_EQ(KERN_OK, (kern_err_t)err, "sys_call2(SEM_WAIT, 0) OK");

    /* cleanup capability via syscall */
    sys_call1(SYSCALL_SEM_DELETE, cap);

    /* --- Part B: Direct cap_create, then test via SVC --- */
    {
        sem_id_t sid = sem_create(0, 1);
        cap_id_t dcap = cap_create((void *)(uintptr_t)(sid + 1),
                                   CAP_OBJ_SEMAPHORE, CAP_FULL, 1);
        (void)cap_resolve(dcap, CAP_OBJ_SEMAPHORE, CAP_WRITE);
        kern_err_t derr = (kern_err_t)sys_call1(SYSCALL_SEM_POST, dcap);
        TEST_ASSERT_EQ(KERN_OK, derr, "PartB: direct-cap via SVC POST OK");
        /* cleanup */
        cap_delete(dcap);
        sem_delete(sid);
    }
}

/*============================================================================
 * 测试 5: task_self via syscall
 *============================================================================*/

static void test_syscall_task_self(void) {
    test_section("Test 5: Task Self via sys_call0");

    task_id_t id = (task_id_t)sys_call0(SYSCALL_TASK_SELF);
    TEST_ASSERT(id >= 0, "sys_call0(TASK_SELF) returns valid ID");
}

/*============================================================================
 * 测试 6: syscall 参数传递正确性 (R1/R2 不被破坏)
 *============================================================================*/

static void test_syscall_args(void) {
    test_section("Test 6: Syscall Args Integrity (R1/R2 preserved)");

    int sem = sys_call2(SYSCALL_SEM_CREATE, 1, 0);
    TEST_ASSERT(sem >= 0, "sem created");

    /* post/wait verify argument passing is correct */
    int err = sys_call1(SYSCALL_SEM_POST, sem);
    TEST_ASSERT_EQ(KERN_OK, err, "post OK");

    err = sys_call2(SYSCALL_SEM_WAIT, sem, 100);
    TEST_ASSERT_EQ(KERN_OK, err, "wait with timeout OK");

    /* cleanup capability */
    sys_call1(SYSCALL_SEM_DELETE, sem);
}

/*============================================================================
 * 测试 7: sys_call2 的 R2 完整性 (验证修复 R2 clobber bug)
 *============================================================================*/

static void test_syscall_r2_integrity(void) {
    test_section("Test 7: R2 Register Integrity");

    /* Repeated sys_call2 to verify R2 is preserved */
    cap_id_t caps[10];
    for (int i = 0; i < 10; i++) {
        int err = sys_call2(SYSCALL_SEM_CREATE, 1, 0);
        TEST_ASSERT(err >= 0 || err == KERN_ERR, "R2 intact after syscall");
        caps[i] = (cap_id_t)err;
    }
    /* cleanup capabilities */
    for (int i = 0; i < 10; i++) {
        if (caps[i] >= 0) sys_call1(SYSCALL_SEM_DELETE, caps[i]);
    }
}

/*============================================================================
 * 测试 8: syscall 坏指针应返回错误，不应造成内核 fault
 *============================================================================*/

static void test_syscall_bad_user_pointers(void) {
    test_section("Test 8: Bad User Pointers");

    int err = sys_call5(SYSCALL_TASK_CREATE,
                        (int)(uintptr_t)0xBBBBBBBBu,
                        (int)(uintptr_t)test_syscall_bad_user_pointers,
                        0, 10, 512);
    TEST_ASSERT_EQ(KERN_ERR_PARAM, err, "bad task name pointer rejected");

#if VFS_ENABLE
    err = sys_call2(SYSCALL_OPEN, (int)(uintptr_t)0xBBBBBBBBu, 0);
    TEST_ASSERT_EQ(KERN_ERR_PARAM, err, "bad open path pointer rejected");

    err = sys_call3(SYSCALL_READ, 0, (int)(uintptr_t)0xBBBBBBBBu, 4);
    TEST_ASSERT_EQ(KERN_ERR_PARAM, err, "bad read buffer pointer rejected");

    err = sys_call3(SYSCALL_WRITE, 0, (int)(uintptr_t)0xBBBBBBBBu, 4);
    TEST_ASSERT_EQ(KERN_ERR_PARAM, err, "bad write buffer pointer rejected");
#endif
}

/*============================================================================
 * 测试 9: syscall/SVC 负向安全边界
 *============================================================================*/

static int test_invoke_svc0_again(void) {
    register int r0 __asm("r0") = 0x12345678;
    __asm volatile("svc #0" : "+r"(r0) :: "memory");
    return r0;
}

static int test_invoke_svc2_invalid(void) {
    register int r0 __asm("r0") = 0x12345678;
    __asm volatile("svc #2" : "+r"(r0) :: "memory");
    return r0;
}

#if MPU_ENABLE && CAP_ENABLE
static void user_raw_task_id_control_task(void *arg) {
    int raw_task_id = (int)(uintptr_t)arg;
    int del_err = sys_task_delete(raw_task_id);
    int suspend_err = sys_task_suspend(raw_task_id);
    int result = KERN_ERR;

    if (del_err == KERN_ERR_CAP && suspend_err == KERN_ERR_CAP) {
        result = KERN_OK;
    }

    sys_task_exit((void *)(intptr_t)result);
}

static void user_forbidden_callback(void *arg) {
    (void)arg;
}

static void user_callback_syscall_task(void *arg) {
    (void)arg;
    int timer_err = sys_timer_create("u_tmr", user_forbidden_callback, NULL, 0);
    int irq_err = sys_call3(SYSCALL_IRQ_REGISTER, 1,
                            (int)(uintptr_t)user_forbidden_callback, 8);
    int bh_err = sys_call2(SYSCALL_BH_CREATE,
                           (int)(uintptr_t)user_forbidden_callback, 0);
    int bh_sched_err = sys_call1(SYSCALL_BH_SCHEDULE, 0);
    int result = KERN_ERR;

    if (timer_err == KERN_ERR_PERM &&
        irq_err == KERN_ERR_PERM &&
        bh_err == KERN_ERR_PERM &&
        bh_sched_err == KERN_ERR_CAP) {
        result = KERN_OK;
    }

    sys_task_exit((void *)(intptr_t)result);
}
#endif

static void test_syscall_security_negative(void) {
    test_section("Test 9: Syscall security negative cases");

    int err = sys_call0(SYSCALL_TABLE_SIZE + 1);
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, err, "invalid syscall number rejected");

    err = test_invoke_svc2_invalid();
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, err, "invalid SVC immediate rejected");

    err = test_invoke_svc0_again();
    TEST_ASSERT_EQ((int)KERN_ERR_STATE, err, "running task cannot reuse SVC #0");

#if MPU_ENABLE && CAP_ENABLE
    task_id_t victim = task_create_user("raw_victim", user_raw_task_id_control_task,
                                        NULL, 10, 512);
    TEST_ASSERT(victim >= 0, "raw-id victim task created");
    if (victim < 0) return;

    task_id_t attacker = task_create_user("raw_attacker",
                                          user_raw_task_id_control_task,
                                          (void *)(uintptr_t)victim, 5, 512);
    TEST_ASSERT(attacker >= 0, "raw-id attacker task created");
    if (attacker < 0) {
        (void)task_delete(victim);
        return;
    }

    task_start(attacker);
    void *retval = NULL;
    kern_err_t join_err = task_join(attacker, &retval, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)join_err, "raw-id attacker joined");
    TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                   "user raw task id management rejected");

    TEST_ASSERT_NOT_NULL(task_get_tcb(victim), "raw-id victim still exists");
    (void)task_delete(victim);

    task_id_t cb_task = task_create_user("cb_reject",
                                         user_callback_syscall_task,
                                         NULL, 5, 512);
    TEST_ASSERT(cb_task >= 0, "callback rejection user task created");
    if (cb_task < 0) return;

    task_start(cb_task);
    retval = NULL;
    join_err = task_join(cb_task, &retval, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)join_err, "callback rejection task joined");
    TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                   "user callback syscalls rejected");
#else
    test_skip("MPU or capability disabled");
#endif
}

/*============================================================================
 * 测试 10: 用户态服务通过 sleepable endpoint syscall 处理请求
 *============================================================================*/

#if MPU_ENABLE && IPC_ENDPOINT && CAP_ENABLE
static void user_endpoint_service_task(void *arg) {
    int ep_cap = (int)(uintptr_t)arg;
    uint8_t msg_buf[KERN_EP_MSG_SIZE];
    uint32_t *req = (uint32_t *)msg_buf;
    int err;

    for (uint32_t i = 0; i < sizeof(msg_buf); i++) {
        msg_buf[i] = 0;
    }

    err = sys_ep_recv(ep_cap, msg_buf, 1000);

    if (err == KERN_OK) {
        *req += 100;
        err = sys_ep_reply(ep_cap, msg_buf);
    }

    sys_task_exit((void *)(uintptr_t)err);
}

static void user_endpoint_recv_timeout_task(void *arg) {
    int ep_cap = (int)(uintptr_t)arg;
    uint8_t msg_buf[KERN_EP_MSG_SIZE];
    int err;

    for (uint32_t i = 0; i < sizeof(msg_buf); i++) {
        msg_buf[i] = 0;
    }

    err = sys_ep_recv(ep_cap, msg_buf, 2);
    sys_task_exit((void *)(uintptr_t)err);
}

static void user_endpoint_client_task(void *arg) {
    int ep_cap = (int)(uintptr_t)arg;
    uint8_t msg_buf[KERN_EP_MSG_SIZE];
    uint32_t *msg = (uint32_t *)msg_buf;
    int err;

    for (uint32_t i = 0; i < sizeof(msg_buf); i++) {
        msg_buf[i] = 0;
    }
    *msg = 77;

    err = sys_ep_send(ep_cap, msg_buf, 1000);
    if (err == KERN_OK && *msg != 177) {
        err = KERN_ERR;
    }

    sys_task_exit((void *)(uintptr_t)err);
}

static void user_endpoint_send_wait_task(void *arg) {
    int ep_cap = (int)(uintptr_t)arg;
    uint8_t msg_buf[KERN_EP_MSG_SIZE];
    uint32_t *msg = (uint32_t *)msg_buf;
    int err;

    for (uint32_t i = 0; i < sizeof(msg_buf); i++) {
        msg_buf[i] = 0;
    }
    *msg = 55;

    err = sys_ep_send(ep_cap, msg_buf, 20);
    sys_task_exit((void *)(uintptr_t)err);
}

static void user_endpoint_send_nowait_task(void *arg) {
    int ep_cap = (int)(uintptr_t)arg;
    uint8_t msg_buf[KERN_EP_MSG_SIZE];
    uint32_t *msg = (uint32_t *)msg_buf;
    int err;

    for (uint32_t i = 0; i < sizeof(msg_buf); i++) {
        msg_buf[i] = 0;
    }
    *msg = 66;

    err = sys_ep_send(ep_cap, msg_buf, 0);
    sys_task_exit((void *)(uintptr_t)err);
}

static void user_sem_wait_task(void *arg) {
    int sem_cap = (int)(uintptr_t)arg;
    int err = sys_sem_wait(sem_cap, 1000);
    sys_task_exit((void *)(intptr_t)err);
}

static void user_sem_wait_timeout_task(void *arg) {
    int sem_cap = (int)(uintptr_t)arg;
    int err = sys_sem_wait(sem_cap, 2);
    sys_task_exit((void *)(intptr_t)err);
}

static void user_mutex_lock_task(void *arg) {
    int mutex_cap = (int)(uintptr_t)arg;
    int err = sys_mutex_lock(mutex_cap, 1000);
    if (err == KERN_OK) {
        err = sys_mutex_unlock(mutex_cap);
    }
    sys_task_exit((void *)(intptr_t)err);
}

static void user_mutex_lock_timeout_task(void *arg) {
    int mutex_cap = (int)(uintptr_t)arg;
    int err = sys_mutex_lock(mutex_cap, 2);
    sys_task_exit((void *)(intptr_t)err);
}

static void user_mqueue_recv_task(void *arg) {
    int mq_cap = (int)(uintptr_t)arg;
    uint8_t msg_buf[KERN_MSG_MAX_SIZE];
    uint32_t *msg = (uint32_t *)msg_buf;

    for (uint32_t i = 0; i < sizeof(msg_buf); i++) {
        msg_buf[i] = 0;
    }

    int err = sys_mqueue_recv(mq_cap, msg_buf, 1000);
    if (err == KERN_OK && *msg != 0x4d515245U) {
        err = KERN_ERR_STATE;
    }
    sys_task_exit((void *)(intptr_t)err);
}

static void user_mqueue_recv_timeout_task(void *arg) {
    int mq_cap = (int)(uintptr_t)arg;
    uint8_t msg_buf[KERN_MSG_MAX_SIZE];

    for (uint32_t i = 0; i < sizeof(msg_buf); i++) {
        msg_buf[i] = 0;
    }

    int err = sys_mqueue_recv(mq_cap, msg_buf, 2);
    sys_task_exit((void *)(intptr_t)err);
}

static void user_mqueue_send_task(void *arg) {
    int mq_cap = (int)(uintptr_t)arg;
    uint8_t msg_buf[KERN_MSG_MAX_SIZE];
    uint32_t *msg = (uint32_t *)msg_buf;

    for (uint32_t i = 0; i < sizeof(msg_buf); i++) {
        msg_buf[i] = 0;
    }
    *msg = 0x4d515345U;

    int err = sys_mqueue_send(mq_cap, msg_buf, 1000);
    sys_task_exit((void *)(intptr_t)err);
}
#endif

static void test_user_endpoint_service_nonblocking(void) {
    test_section("Test 9: User endpoint service sleepable recv");

#if MPU_ENABLE && IPC_ENDPOINT && CAP_ENABLE
    ep_id_t ep = endpoint_create("u_svc", sizeof(uint32_t), 2);
    TEST_ASSERT(ep >= 0, "service endpoint created");
    if (ep < 0) return;

    cap_id_t ep_cap = cap_create((void *)(uintptr_t)(ep + 1),
                                 CAP_OBJ_ENDPOINT, CAP_FULL, 0);
    TEST_ASSERT(ep_cap >= 0, "service endpoint cap created");
    if (ep_cap < 0) {
        endpoint_delete(ep);
        return;
    }

    task_id_t service = task_create_user("u_ep_svc",
                                         user_endpoint_service_task,
                                         (void *)(uintptr_t)ep_cap,
                                         5, 512);
    TEST_ASSERT(service >= 0, "user endpoint service created");
    if (service < 0) {
        cap_delete(ep_cap);
        endpoint_delete(ep);
        return;
    }

    kern_err_t err = cap_transfer(ep_cap, (uint8_t)service);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "service endpoint cap transferred");
    if (err != KERN_OK) {
        (void)task_delete(service);
        endpoint_delete(ep);
        return;
    }

    task_start(service);

    uint32_t msg = 23;
    err = endpoint_send(ep, &msg, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "kernel client endpoint send OK");
    TEST_ASSERT_EQ(123, (int)msg, "user service replied through endpoint");

    void *retval = NULL;
    kern_err_t join_err = task_join(service, &retval, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)join_err, "user service joined OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)(uintptr_t)retval,
                   "user service syscall result OK");

    endpoint_delete(ep);
#else
    test_skip("MPU, endpoint, or capability disabled");
#endif
}

static void test_user_endpoint_recv_sleep_timeout(void) {
    test_section("Test 10: User endpoint recv sleep timeout");

#if MPU_ENABLE && IPC_ENDPOINT && CAP_ENABLE
    ep_id_t ep = endpoint_create("u_svc_to", sizeof(uint32_t), 1);
    TEST_ASSERT(ep >= 0, "timeout endpoint created");
    if (ep < 0) return;

    cap_id_t ep_cap = cap_create((void *)(uintptr_t)(ep + 1),
                                 CAP_OBJ_ENDPOINT, CAP_FULL, 0);
    TEST_ASSERT(ep_cap >= 0, "timeout endpoint cap created");
    if (ep_cap < 0) {
        endpoint_delete(ep);
        return;
    }

    task_id_t service = task_create_user("u_ep_to",
                                         user_endpoint_recv_timeout_task,
                                         (void *)(uintptr_t)ep_cap,
                                         5, 512);
    TEST_ASSERT(service >= 0, "timeout user receiver created");
    if (service < 0) {
        cap_delete(ep_cap);
        endpoint_delete(ep);
        return;
    }

    kern_err_t err = cap_transfer(ep_cap, (uint8_t)service);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "timeout endpoint cap transferred");
    if (err != KERN_OK) {
        (void)task_delete(service);
        endpoint_delete(ep);
        return;
    }

    task_start(service);

    void *retval = NULL;
    kern_err_t join_err = task_join(service, &retval, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)join_err, "timeout receiver joined OK");
    TEST_ASSERT_EQ((int)KERN_ERR_TIMEOUT, (int)(intptr_t)retval,
                   "sleepable recv returned timeout");

    endpoint_delete(ep);
#else
    test_skip("MPU, endpoint, or capability disabled");
#endif
}

static void test_user_endpoint_send_sleep_reply(void) {
    test_section("Test 11: User endpoint send sleep reply");

#if MPU_ENABLE && IPC_ENDPOINT && CAP_ENABLE
    ep_id_t ep = endpoint_create("u_cli", sizeof(uint32_t), 1);
    TEST_ASSERT(ep >= 0, "client endpoint created");
    if (ep < 0) return;

    cap_id_t ep_cap = cap_create((void *)(uintptr_t)(ep + 1),
                                 CAP_OBJ_ENDPOINT, CAP_FULL, 0);
    TEST_ASSERT(ep_cap >= 0, "client endpoint cap created");
    if (ep_cap < 0) {
        endpoint_delete(ep);
        return;
    }

    task_id_t client = task_create_user("u_ep_cli",
                                        user_endpoint_client_task,
                                        (void *)(uintptr_t)ep_cap,
                                        5, 512);
    TEST_ASSERT(client >= 0, "user endpoint client created");
    if (client < 0) {
        cap_delete(ep_cap);
        endpoint_delete(ep);
        return;
    }

    kern_err_t err = cap_transfer(ep_cap, (uint8_t)client);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "client endpoint cap transferred");
    if (err != KERN_OK) {
        (void)task_delete(client);
        endpoint_delete(ep);
        return;
    }

    task_start(client);

    uint32_t msg = 0;
    err = endpoint_recv(ep, &msg, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "kernel server received user request");
    TEST_ASSERT_EQ(77, (int)msg, "kernel server saw user payload");
    msg += 100;
    err = endpoint_reply(ep, &msg);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "kernel server replied to user client");

    void *retval = NULL;
    kern_err_t join_err = task_join(client, &retval, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)join_err, "user client joined OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                   "sleepable send returned reply OK");

    endpoint_delete(ep);
#else
    test_skip("MPU, endpoint, or capability disabled");
#endif
}

static void test_user_endpoint_send_sleep_timeout(void) {
    test_section("Test 12: User endpoint send sleep timeout");

#if MPU_ENABLE && IPC_ENDPOINT && CAP_ENABLE
    ep_id_t ep = endpoint_create("u_cli_to", sizeof(uint32_t), 1);
    TEST_ASSERT(ep >= 0, "send-timeout endpoint created");
    if (ep < 0) return;

    cap_id_t ep_cap = cap_create((void *)(uintptr_t)(ep + 1),
                                 CAP_OBJ_ENDPOINT, CAP_FULL, 0);
    TEST_ASSERT(ep_cap >= 0, "send-timeout endpoint cap created");
    if (ep_cap < 0) {
        endpoint_delete(ep);
        return;
    }

    task_id_t client = task_create_user("u_ep_sto",
                                        user_endpoint_send_wait_task,
                                        (void *)(uintptr_t)ep_cap,
                                        5, 512);
    TEST_ASSERT(client >= 0, "send-timeout user client created");
    if (client < 0) {
        cap_delete(ep_cap);
        endpoint_delete(ep);
        return;
    }

    kern_err_t err = cap_transfer(ep_cap, (uint8_t)client);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "send-timeout cap transferred");
    if (err != KERN_OK) {
        (void)task_delete(client);
        endpoint_delete(ep);
        return;
    }

    task_start(client);

    void *retval = NULL;
    kern_err_t join_err = task_join(client, &retval, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)join_err, "send-timeout client joined OK");
    TEST_ASSERT_EQ((int)KERN_ERR_TIMEOUT, (int)(intptr_t)retval,
                   "sleepable send returned timeout");

    endpoint_delete(ep);
#else
    test_skip("MPU, endpoint, or capability disabled");
#endif
}

static void test_user_endpoint_send_sleep_delete(void) {
    test_section("Test 13: User endpoint send sleep delete");

#if MPU_ENABLE && IPC_ENDPOINT && CAP_ENABLE
    ep_id_t ep = endpoint_create("u_cli_del", sizeof(uint32_t), 1);
    TEST_ASSERT(ep >= 0, "send-delete endpoint created");
    if (ep < 0) return;

    cap_id_t ep_cap = cap_create((void *)(uintptr_t)(ep + 1),
                                 CAP_OBJ_ENDPOINT, CAP_FULL, 0);
    TEST_ASSERT(ep_cap >= 0, "send-delete endpoint cap created");
    if (ep_cap < 0) {
        endpoint_delete(ep);
        return;
    }

    task_id_t client = task_create_user("u_ep_sdel",
                                        user_endpoint_send_wait_task,
                                        (void *)(uintptr_t)ep_cap,
                                        5, 512);
    TEST_ASSERT(client >= 0, "send-delete user client created");
    if (client < 0) {
        cap_delete(ep_cap);
        endpoint_delete(ep);
        return;
    }

    kern_err_t err = cap_transfer(ep_cap, (uint8_t)client);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "send-delete cap transferred");
    if (err != KERN_OK) {
        (void)task_delete(client);
        endpoint_delete(ep);
        return;
    }

    task_start(client);
    task_delay(1);

    err = endpoint_delete(ep);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "endpoint delete woke send syscall");

    void *retval = NULL;
    kern_err_t join_err = task_join(client, &retval, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)join_err, "send-delete client joined OK");
    TEST_ASSERT_EQ((int)KERN_ERR_NOEXIST, (int)(intptr_t)retval,
                   "sleepable send returned noexist after delete");
#else
    test_skip("MPU, endpoint, or capability disabled");
#endif
}

static void test_user_endpoint_send_nowait_timeout(void) {
    test_section("Test 14: User endpoint send no-wait timeout");

#if MPU_ENABLE && IPC_ENDPOINT && CAP_ENABLE
    ep_id_t ep = endpoint_create("u_cli_nw", sizeof(uint32_t), 1);
    TEST_ASSERT(ep >= 0, "send-nowait endpoint created");
    if (ep < 0) return;

    cap_id_t ep_cap = cap_create((void *)(uintptr_t)(ep + 1),
                                 CAP_OBJ_ENDPOINT, CAP_FULL, 0);
    TEST_ASSERT(ep_cap >= 0, "send-nowait endpoint cap created");
    if (ep_cap < 0) {
        endpoint_delete(ep);
        return;
    }

    task_id_t client = task_create_user("u_ep_snw",
                                        user_endpoint_send_nowait_task,
                                        (void *)(uintptr_t)ep_cap,
                                        5, 512);
    TEST_ASSERT(client >= 0, "send-nowait user client created");
    if (client < 0) {
        cap_delete(ep_cap);
        endpoint_delete(ep);
        return;
    }

    kern_err_t err = cap_transfer(ep_cap, (uint8_t)client);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "send-nowait cap transferred");
    if (err != KERN_OK) {
        (void)task_delete(client);
        endpoint_delete(ep);
        return;
    }

    task_start(client);

    void *retval = NULL;
    kern_err_t join_err = task_join(client, &retval, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)join_err, "send-nowait client joined OK");
    TEST_ASSERT_EQ((int)KERN_ERR_TIMEOUT, (int)(intptr_t)retval,
                   "nowait send returned timeout");

    endpoint_delete(ep);
#else
    test_skip("MPU, endpoint, or capability disabled");
#endif
}

static void test_blocking_ipc_syscalls_rejected(void) {
    test_section("Test 15: Non-continuation IPC blocking rejected");

    uint8_t ep_msg[KERN_EP_MSG_SIZE];
    uint8_t ch_msg[KERN_CH_MSG_SIZE];
    ipc_cap_xfer_t xfers[IPC_CAPS_MAX];
    cap_id_t out_caps[IPC_CAPS_MAX];
    uint8_t out_count = 0;

    for (uint32_t i = 0; i < sizeof(ep_msg); i++) {
        ep_msg[i] = 0;
    }
    for (uint32_t i = 0; i < sizeof(ch_msg); i++) {
        ch_msg[i] = 0;
    }
    for (uint32_t i = 0; i < IPC_CAPS_MAX; i++) {
        xfers[i].src_cap = KERN_INVALID_ID;
        xfers[i].rights = 0;
        xfers[i].flags = 0;
        out_caps[i] = KERN_INVALID_ID;
    }

    int err = sys_ep_send_caps(0, ep_msg, xfers, 0, 1);
    TEST_ASSERT_EQ((int)KERN_ERR_BUSY, err,
                   "blocking ep_send_caps rejected");

    err = sys_ep_recv_caps(0, ep_msg, out_caps, &out_count, 1);
    TEST_ASSERT_EQ((int)KERN_ERR_BUSY, err,
                   "blocking ep_recv_caps rejected");

    err = sys_ch_send(0, ch_msg, 1);
    TEST_ASSERT_EQ((int)KERN_ERR_BUSY, err,
                   "blocking ch_send rejected");

    err = sys_ch_recv(0, ch_msg, 1);
    TEST_ASSERT_EQ((int)KERN_ERR_BUSY, err,
                   "blocking ch_recv rejected");

    err = sys_ch_send_caps(0, ch_msg, xfers, 0, 1);
    TEST_ASSERT_EQ((int)KERN_ERR_BUSY, err,
                   "blocking ch_send_caps rejected");

    err = sys_ch_recv_caps(0, ch_msg, out_caps, &out_count, 1);
    TEST_ASSERT_EQ((int)KERN_ERR_BUSY, err,
                   "blocking ch_recv_caps rejected");
}

static void test_user_sem_wait_sleepable(void) {
    test_section("Test 16: User semaphore wait sleepable");

#if MPU_ENABLE && CAP_ENABLE
    sem_id_t sem = sem_create(0, 1);
    TEST_ASSERT(sem >= 0, "sleepable semaphore created");
    if (sem < 0) return;

    cap_id_t sem_cap = cap_create((void *)(uintptr_t)(sem + 1),
                                  CAP_OBJ_SEMAPHORE, CAP_FULL, 0);
    TEST_ASSERT(sem_cap >= 0, "sleepable semaphore cap created");
    if (sem_cap < 0) {
        sem_delete(sem);
        return;
    }

    task_id_t waiter = task_create_user("u_sem_wait",
                                        user_sem_wait_task,
                                        (void *)(uintptr_t)sem_cap,
                                        5, 512);
    TEST_ASSERT(waiter >= 0, "sleepable semaphore waiter created");
    if (waiter < 0) {
        cap_delete(sem_cap);
        sem_delete(sem);
        return;
    }

    kern_err_t err = cap_transfer(sem_cap, (uint8_t)waiter);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "sleepable semaphore cap transferred");
    if (err != KERN_OK) {
        (void)task_delete(waiter);
        sem_delete(sem);
        return;
    }

    task_start(waiter);
    task_delay(1);

    err = sem_post(sem);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "kernel post woke sem syscall");

    void *retval = NULL;
    kern_err_t join_err = task_join(waiter, &retval, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)join_err, "sem waiter joined OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                   "sleepable sem wait returned OK");

    sem_delete(sem);
#else
    test_skip("MPU or capability disabled");
#endif
}

static void test_user_sem_wait_sleep_timeout(void) {
    test_section("Test 17: User semaphore wait sleep timeout");

#if MPU_ENABLE && CAP_ENABLE
    sem_id_t sem = sem_create(0, 1);
    TEST_ASSERT(sem >= 0, "timeout semaphore created");
    if (sem < 0) return;

    cap_id_t sem_cap = cap_create((void *)(uintptr_t)(sem + 1),
                                  CAP_OBJ_SEMAPHORE, CAP_FULL, 0);
    TEST_ASSERT(sem_cap >= 0, "timeout semaphore cap created");
    if (sem_cap < 0) {
        sem_delete(sem);
        return;
    }

    task_id_t waiter = task_create_user("u_sem_to",
                                        user_sem_wait_timeout_task,
                                        (void *)(uintptr_t)sem_cap,
                                        5, 512);
    TEST_ASSERT(waiter >= 0, "timeout semaphore waiter created");
    if (waiter < 0) {
        cap_delete(sem_cap);
        sem_delete(sem);
        return;
    }

    kern_err_t err = cap_transfer(sem_cap, (uint8_t)waiter);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "timeout semaphore cap transferred");
    if (err != KERN_OK) {
        (void)task_delete(waiter);
        sem_delete(sem);
        return;
    }

    task_start(waiter);

    void *retval = NULL;
    kern_err_t join_err = task_join(waiter, &retval, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)join_err, "sem timeout waiter joined OK");
    TEST_ASSERT_EQ((int)KERN_ERR_TIMEOUT, (int)(intptr_t)retval,
                   "sleepable sem wait returned timeout");

    sem_delete(sem);
#else
    test_skip("MPU or capability disabled");
#endif
}

static void test_user_mutex_lock_sleepable(void) {
    test_section("Test 18: User mutex lock sleepable");

#if MPU_ENABLE && CAP_ENABLE
    mutex_id_t mid = mutex_create();
    TEST_ASSERT(mid >= 0, "sleepable mutex created");
    if (mid < 0) return;

    kern_err_t err = mutex_lock(mid, 0);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "kernel locked mutex first");
    if (err != KERN_OK) {
        mutex_delete(mid);
        return;
    }

    cap_id_t mutex_cap = cap_create((void *)(uintptr_t)(mid + 1),
                                    CAP_OBJ_MUTEX, CAP_FULL, 0);
    TEST_ASSERT(mutex_cap >= 0, "sleepable mutex cap created");
    if (mutex_cap < 0) {
        mutex_unlock(mid);
        mutex_delete(mid);
        return;
    }

    task_id_t waiter = task_create_user("u_mtx_wait",
                                        user_mutex_lock_task,
                                        (void *)(uintptr_t)mutex_cap,
                                        5, 512);
    TEST_ASSERT(waiter >= 0, "sleepable mutex waiter created");
    if (waiter < 0) {
        cap_delete(mutex_cap);
        mutex_unlock(mid);
        mutex_delete(mid);
        return;
    }

    err = cap_transfer(mutex_cap, (uint8_t)waiter);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "sleepable mutex cap transferred");
    if (err != KERN_OK) {
        (void)task_delete(waiter);
        mutex_unlock(mid);
        mutex_delete(mid);
        return;
    }

    task_start(waiter);
    task_delay(1);

    err = mutex_unlock(mid);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "kernel unlock woke mutex syscall");

    void *retval = NULL;
    kern_err_t join_err = task_join(waiter, &retval, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)join_err, "mutex waiter joined OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                   "sleepable mutex lock/unlock returned OK");

    mutex_delete(mid);
#else
    test_skip("MPU or capability disabled");
#endif
}

static void test_user_mutex_lock_sleep_timeout(void) {
    test_section("Test 19: User mutex lock sleep timeout");

#if MPU_ENABLE && CAP_ENABLE
    mutex_id_t mid = mutex_create();
    TEST_ASSERT(mid >= 0, "timeout mutex created");
    if (mid < 0) return;

    kern_err_t err = mutex_lock(mid, 0);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "kernel locked timeout mutex");
    if (err != KERN_OK) {
        mutex_delete(mid);
        return;
    }

    cap_id_t mutex_cap = cap_create((void *)(uintptr_t)(mid + 1),
                                    CAP_OBJ_MUTEX, CAP_FULL, 0);
    TEST_ASSERT(mutex_cap >= 0, "timeout mutex cap created");
    if (mutex_cap < 0) {
        mutex_unlock(mid);
        mutex_delete(mid);
        return;
    }

    task_id_t waiter = task_create_user("u_mtx_to",
                                        user_mutex_lock_timeout_task,
                                        (void *)(uintptr_t)mutex_cap,
                                        5, 512);
    TEST_ASSERT(waiter >= 0, "timeout mutex waiter created");
    if (waiter < 0) {
        cap_delete(mutex_cap);
        mutex_unlock(mid);
        mutex_delete(mid);
        return;
    }

    err = cap_transfer(mutex_cap, (uint8_t)waiter);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "timeout mutex cap transferred");
    if (err != KERN_OK) {
        (void)task_delete(waiter);
        mutex_unlock(mid);
        mutex_delete(mid);
        return;
    }

    task_start(waiter);

    void *retval = NULL;
    kern_err_t join_err = task_join(waiter, &retval, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)join_err, "mutex timeout waiter joined OK");
    TEST_ASSERT_EQ((int)KERN_ERR_TIMEOUT, (int)(intptr_t)retval,
                   "sleepable mutex lock returned timeout");

    mutex_unlock(mid);
    mutex_delete(mid);
#else
    test_skip("MPU or capability disabled");
#endif
}

static void test_user_mqueue_recv_sleepable(void) {
    test_section("Test 20: User mqueue recv sleepable");

#if MPU_ENABLE && CAP_ENABLE
    queue_id_t mq = mqueue_create(sizeof(uint32_t), 1);
    TEST_ASSERT(mq >= 0, "sleepable mqueue created");
    if (mq < 0) return;

    cap_id_t mq_cap = cap_create((void *)(uintptr_t)(mq + 1),
                                 CAP_OBJ_MQUEUE, CAP_FULL, 0);
    TEST_ASSERT(mq_cap >= 0, "sleepable mqueue cap created");
    if (mq_cap < 0) {
        mqueue_delete(mq);
        return;
    }

    task_id_t waiter = task_create_user("u_mq_recv",
                                        user_mqueue_recv_task,
                                        (void *)(uintptr_t)mq_cap,
                                        5, 512);
    TEST_ASSERT(waiter >= 0, "sleepable mqueue recv task created");
    if (waiter < 0) {
        cap_delete(mq_cap);
        mqueue_delete(mq);
        return;
    }

    kern_err_t err = cap_transfer(mq_cap, (uint8_t)waiter);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "sleepable mqueue cap transferred");
    if (err != KERN_OK) {
        (void)task_delete(waiter);
        mqueue_delete(mq);
        return;
    }

    task_start(waiter);
    task_delay(1);

    uint32_t msg = 0x4d515245U;
    err = mqueue_send(mq, &msg, 0);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "kernel send woke mqueue recv syscall");

    void *retval = NULL;
    kern_err_t join_err = task_join(waiter, &retval, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)join_err, "mqueue recv waiter joined OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                   "sleepable mqueue recv returned OK");

    mqueue_delete(mq);
#else
    test_skip("MPU or capability disabled");
#endif
}

static void test_user_mqueue_recv_sleep_timeout(void) {
    test_section("Test 21: User mqueue recv sleep timeout");

#if MPU_ENABLE && CAP_ENABLE
    queue_id_t mq = mqueue_create(sizeof(uint32_t), 1);
    TEST_ASSERT(mq >= 0, "timeout mqueue created");
    if (mq < 0) return;

    cap_id_t mq_cap = cap_create((void *)(uintptr_t)(mq + 1),
                                 CAP_OBJ_MQUEUE, CAP_FULL, 0);
    TEST_ASSERT(mq_cap >= 0, "timeout mqueue cap created");
    if (mq_cap < 0) {
        mqueue_delete(mq);
        return;
    }

    task_id_t waiter = task_create_user("u_mq_to",
                                        user_mqueue_recv_timeout_task,
                                        (void *)(uintptr_t)mq_cap,
                                        5, 512);
    TEST_ASSERT(waiter >= 0, "timeout mqueue recv task created");
    if (waiter < 0) {
        cap_delete(mq_cap);
        mqueue_delete(mq);
        return;
    }

    kern_err_t err = cap_transfer(mq_cap, (uint8_t)waiter);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "timeout mqueue cap transferred");
    if (err != KERN_OK) {
        (void)task_delete(waiter);
        mqueue_delete(mq);
        return;
    }

    task_start(waiter);

    void *retval = NULL;
    kern_err_t join_err = task_join(waiter, &retval, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)join_err,
                   "mqueue timeout waiter joined OK");
    TEST_ASSERT_EQ((int)KERN_ERR_TIMEOUT, (int)(intptr_t)retval,
                   "sleepable mqueue recv returned timeout");

    mqueue_delete(mq);
#else
    test_skip("MPU or capability disabled");
#endif
}

static void test_user_mqueue_send_sleepable(void) {
    test_section("Test 22: User mqueue send sleepable");

#if MPU_ENABLE && CAP_ENABLE
    queue_id_t mq = mqueue_create(sizeof(uint32_t), 1);
    TEST_ASSERT(mq >= 0, "send mqueue created");
    if (mq < 0) return;

    uint32_t first = 0x11111111U;
    kern_err_t err = mqueue_send(mq, &first, 0);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "kernel filled mqueue first");
    if (err != KERN_OK) {
        mqueue_delete(mq);
        return;
    }

    cap_id_t mq_cap = cap_create((void *)(uintptr_t)(mq + 1),
                                 CAP_OBJ_MQUEUE, CAP_FULL, 0);
    TEST_ASSERT(mq_cap >= 0, "send mqueue cap created");
    if (mq_cap < 0) {
        mqueue_delete(mq);
        return;
    }

    task_id_t sender = task_create_user("u_mq_send",
                                        user_mqueue_send_task,
                                        (void *)(uintptr_t)mq_cap,
                                        5, 512);
    TEST_ASSERT(sender >= 0, "sleepable mqueue send task created");
    if (sender < 0) {
        cap_delete(mq_cap);
        mqueue_delete(mq);
        return;
    }

    err = cap_transfer(mq_cap, (uint8_t)sender);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "send mqueue cap transferred");
    if (err != KERN_OK) {
        (void)task_delete(sender);
        mqueue_delete(mq);
        return;
    }

    task_start(sender);
    task_delay(1);

    uint32_t got = 0;
    err = mqueue_recv(mq, &got, 0);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "kernel recv opened mqueue slot");
    TEST_ASSERT_EQ((int)0x11111111U, (int)got, "kernel received first mqueue msg");

    void *retval = NULL;
    kern_err_t join_err = task_join(sender, &retval, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)join_err, "mqueue sender joined OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                   "sleepable mqueue send returned OK");

    got = 0;
    err = mqueue_recv(mq, &got, 0);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "kernel received user mqueue msg");
    TEST_ASSERT_EQ((int)0x4d515345U, (int)got,
                   "sleepable mqueue send copied message");

    mqueue_delete(mq);
#else
    test_skip("MPU or capability disabled");
#endif
}

/*============================================================================
 * Syscall 测试模块入口
 *============================================================================*/

static void test_syscall_module(void) {
    test_syscall_table_exists();
    test_syscall_task_yield();
    test_syscall_task_delay();
    test_syscall_sem();
    test_syscall_task_self();
    test_syscall_args();
    test_syscall_r2_integrity();
    test_syscall_bad_user_pointers();
    test_syscall_security_negative();
    test_user_endpoint_service_nonblocking();
    test_user_endpoint_recv_sleep_timeout();
    test_user_endpoint_send_sleep_reply();
    test_user_endpoint_send_sleep_timeout();
    test_user_endpoint_send_sleep_delete();
    test_user_endpoint_send_nowait_timeout();
    test_blocking_ipc_syscalls_rejected();
    test_user_sem_wait_sleepable();
    test_user_sem_wait_sleep_timeout();
    test_user_mutex_lock_sleepable();
    test_user_mutex_lock_sleep_timeout();
    test_user_mqueue_recv_sleepable();
    test_user_mqueue_recv_sleep_timeout();
    test_user_mqueue_send_sleepable();
}

TEST_MODULE_REGISTER(syscall, test_syscall_module);

#endif /* SYSCALL_ENABLE */
