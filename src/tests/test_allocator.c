/**
 * @file test_allocator.c
 * @brief Phase A — allocator service + shm cap transfer tests
 *
 * 验证 allocator 这个特权服务能为 user 任务创建 shm,并通过 endpoint IPC
 * 把 shm cap 转交给客户端,客户端 sys_shm_map 后能读写同一块内存。
 *
 * 测试链路:
 *   1. 内核态创建 allocator 特权任务 + endpoint
 *   2. 内核态创建 client user 任务 + endpoint
 *   3. client 调 allocator_create_shm 请求 shm
 *   4. allocator sys_shm_create → 推 shm cap 到 client inbox
 *   5. client sys_shm_map → 写入测试数据 → 读回校验
 */

#include "test_framework.h"
#include "kernel.h"
#include "task.h"
#include "endpoint.h"
#include "allocator_proto.h"
#include "user_api.h"

#if TEST_MODULE_ALLOCATOR && CAP_ENABLE && MPU_ENABLE

/*============================================================================
 * M2-Step2b: 双 cap arg 传递机制 (cap_id_t 扩位准备,见 test_driver.c)
 *============================================================================*/
#define ALLOC_TEST_PAIR_MAX 8
typedef struct {
    task_id_t task_id;   /* -1 = free slot */
    cap_id_t  cap_b;
} alloc_test_pair_slot_t;

static alloc_test_pair_slot_t alloc_test_pair_slots[ALLOC_TEST_PAIR_MAX] = {
    [0 ... ALLOC_TEST_PAIR_MAX - 1] = { .task_id = -1, .cap_b = (cap_id_t)-1 },
};

static void alloc_test_set_arg_pair(task_id_t task_id, int cap_a, cap_id_t cap_b) {
    tcb_t *tcb = task_get_tcb(task_id);
    if (tcb != NULL && tcb->sp != NULL) {
        uint32_t *stacked_r0 = (uint32_t *)((uint8_t *)tcb->sp + 32U);
        *stacked_r0 = (uint32_t)cap_a;
    }
    for (int i = 0; i < ALLOC_TEST_PAIR_MAX; i++) {
        if (alloc_test_pair_slots[i].task_id == task_id) {
            alloc_test_pair_slots[i].cap_b = cap_b;
            return;
        }
    }
    for (int i = 0; i < ALLOC_TEST_PAIR_MAX; i++) {
        if (alloc_test_pair_slots[i].task_id < 0) {
            alloc_test_pair_slots[i].task_id = task_id;
            alloc_test_pair_slots[i].cap_b = cap_b;
            return;
        }
    }
}

static void alloc_test_reset_pairs(void) {
    for (int i = 0; i < ALLOC_TEST_PAIR_MAX; i++) {
        alloc_test_pair_slots[i].task_id = -1;
        alloc_test_pair_slots[i].cap_b = (cap_id_t)-1;
    }
}

static cap_id_t alloc_test_get_cap_b(void) {
    task_id_t self = (task_id_t)sys_task_self();
    for (int i = 0; i < ALLOC_TEST_PAIR_MAX; i++) {
        if (alloc_test_pair_slots[i].task_id == self) {
            return alloc_test_pair_slots[i].cap_b;
        }
    }
    return (cap_id_t)-1;
}

/*============================================================================
 * 测试用的 endpoint / cap (通过 r0 传给任务)
 *============================================================================*/

/* allocator 任务:arg = 它自己的 ep_cap */
static void allocator_task(void *arg) {
    int ep_cap = (int)(uintptr_t)arg;
    int err = allocator_service_run(ep_cap, 30);  /* 处理 30 个请求后退出 */
    sys_task_exit((void *)(intptr_t)err);
}

/* client user 任务:arg 编码两个 cap (user 任务不能用全局 .bss,MPU 禁止)。
 * 低 16 位 = allocator ep cap,高 16 位 = 自己的 inbox cap。
 * 结果通过 sys_task_exit retval 返回。 */
static void client_shm_task(void *arg) {
    int alloc_ep_cap = (int)(intptr_t)arg;
    int inbox_ep_cap = (int)alloc_test_get_cap_b();
    int shm_cap = KERN_INVALID_ID;
    int err;

    /* 1. ping allocator */
    err = allocator_ping(alloc_ep_cap, 1000);
    if (err != KERN_OK) {
        sys_task_exit((void *)(intptr_t)err);
    }

    /* 2. 请求创建 shm (64 字节) */
    err = allocator_create_shm(alloc_ep_cap, inbox_ep_cap,
                               64, ALLOC_RIGHTS_RW, &shm_cap, 1000);
    if (err != KERN_OK || shm_cap <= 0) {
        sys_task_exit((void *)(intptr_t)((err != KERN_OK) ? err
                                                          : KERN_ERR_CAP));
    }

    /* 3. map shm (user 任务才允许 sys_shm_map) */
    uint8_t *shm = (uint8_t *)sys_shm_map(shm_cap, CAP_READ | CAP_WRITE);
    if ((intptr_t)shm <= 0) {
        sys_task_exit((void *)(intptr_t)shm);
    }

    /* 4. 写测试数据 */
    for (int i = 0; i < 64; i++) {
        shm[i] = (uint8_t)(0xA0 | (i & 0x0F));
    }

    /* 5. 读回校验 */
    int mismatch = 0;
    for (int i = 0; i < 64; i++) {
        if (shm[i] != (uint8_t)(0xA0 | (i & 0x0F))) {
            mismatch++;
        }
    }

    sys_task_exit((void *)(intptr_t)((mismatch == 0) ? KERN_OK
                                                     : KERN_ERR_FAULT));
}

/*============================================================================
 * Test 1: allocator 创建 shm 并验证客户端可读写
 *============================================================================*/

static void test_allocator_shm_create_and_map(void) {
    test_section("Test 1: allocator creates shm, client maps + R/W");

    /* 创建 allocator endpoint */
    ep_id_t alloc_ep = endpoint_create("alloc_svc", KERN_EP_MSG_SIZE, 4);
    TEST_ASSERT(alloc_ep >= 0, "allocator endpoint created");
    if (alloc_ep < 0) return;

    /* 创建 allocator 特权任务 (task_create,非 user,因需 sys_shm_create) */
    task_id_t alloc_id = task_create("alloc_svc", allocator_task, NULL,
                                     10, 1024);
    TEST_ASSERT(alloc_id >= 0, "allocator task created");
    if (alloc_id < 0) {
        (void)endpoint_delete(alloc_ep);
        return;
    }

    /* 给 allocator mint endpoint cap 并塞 r0 */
    tcb_t *alloc_tcb = task_get_tcb(alloc_id);
    cap_id_t alloc_service_cap = KERN_INVALID_ID;
    if (alloc_tcb != NULL) {
        alloc_service_cap = cap_create_for(alloc_tcb,
                                           (void *)(uintptr_t)(alloc_ep + 1),
                                           CAP_OBJ_ENDPOINT,
                                           CAP_READ | CAP_WRITE);
    }
    TEST_ASSERT(alloc_service_cap >= 0, "allocator got endpoint cap");

    /* 创建 client user 任务 + 它自己的 inbox endpoint。
     * user 任务才能 sys_shm_map (syscall 要求 user)。
     * arg 编码两个 cap,避免用全局变量 (user MPU 禁止 .bss)。 */
    ep_id_t client_ep = endpoint_create("alloc_cli", KERN_EP_MSG_SIZE, 2);
    TEST_ASSERT(client_ep >= 0, "client inbox endpoint created");

    task_id_t client_id = task_create_user("alloc_cli", client_shm_task,
                                           NULL, 11, 1024);
    TEST_ASSERT(client_id >= 0, "client task created");

    tcb_t *client_tcb = task_get_tcb(client_id);
    cap_id_t client_alloc_cap = KERN_INVALID_ID;  /* client 持有的 allocator ep cap */
    cap_id_t client_inbox_cap = KERN_INVALID_ID;  /* client 持有的自己 inbox cap */
    if (client_tcb != NULL) {
        client_alloc_cap = cap_create_for(client_tcb,
                                          (void *)(uintptr_t)(alloc_ep + 1),
                                          CAP_OBJ_ENDPOINT,
                                          CAP_READ | CAP_WRITE);
        /* inbox cap 带 TRANSFER,这样 client 能把它 COPY 给 allocator,
         * allocator 再用它推 shm cap 回来 */
        client_inbox_cap = cap_create_for(client_tcb,
                                          (void *)(uintptr_t)(client_ep + 1),
                                          CAP_OBJ_ENDPOINT,
                                          CAP_READ | CAP_WRITE | CAP_TRANSFER);
    }
    TEST_ASSERT(client_alloc_cap >= 0, "client got allocator ep cap");
    TEST_ASSERT(client_inbox_cap >= 0, "client got own inbox cap");

    /* 启动 allocator */
    if (alloc_tcb != NULL && alloc_tcb->sp != NULL &&
        alloc_service_cap >= 0) {
        uint32_t *stacked_r0 = (uint32_t *)((uint8_t *)alloc_tcb->sp + 32U);
        *stacked_r0 = (uint32_t)alloc_service_cap;
        kern_err_t e = task_start(alloc_id);
        TEST_ASSERT_EQ((int)KERN_OK, (int)e, "allocator started");
    }

    /* 启动 client (alloc_cap 经 R0, inbox_cap 经 pair 表) */
    if (client_tcb != NULL && client_tcb->sp != NULL) {
        alloc_test_set_arg_pair(client_id, (int)client_alloc_cap, client_inbox_cap);
        kern_err_t e = task_start(client_id);
        TEST_ASSERT_EQ((int)KERN_OK, (int)e, "client started");
    }

    /* 等 client 完成 */
    void *retval = NULL;
    kern_err_t e = task_join(client_id, &retval, 2000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)e, "client joined (no fault)");
    TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                   "client shm create+map+R/W OK");

    /* 等 allocator 退出 (它处理 30 个请求后自行退出,或超时) */
    retval = NULL;
    e = task_join(alloc_id, &retval, 2000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)e, "allocator joined");

    /* 清理任务和 endpoint。
     * 注意:allocator 创建的 shm 会有 1 cap + 1 mem 泄漏,因为
     * sys_shm_create 内部用 cap_create_for(NULL,...) 把 owner 设成 0,
     * allocator 退出时 cap_revoke_all 清不掉 (owner≠allocator_id)。
     * 这是内核 sys_shm_create 的已知设计限制,Phase B 评估是否改 owner。
     * allocator 作为常驻服务时泄漏不累积 (每次请求只创建一次)。 */
    if (client_id >= 0 &&
        task_get_state(client_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(client_id);
    }
    if (alloc_id >= 0 &&
        task_get_state(alloc_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(alloc_id);
    }
    if (client_ep >= 0) {
        (void)endpoint_delete(client_ep);
    }
    if (alloc_ep >= 0) {
        (void)endpoint_delete(alloc_ep);
    }
}

/*============================================================================
 * Test 2: allocator ping
 *============================================================================*/

static void test_allocator_ping(void) {
    test_section("Test 2: allocator ping");

    ep_id_t alloc_ep = endpoint_create("alloc_ping", KERN_EP_MSG_SIZE, 2);
    TEST_ASSERT(alloc_ep >= 0, "ping: allocator endpoint created");
    if (alloc_ep < 0) return;

    task_id_t alloc_id = task_create("alloc_ping", allocator_task, NULL,
                                     10, 768);
    TEST_ASSERT(alloc_id >= 0, "ping: allocator task created");
    if (alloc_id < 0) {
        (void)endpoint_delete(alloc_ep);
        return;
    }

    tcb_t *alloc_tcb = task_get_tcb(alloc_id);
    cap_id_t alloc_cap = KERN_INVALID_ID;
    if (alloc_tcb != NULL) {
        alloc_cap = cap_create_for(alloc_tcb,
                                   (void *)(uintptr_t)(alloc_ep + 1),
                                   CAP_OBJ_ENDPOINT,
                                   CAP_READ | CAP_WRITE);
    }

    /* client 用内核态直接 ping (测试任务本身是特权的) */
    /* 先拿一个 allocator ep cap 给当前测试任务 */
    cap_id_t self_cap = cap_create_for(sched_get_current(),
                                       (void *)(uintptr_t)(alloc_ep + 1),
                                       CAP_OBJ_ENDPOINT,
                                       CAP_READ | CAP_WRITE);
    TEST_ASSERT(self_cap >= 0, "ping: test got allocator ep cap");

    if (alloc_tcb != NULL && alloc_tcb->sp != NULL && alloc_cap >= 0) {
        uint32_t *stacked_r0 = (uint32_t *)((uint8_t *)alloc_tcb->sp + 32U);
        *stacked_r0 = (uint32_t)alloc_cap;
        (void)task_start(alloc_id);
    }

    int err = allocator_ping((int)self_cap, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, err, "ping: allocator responded");

    (void)sys_cap_revoke((int)self_cap);
    if (alloc_id >= 0 &&
        task_get_state(alloc_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(alloc_id);
    }
    if (alloc_ep >= 0) {
        (void)endpoint_delete(alloc_ep);
    }
}

/*============================================================================
 * Module registration
 *============================================================================*/

static void test_allocator_module(void) {
    alloc_test_reset_pairs();
    test_allocator_ping();
    test_allocator_shm_create_and_map();
}

TEST_MODULE_REGISTER(allocator, test_allocator_module);

#endif /* TEST_MODULE_ALLOCATOR && CAP_ENABLE && MPU_ENABLE */
