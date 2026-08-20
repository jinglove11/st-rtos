/**
 * @file test_syscall_user.c
 * @brief ABI 层 — 用户任务 syscall 契约
 *
 * 全部用例由用户任务经 SVC 发起:local cptr 生命周期、raw-id 拒绝、
 * timer/mem/shm 用户路径、endpoint/channel/sync 阻塞语义与 cap 传递。
 */

/**
 * @file test_syscall.c
 * @brief 系统调用接口测试 — 使用通用 sys_callN API
 */

#include "test_framework.h"
#include "kernel.h"
#include "syscall.h"
#include "user_api.h"
#include "task.h"
#include "continuation.h"
#include "semaphore.h"
#include "mutex.h"
#include "mqueue.h"
#include "event.h"
#include "endpoint.h"
#include "channel.h"
#include "timer.h"
#include "mem.h"

#if TEST_ENABLE
/* P0-5 验证补漏:TEST-off 镜像(dev/release/tiny)不得链入测试代码。
 * TEST_ENABLE 为测试代码链接总门(与既有模块级 TEST_MODULE_* 门互补)。 */

#if CAP_ENABLE
#include "capability.h"
#endif


#if SYSCALL_ENABLE

#if MPU_ENABLE && CAP_ENABLE
static void user_local_cptr_lifecycle_task(void *arg) {
    (void)arg;

    int cap = sys_sem_create(1, 0);
    if (cap < 0) {
        sys_task_exit((void *)(intptr_t)cap);
    }

    /* A capability created by a user task must be installed in that task's
     * root CNode and returned as the same task-local CPtr. */
    int self_cap = sys_cap_self_slot(CAP_OBJ_SEMAPHORE, 0);
    if (self_cap != cap) {
        (void)sys_sem_delete(cap);
        sys_task_exit((void *)(intptr_t)KERN_ERR_STATE);
    }

    int err = sys_sem_post(cap);
    if (err == KERN_OK) {
        err = sys_sem_wait(cap, 0);
    }
    if (err == KERN_OK) {
        err = sys_sem_delete(cap);
    }
    if (err == KERN_OK && sys_sem_post(cap) != KERN_ERR_CAP) {
        err = KERN_ERR_STATE;
    }

    /* Return the deleted numeric CPtr so the privileged test runner can
     * inspect its ABI tag without keeping the kernel object alive. */
    sys_task_exit(err == KERN_OK
                      ? (void *)(uintptr_t)(uint32_t)cap
                      : (void *)(intptr_t)err);
}

#define RAW_AUDIT_TASK_ACCESS    (1U << 0)
#define RAW_AUDIT_OBJECT_ACCESS  (1U << 1)
#define RAW_AUDIT_RESOURCE_CAPS  (1U << 2)
#define RAW_AUDIT_CAP_ROUTING    (1U << 3)
#define RAW_AUDIT_OBJECT_LIFE    (1U << 4)

static void user_raw_audit_peer_task(void *arg) {
    (void)arg;
    sys_task_exit(NULL);
}

/*
 * M2 final acceptance: a user task may know every small integer object ID,
 * but none of those integers is authority.  Keep real objects alive while
 * probing the complete ID ranges, then prove their CSpace caps still work.
 */
static void user_raw_object_id_control_task(void *arg) {
    int raw_task_id = (int)(uintptr_t)arg;
    uint32_t failures = 0;
    uint8_t msg[KERN_CH_MSG_SIZE];
    void *mapped = NULL;

    for (uint32_t i = 0; i < sizeof(msg); i++) {
        msg[i] = (uint8_t)i;
    }

    int sem_cap = sys_sem_create(1, 0);
    int mutex_cap = sys_mutex_create();
    int mq_cap = sys_mqueue_create(8, 2);
    int event_cap = sys_event_create(0);
    int ep_cap = sys_ep_create("raw_audit_ep", KERN_EP_MSG_SIZE, 1);
    int ch_cap = sys_ch_create(KERN_CH_MSG_SIZE, 0);
    int timer_cap = sys_timer_create("raw_audit_timer", NULL, NULL, 1);
    int frame_cap = sys_mem_alloc(64, CAP_READ | CAP_WRITE | CAP_MANAGE);
    /* P2-1: 用户直呼 sys_task_create 已封 —— peers 由特权编排者预建
     * 并授 task cap(self_slot(CAP_OBJ_TASK, 0/1) 发现)。 */
    int peer_a_cap = sys_cap_self_slot(CAP_OBJ_TASK, 0);
    int peer_b_cap = sys_cap_self_slot(CAP_OBJ_TASK, 1);

    if (sem_cap < 0 || mutex_cap < 0 || mq_cap < 0 || event_cap < 0 ||
        ep_cap < 0 || ch_cap < 0 || timer_cap < 0 || frame_cap < 0 ||
        peer_a_cap < 0 || peer_b_cap < 0) {
        failures |= RAW_AUDIT_OBJECT_LIFE;
    }

    if (sys_task_delete(raw_task_id) != KERN_ERR_CAP ||
        sys_task_suspend(raw_task_id) != KERN_ERR_CAP ||
        sys_task_set_policy(raw_task_id, SYS_SCHED_NORMAL) != KERN_ERR_CAP) {
        failures |= RAW_AUDIT_TASK_ACCESS;
    }

    for (int raw = 0; raw < KERN_MAX_SEMAPHORES; raw++) {
        if (sys_sem_post(raw) != KERN_ERR_CAP) {
            failures |= RAW_AUDIT_OBJECT_ACCESS;
        }
    }
    for (int raw = 0; raw < KERN_MAX_MUTEXES; raw++) {
        if (sys_mutex_unlock(raw) != KERN_ERR_CAP) {
            failures |= RAW_AUDIT_OBJECT_ACCESS;
        }
    }
    for (int raw = 0; raw < KERN_MAX_MQUEUES; raw++) {
        if (sys_mqueue_send(raw, msg, 0) != KERN_ERR_CAP) {
            failures |= RAW_AUDIT_OBJECT_ACCESS;
        }
    }
    for (int raw = 0; raw < KERN_MAX_EVENTS; raw++) {
        if (sys_event_set(raw, 1U) != KERN_ERR_CAP) {
            failures |= RAW_AUDIT_OBJECT_ACCESS;
        }
    }
    for (int raw = 0; raw < KERN_MAX_ENDPOINTS; raw++) {
        if (sys_ep_send(raw, msg, 0) != KERN_ERR_CAP) {
            failures |= RAW_AUDIT_OBJECT_ACCESS;
        }
    }
    for (int raw = 0; raw < KERN_MAX_CHANNELS; raw++) {
        if (sys_ch_send(raw, msg, 0) != KERN_ERR_CAP) {
            failures |= RAW_AUDIT_OBJECT_ACCESS;
        }
    }
    for (int raw = 0; raw < TIMER_MAX; raw++) {
        if (sys_timer_start(raw, 1) != KERN_ERR_CAP) {
            failures |= RAW_AUDIT_OBJECT_ACCESS;
        }
    }
    for (int raw = 0; raw < KFRAME_OBJECT_MAX; raw++) {
        if (sys_mem_size(raw) != KERN_ERR_CAP) {
            failures |= RAW_AUDIT_OBJECT_ACCESS;
        }
    }

    factory_create_request_t request = {
        .obj_type = CAP_OBJ_SEMAPHORE,
        .param0 = 0,
        .param1 = 1,
    };
    for (int raw = 0; raw < KERN_MAX_FACTORIES; raw++) {
        if (sys_factory_create(raw, &request) != KERN_ERR_CAP) {
            failures |= RAW_AUDIT_RESOURCE_CAPS;
        }
    }

#if IRQ_ENABLE && IPC_ENDPOINT
    if (ep_cap >= 0) {
        for (int raw = 0; raw < IRQ_MAX_USER; raw++) {
            if (sys_irq_bind(raw, ep_cap, 0x52415749U) != KERN_ERR_CAP) {
                failures |= RAW_AUDIT_RESOURCE_CAPS;
            }
        }
    } else {
        failures |= RAW_AUDIT_RESOURCE_CAPS;
    }
#endif

#if MPU_ENABLE
    for (int raw = 0; raw < 8; raw++) {
        if (sys_mmio_map(raw, CAP_READ, &mapped) != KERN_ERR_CAP) {
            failures |= RAW_AUDIT_RESOURCE_CAPS;
        }
    }
#endif

    /*
     * These were the two remaining raw-ID routing paths:
     * - legacy CAP_TRANSFER selected the destination by task ID;
     * - CH_CONNECT selected both peers by task ID.
     */
    if (sem_cap >= 0) {
        if (sys_call2(SYSCALL_CAP_TRANSFER, sem_cap, raw_task_id) !=
                KERN_ERR_CAP ||
            sys_cap_transfer_to(sem_cap, raw_task_id) != KERN_ERR_CAP) {
            failures |= RAW_AUDIT_CAP_ROUTING;
        }
    }
    if (ch_cap >= 0 &&
        sys_ch_connect(ch_cap, raw_task_id, sys_task_self()) != KERN_ERR_CAP) {
        failures |= RAW_AUDIT_CAP_ROUTING;
    }
    if (ch_cap >= 0 && peer_a_cap >= 0 && peer_b_cap >= 0 &&
        sys_ch_connect(ch_cap, peer_a_cap, peer_b_cap) != KERN_OK) {
        failures |= RAW_AUDIT_CAP_ROUTING;
    }

    /* Failed raw-ID probes must not mutate or move the real objects. */
    if (sem_cap >= 0 &&
        (sys_sem_post(sem_cap) != KERN_OK ||
         sys_sem_wait(sem_cap, 0) != KERN_OK)) {
        failures |= RAW_AUDIT_OBJECT_LIFE;
    }
    if (frame_cap >= 0 && sys_mem_size(frame_cap) != 64) {
        failures |= RAW_AUDIT_OBJECT_LIFE;
    }

    if (frame_cap >= 0 && sys_mem_free(frame_cap) != KERN_OK) {
        failures |= RAW_AUDIT_OBJECT_LIFE;
    }
    if (timer_cap >= 0 && sys_cap_revoke(timer_cap) != KERN_OK) {
        failures |= RAW_AUDIT_OBJECT_LIFE;
    }
    if (ch_cap >= 0 && sys_ch_delete(ch_cap) != KERN_OK) {
        failures |= RAW_AUDIT_OBJECT_LIFE;
    }
    if (peer_b_cap >= 0 && sys_task_delete(peer_b_cap) != KERN_OK) {
        failures |= RAW_AUDIT_OBJECT_LIFE;
    }
    if (peer_a_cap >= 0 && sys_task_delete(peer_a_cap) != KERN_OK) {
        failures |= RAW_AUDIT_OBJECT_LIFE;
    }
    if (ep_cap >= 0 && sys_ep_delete(ep_cap) != KERN_OK) {
        failures |= RAW_AUDIT_OBJECT_LIFE;
    }
    if (event_cap >= 0 && sys_event_delete(event_cap) != KERN_OK) {
        failures |= RAW_AUDIT_OBJECT_LIFE;
    }
    if (mq_cap >= 0 && sys_mqueue_delete(mq_cap) != KERN_OK) {
        failures |= RAW_AUDIT_OBJECT_LIFE;
    }
    if (mutex_cap >= 0 && sys_mutex_delete(mutex_cap) != KERN_OK) {
        failures |= RAW_AUDIT_OBJECT_LIFE;
    }
    if (sem_cap >= 0 && sys_sem_delete(sem_cap) != KERN_OK) {
        failures |= RAW_AUDIT_OBJECT_LIFE;
    }

    sys_task_exit((void *)(uintptr_t)failures);
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

static void user_timer_notify_task(void *arg) {
    (void)arg;
    int ep_cap = sys_cap_self_slot(CAP_OBJ_ENDPOINT, 0);
    int timer_cap = sys_cap_self_slot(CAP_OBJ_TIMER, 0);
    uint8_t msg_buf[KERN_EP_MSG_SIZE];
    uint32_t *msg = (uint32_t *)msg_buf;
    int err;

    for (uint32_t i = 0; i < sizeof(msg_buf); i++) {
        msg_buf[i] = 0;
    }

    err = sys_timer_bind(timer_cap, ep_cap, 0x54555352U);
    if (err != KERN_OK) {
        sys_task_exit((void *)(intptr_t)err);
    }

    err = sys_timer_start(timer_cap, 3);
    if (err != KERN_OK) {
        sys_task_exit((void *)(intptr_t)err);
    }

    err = sys_ep_recv(ep_cap, msg_buf, 1000);
    if (err == KERN_OK && msg[0] != 0x54555352U) {
        err = KERN_ERR_STATE;
    }

    sys_task_exit((void *)(intptr_t)err);
}

static void user_mem_cap_task(void *arg) {
    (void)arg;

    int cap = sys_mem_alloc(32, CAP_READ | CAP_WRITE | CAP_MANAGE);
    int err = KERN_OK;
    if (cap < 0) {
        sys_task_exit((void *)(intptr_t)cap);
    }
    if (sys_cap_type(cap) != CAP_OBJ_FRAME) {
        (void)sys_mem_free(cap);
        sys_task_exit((void *)(intptr_t)KERN_ERR_STATE);
    }

    int size = sys_mem_size(cap);
    if (size != 32) {
        err = KERN_ERR_STATE;
    }

    if (err == KERN_OK) {
        err = sys_mem_free(cap);
    } else {
        (void)sys_mem_free(cap);
    }

    sys_task_exit((void *)(intptr_t)err);
}

static void user_shm_map_task(void *arg) {
    (void)arg;
    int cap = sys_cap_self_slot(CAP_OBJ_SHM, 0);
    int err = KERN_OK;

    int created = sys_shm_create(256, CAP_READ | CAP_WRITE);
    if (created != KERN_ERR_PERM) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_PERM);
    }

    int bad = sys_call2(SYSCALL_SHM_MAP, KERN_INVALID_ID, CAP_READ);
    if (bad != KERN_ERR_CAP) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_STATE);
    }

    bad = sys_call2(SYSCALL_SHM_MAP, cap, CAP_MANAGE);
    if (bad != KERN_ERR_PARAM) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_STATE);
    }

    uint8_t *shm = (uint8_t *)sys_shm_map(cap, CAP_READ | CAP_WRITE);
    if ((intptr_t)shm < 0) {
        sys_task_exit((void *)(intptr_t)shm);
    }

    if (shm[0] != 0x5aU) {
        err = KERN_ERR_STATE;
    } else {
        shm[1] = 0xa5U;
    }

    if (err == KERN_OK) {
        err = sys_shm_unmap(cap);
    } else {
        (void)sys_shm_unmap(cap);
    }

    if (err == KERN_OK) {
        int second = sys_shm_unmap(cap);
        if (second != KERN_ERR_NOEXIST) {
            err = KERN_ERR_STATE;
        }
    }

    sys_task_exit((void *)(intptr_t)err);
}

static void user_shm_map_exhaust_task(void *arg) {
    (void)arg;
    int ep_cap = sys_cap_self_slot(CAP_OBJ_ENDPOINT, 0);
    uint8_t msg_buf[KERN_EP_MSG_SIZE];
    uint32_t *msg = (uint32_t *)msg_buf;
    cap_id_t caps[IPC_CAPS_MAX];
    cap_id_t mapped[TASK_SHM_MAP_MAX];
    uint8_t cap_count = 0;
    int err = KERN_OK;

    for (uint32_t i = 0; i < TASK_SHM_MAP_MAX; i++) {
        mapped[i] = KERN_INVALID_ID;
    }

    for (uint32_t i = 0; i < TASK_SHM_MAP_MAX + 1; i++) {
        for (uint32_t j = 0; j < sizeof(msg_buf); j++) {
            msg_buf[j] = 0;
        }
        for (uint32_t j = 0; j < IPC_CAPS_MAX; j++) {
            caps[j] = KERN_INVALID_ID;
        }
        cap_count = 0;

        err = sys_ep_recv_caps(ep_cap, msg_buf, caps, &cap_count, 1000);
        if (err != KERN_OK) {
            break;
        }
        if (cap_count != 1) {
            err = KERN_ERR_STATE;
            break;
        }

        void *addr = sys_shm_map(caps[0], CAP_READ | CAP_WRITE);
        if (i < TASK_SHM_MAP_MAX) {
            if ((intptr_t)addr < 0) {
                err = (int)(intptr_t)addr;
                break;
            }
            mapped[i] = caps[0];
            *msg = i + 1U;
            err = sys_ep_reply(ep_cap, msg_buf);
            if (err != KERN_OK) {
                break;
            }
        } else {
            if ((intptr_t)addr != KERN_ERR_RESOURCE) {
                err = KERN_ERR_STATE;
                break;
            }
            *msg = 0xeeU;
            err = sys_ep_reply(ep_cap, msg_buf);
            break;
        }
    }

    for (uint32_t i = 0; i < TASK_SHM_MAP_MAX; i++) {
        if (mapped[i] >= 0) {
            int unmap_err = sys_shm_unmap(mapped[i]);
            if (err == KERN_OK && unmap_err != KERN_OK) {
                err = unmap_err;
            }
        }
    }

    sys_task_exit((void *)(intptr_t)err);
}

static void test_set_created_task_arg(tcb_t *tcb, uintptr_t arg) {
    if (tcb == NULL || tcb->sp == NULL) {
        return;
    }

    uint32_t *stacked_r0 = (uint32_t *)((uint8_t *)tcb->sp + 32U);
    *stacked_r0 = (uint32_t)arg;
}
#endif

static void test_user_local_cptr_lifecycle(void) {
    test_section("Test 8b: User-local CPtr syscall lifecycle");

#if MPU_ENABLE && CAP_ENABLE
    task_id_t user = task_create_user("u_local_cap",
                                      user_local_cptr_lifecycle_task,
                                      NULL, 5, 512);
    TEST_ASSERT(user >= 0, "local CPtr user task created");
    if (user < 0) return;

    task_start(user);
    void *retval = NULL;
    kern_err_t join_err = task_join(user, &retval, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)join_err,
                   "local CPtr user task joined");
    if (join_err == KERN_OK) {
        cap_id_t returned = (cap_id_t)(uintptr_t)retval;
        TEST_ASSERT(cap_is_local_cptr(returned),
                    "user syscall returned a task-local CPtr");
    }
#else
    test_skip("MPU or capability disabled");
#endif
}

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

/* P2-1: 用户态直呼 sys_task_create 已封 —— 必得 KERN_ERR_PERM
 * (任务创建唯一合法路径:factory cap 的 CAP_OBJ_TASK 位)。 */
static void user_task_create_denied_task(void *arg) {
    (void)arg;
    int r = sys_task_create("denied_spawn",
                            (void (*)(void *))user_task_create_denied_task,
                            NULL, 8, 512);
    sys_task_exit((void *)(intptr_t)
                  (r == KERN_ERR_PERM ? KERN_OK : KERN_ERR_STATE));
}

static void test_task_create_user_gate(void) {
    test_section("Test 9z: user sys_task_create gated to PERM");
    test_user_case("task_create user gate", user_task_create_denied_task,
                   KERN_OK, 5000U);
}

static void test_syscall_security_negative(void) {
    test_section("Test 9: Syscall security negative cases");

    int err = sys_call0(SYSCALL_TABLE_SIZE + 1);
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, err, "invalid syscall number rejected");

    err = test_invoke_svc2_invalid();
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, err, "invalid SVC immediate rejected");

    err = test_invoke_svc0_again();
    TEST_ASSERT_EQ((int)KERN_ERR_STATE, err, "running task cannot reuse SVC #0");

#if MPU_ENABLE && CAP_ENABLE
    task_id_t victim = task_create_user("raw_victim",
                                        user_raw_object_id_control_task,
                                        NULL, 10, 1024);
    TEST_ASSERT(victim >= 0, "raw-id victim task created");
    if (victim < 0) return;

    task_id_t attacker = task_create_user("raw_attacker",
                                          user_raw_object_id_control_task,
                                          (void *)(uintptr_t)victim, 5, 1024);
    TEST_ASSERT(attacker >= 0, "raw-id attacker task created");
    if (attacker < 0) {
        (void)task_delete(victim);
        return;
    }

    /* P2-1: 预建 peers(不 start,仅作为 cap 载体)并授予 attacker */
    task_id_t peer_a = task_create_user("raw_peer_a",
                                        user_raw_audit_peer_task,
                                        NULL, 8, 512);
    task_id_t peer_b = task_create_user("raw_peer_b",
                                        user_raw_audit_peer_task,
                                        NULL, 8, 512);
    TEST_ASSERT(peer_a >= 0 && peer_b >= 0, "audit peers pre-created");
    tcb_t *atk = attacker >= 0 ? task_get_tcb(attacker) : NULL;
    if (peer_a >= 0 && peer_b >= 0 && atk != NULL) {
        (void)cap_create_for(atk, task_obj_for_cap(peer_a),
                             CAP_OBJ_TASK, CAP_FULL);
        (void)cap_create_for(atk, task_obj_for_cap(peer_b),
                             CAP_OBJ_TASK, CAP_FULL);
    }

    task_start(attacker);
    void *retval = NULL;
    kern_err_t join_err = task_join(attacker, &retval, 4000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)join_err, "raw-id attacker joined");
    uint32_t raw_audit = join_err == KERN_OK
        ? (uint32_t)(uintptr_t)retval : UINT32_MAX;
    TEST_ASSERT_EQ(0, (int)(raw_audit & RAW_AUDIT_TASK_ACCESS),
                   "user raw task-id management rejected");
    TEST_ASSERT_EQ(0, (int)(raw_audit & RAW_AUDIT_OBJECT_ACCESS),
                   "user raw sync/IPC/timer/frame IDs rejected");
    TEST_ASSERT_EQ(0, (int)(raw_audit & RAW_AUDIT_RESOURCE_CAPS),
                   "user raw factory/IRQ/MMIO IDs rejected");
    TEST_ASSERT_EQ(0, (int)(raw_audit & RAW_AUDIT_CAP_ROUTING),
                   "raw task IDs rejected by cap and channel routing");
    TEST_ASSERT_EQ(0, (int)(raw_audit & RAW_AUDIT_OBJECT_LIFE),
                   "raw-ID probes leave capability objects intact");

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

static void test_user_timer_endpoint_notification(void) {
    test_section("Test 9b: User timer endpoint notification");

#if MPU_ENABLE && CAP_ENABLE && IPC_ENDPOINT && TIMER_ENABLE
    ep_id_t ep = endpoint_create("u_tmr_ep", KERN_EP_MSG_SIZE, 2);
    TEST_ASSERT(ep >= 0, "user timer endpoint created");
    if (ep < 0) return;

    timer_id_t tid = timer_create("u_tmr_nt", NULL, NULL, 0);
    TEST_ASSERT(tid >= 0, "user notification timer created");
    if (tid < 0) {
        endpoint_delete(ep);
        return;
    }

    cap_id_t ep_cap = cap_create(endpoint_obj_for_cap(ep), CAP_OBJ_ENDPOINT, CAP_FULL, 0);
    TEST_ASSERT(ep_cap >= 0, "user timer endpoint cap created");
    if (ep_cap < 0) {
        timer_delete(tid);
        endpoint_delete(ep);
        return;
    }

    cap_id_t timer_cap = cap_create_for(NULL, timer_obj_for_cap(tid), CAP_OBJ_TIMER, CAP_FULL);
    TEST_ASSERT(timer_cap >= 0, "user timer cap created");
    if (timer_cap < 0) {
        cap_delete(ep_cap);
        timer_delete(tid);
        endpoint_delete(ep);
        return;
    }

    task_id_t user = task_create_user("u_tmr_nt",
                                      user_timer_notify_task,
                                      NULL,
                                      5, 768);
    TEST_ASSERT(user >= 0, "user timer notify task created");
    if (user < 0) {
        cap_delete(timer_cap);
        cap_delete(ep_cap);
        timer_delete(tid);
        endpoint_delete(ep);
        return;
    }

    kern_err_t err = cap_transfer(ep_cap, (uint8_t)user);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "timer endpoint cap transferred");
    if (err != KERN_OK) {
        (void)task_delete(user);
        cap_delete(timer_cap);
        timer_delete(tid);
        endpoint_delete(ep);
        return;
    }

    err = cap_transfer(timer_cap, (uint8_t)user);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "timer cap transferred");
    if (err != KERN_OK) {
        (void)task_delete(user);
        timer_delete(tid);
        endpoint_delete(ep);
        return;
    }

    /* cap_a(R0)=ep_cap, cap_b(table)=timer_cap */
    task_start(user);

    void *retval = NULL;
    kern_err_t join_err = task_join(user, &retval, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)join_err, "timer notify user joined");
    TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                   "user received timer endpoint notification");

    timer_delete(tid);
    endpoint_delete(ep);
#else
    test_skip("MPU, capability, endpoint, or timer disabled");
#endif
}

static void test_user_mem_cap_syscalls(void) {
    test_section("Test 9c: User memory cap syscalls");

#if MPU_ENABLE && CAP_ENABLE && MEM_DYNAMIC
    uint32_t outstanding = mem_get_outstanding_allocs();
    task_id_t user = task_create_user("u_mem_cap",
                                      user_mem_cap_task,
                                      NULL, 5, 768);
    TEST_ASSERT(user >= 0, "user memory cap task created");
    if (user < 0) return;

    task_start(user);

    void *retval = NULL;
    kern_err_t join_err = task_join(user, &retval, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)join_err, "memory cap user joined");
    TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                   "user memory cap syscalls returned OK");
    TEST_ASSERT_EQ((int)outstanding, (int)mem_get_outstanding_allocs(),
                   "user memory cap cleanup restored outstanding");

    int err = sys_mem_size(KERN_INVALID_ID);
    TEST_ASSERT_EQ((int)KERN_ERR_CAP, err,
                   "invalid mem cap size rejected");
#else
    test_skip("MPU, capability, or dynamic memory disabled");
#endif
}

static void test_user_shm_map_syscalls(void) {
    test_section("Test 9d: User SHM map syscalls");

#if MPU_ENABLE && CAP_ENABLE && MEM_DYNAMIC
    uint32_t outstanding = mem_get_outstanding_allocs();
    cap_id_t root = kshm_create_aligned_cap(256,
                                            CAP_READ | CAP_WRITE |
                                            CAP_MANAGE | CAP_TRANSFER |
                                            CAP_GRANT);
    TEST_ASSERT(root >= 0, "kernel created aligned SHM cap");
    if (root < 0) return;

    void *base = NULL;
    kern_err_t err = kshm_get_range(root, CAP_WRITE, 0, 2, &base);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "kernel resolved SHM setup range");
    if (err == KERN_OK) {
        ((uint8_t *)base)[0] = 0x5aU;
        ((uint8_t *)base)[1] = 0x00U;
    }

    task_id_t user = task_create_user("u_shm_map",
                                      user_shm_map_task,
                                      NULL, 5, 768);
    TEST_ASSERT(user >= 0, "user SHM map task created");
    if (user < 0) {
        (void)kshm_delete_cap(root);
        return;
    }

    tcb_t *tcb = task_get_tcb(user);
    TEST_ASSERT_NOT_NULL(tcb, "user SHM map TCB resolved");
    cap_id_t user_cap = KERN_INVALID_ID;
    if (tcb != NULL) {
        user_cap = cap_copy_to(NULL, root, tcb, CAP_READ | CAP_WRITE);
    }
    TEST_ASSERT(user_cap >= 0, "SHM cap copied into user CSpace");
    if (user_cap < 0) {
        (void)task_delete(user);
        (void)kshm_delete_cap(root);
        return;
    }

    test_set_created_task_arg(tcb, (uintptr_t)user_cap);
    task_start(user);

    void *retval = NULL;
    kern_err_t join_err = task_join(user, &retval, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)join_err, "SHM map user joined");
    TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                   "user SHM map syscalls returned OK");
    if (base != NULL) {
        TEST_ASSERT_EQ(0xa5, (int)((uint8_t *)base)[1],
                       "user wrote through mapped SHM");
    }

    err = kshm_delete_cap(root);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "kernel deleted SHM root cap");
    TEST_ASSERT_EQ((int)outstanding, (int)mem_get_outstanding_allocs(),
                   "user SHM map cleanup restored outstanding");
#else
    test_skip("MPU, capability, or dynamic memory disabled");
#endif
}

static void test_user_shm_map_region_exhaustion(void) {
    test_section("Test 9f: User SHM map region exhaustion");

#if MPU_ENABLE && CAP_ENABLE && MEM_DYNAMIC && IPC_ENDPOINT
    uint32_t outstanding = mem_get_outstanding_allocs();
    ep_id_t ep = endpoint_create("u_shmx", sizeof(uint32_t), 2);
    TEST_ASSERT(ep >= 0, "SHM exhaustion endpoint created");
    if (ep < 0) return;

    cap_id_t ep_cap = cap_create(endpoint_obj_for_cap(ep), CAP_OBJ_ENDPOINT, CAP_FULL, 0);
    TEST_ASSERT(ep_cap >= 0, "SHM exhaustion endpoint cap created");
    if (ep_cap < 0) {
        endpoint_delete(ep);
        return;
    }

    cap_id_t roots[TASK_SHM_MAP_MAX + 1];
    for (uint32_t i = 0; i < TASK_SHM_MAP_MAX + 1; i++) {
        roots[i] = KERN_INVALID_ID;
    }

    int setup_ok = 1;
    for (uint32_t i = 0; i < TASK_SHM_MAP_MAX + 1; i++) {
        roots[i] = kshm_create_aligned_cap(256,
                                           CAP_READ | CAP_WRITE |
                                           CAP_MANAGE | CAP_TRANSFER |
                                           CAP_GRANT);
        if (roots[i] < 0) {
            setup_ok = 0;
            break;
        }
    }
    TEST_ASSERT(setup_ok == 1, "SHM exhaustion root caps created");
    if (!setup_ok) {
        for (uint32_t i = 0; i < TASK_SHM_MAP_MAX + 1; i++) {
            if (roots[i] >= 0) {
                (void)kshm_delete_cap(roots[i]);
            }
        }
        cap_delete(ep_cap);
        endpoint_delete(ep);
        return;
    }

    task_id_t user = task_create_user("u_shm_x",
                                      user_shm_map_exhaust_task,
                                      (void *)(uintptr_t)ep_cap,
                                      5, 1024);
    TEST_ASSERT(user >= 0, "SHM exhaustion user task created");
    if (user < 0) {
        for (uint32_t i = 0; i < TASK_SHM_MAP_MAX + 1; i++) {
            (void)kshm_delete_cap(roots[i]);
        }
        cap_delete(ep_cap);
        endpoint_delete(ep);
        return;
    }

    kern_err_t err = cap_transfer(ep_cap, (uint8_t)user);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "SHM exhaustion endpoint cap transferred");
    if (err != KERN_OK) {
        (void)task_delete(user);
        for (uint32_t i = 0; i < TASK_SHM_MAP_MAX + 1; i++) {
            (void)kshm_delete_cap(roots[i]);
        }
        endpoint_delete(ep);
        return;
    }

    task_start(user);

    for (uint32_t i = 0; i < TASK_SHM_MAP_MAX + 1; i++) {
        ipc_cap_xfer_t xfer;
        xfer.src_cap = roots[i];
        xfer.rights = CAP_READ | CAP_WRITE;
        xfer.flags = IPC_CAP_COPY;

        uint32_t msg = 0x53000000U | i;
        err = endpoint_send_caps(ep, &msg, &xfer, 1, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "kernel sent SHM cap for exhaustion");
        if (i < TASK_SHM_MAP_MAX) {
            TEST_ASSERT_EQ((int)(i + 1U), (int)msg,
                           "user mapped SHM region before exhaustion");
        } else {
            TEST_ASSERT_EQ(0xee, (int)msg,
                           "user observed SHM map region exhaustion");
        }
        if (err != KERN_OK) {
            break;
        }
    }

    void *retval = NULL;
    kern_err_t join_err = task_join(user, &retval, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)join_err,
                   "SHM exhaustion user joined");
    TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                   "user SHM exhaustion result OK");

    for (uint32_t i = 0; i < TASK_SHM_MAP_MAX + 1; i++) {
        err = kshm_delete_cap(roots[i]);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "SHM exhaustion root cap deleted");
    }
    endpoint_delete(ep);
    TEST_ASSERT_EQ((int)outstanding, (int)mem_get_outstanding_allocs(),
                   "SHM exhaustion cleanup restored outstanding");
#else
    test_skip("MPU, capability, dynamic memory, or endpoint disabled");
#endif
}

/*============================================================================
 * 测试 10: 用户态服务通过 sleepable endpoint syscall 处理请求
 *============================================================================*/

#if MPU_ENABLE && IPC_ENDPOINT && CAP_ENABLE
static void user_endpoint_service_task(void *arg) {
    (void)arg;
    int ep_cap = sys_cap_self_slot(CAP_OBJ_ENDPOINT, 0);
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

static void user_endpoint_reply_cap_service_task(void *arg) {
    (void)arg;
    int ep_cap = sys_cap_self_slot(CAP_OBJ_ENDPOINT, 0);
    uint8_t msg_buf[KERN_EP_MSG_SIZE];
    uint32_t *req = (uint32_t *)msg_buf;
    int err;

    for (uint32_t i = 0; i < sizeof(msg_buf); i++) {
        msg_buf[i] = 0;
    }

    err = sys_ep_recv(ep_cap, msg_buf, 1000);
    if (err != KERN_OK) {
        sys_task_exit((void *)(uintptr_t)(0x1000 | ((uint32_t)(-err) & 0xffU)));
    }
    if (err == KERN_OK) {
        int reply_cap = sys_ep_take_reply(ep_cap);
        if (reply_cap < 0) {
            if (reply_cap == KERN_ERR_CAP) {
                err = 0x2100;
            } else if (reply_cap == KERN_INVALID_ID) {
                err = 0x2200;
            } else {
                err = (int)(0x2000 | ((uint32_t)(-reply_cap) & 0xffU));
            }
        } else {
            *req += 200;
            err = sys_ep_reply(reply_cap, msg_buf);
            if (err != KERN_OK) {
                err = (int)(0x3000 | ((uint32_t)(-err) & 0xffU));
            } else {
                int second = sys_ep_reply(reply_cap, msg_buf);
                if (second == KERN_OK) {
                    err = 0x4000;
                } else {
                    err = KERN_OK;
                }
            }
        }
    }

    sys_task_exit((void *)(uintptr_t)err);
}

static void user_endpoint_reply_cap_timeout_service_task(void *arg) {
    (void)arg;
    int ep_cap = sys_cap_self_slot(CAP_OBJ_ENDPOINT, 0);
    uint8_t msg_buf[KERN_EP_MSG_SIZE];
    int err;

    for (uint32_t i = 0; i < sizeof(msg_buf); i++) {
        msg_buf[i] = 0;
    }

    err = sys_ep_recv(ep_cap, msg_buf, 1000);
    if (err != KERN_OK) {
        sys_task_exit((void *)(uintptr_t)(0x1000 | ((uint32_t)(-err) & 0xffU)));
    }

    int reply_cap = sys_ep_take_reply(ep_cap);
    if (reply_cap < 0) {
        sys_task_exit((void *)(uintptr_t)(0x2000 | ((uint32_t)(-reply_cap) & 0xffU)));
    }

    err = sys_task_delay(20);
    if (err != KERN_OK) {
        sys_task_exit((void *)(uintptr_t)(0x3000 | ((uint32_t)(-err) & 0xffU)));
    }

    err = sys_ep_reply(reply_cap, msg_buf);
    if (err == KERN_OK) {
        sys_task_exit((void *)(uintptr_t)0x4000);
    }

    sys_task_exit((void *)(uintptr_t)KERN_OK);
}

static void user_endpoint_recv_timeout_task(void *arg) {
    (void)arg;
    int ep_cap = sys_cap_self_slot(CAP_OBJ_ENDPOINT, 0);
    uint8_t msg_buf[KERN_EP_MSG_SIZE];
    int err;

    for (uint32_t i = 0; i < sizeof(msg_buf); i++) {
        msg_buf[i] = 0;
    }

    err = sys_ep_recv(ep_cap, msg_buf, 2);
    sys_task_exit((void *)(uintptr_t)err);
}

static void user_endpoint_client_task(void *arg) {
    (void)arg;
    int ep_cap = sys_cap_self_slot(CAP_OBJ_ENDPOINT, 0);
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

static void user_endpoint_send_caps_task(void *arg) {
    (void)arg;
    int ep_cap = sys_cap_self_slot(CAP_OBJ_ENDPOINT, 0);
    cap_id_t src_cap = sys_cap_self_slot(CAP_OBJ_ENDPOINT, 1);
    uint8_t msg_buf[KERN_EP_MSG_SIZE];
    uint32_t *msg = (uint32_t *)msg_buf;
    ipc_cap_xfer_t xfers[IPC_CAPS_MAX];
    int err;

    for (uint32_t i = 0; i < sizeof(msg_buf); i++) {
        msg_buf[i] = 0;
    }
    for (uint32_t i = 0; i < IPC_CAPS_MAX; i++) {
        xfers[i].src_cap = KERN_INVALID_ID;
        xfers[i].rights = 0;
        xfers[i].flags = IPC_CAP_COPY;
    }

    *msg = 55;
    xfers[0].src_cap = src_cap;
    xfers[0].rights = CAP_READ;
    xfers[0].flags = IPC_CAP_COPY;

    err = sys_ep_send_caps(ep_cap, msg_buf, xfers, 1, 1000);
    if (err == KERN_OK && *msg != 56) {
        err = KERN_ERR;
    }

    sys_task_exit((void *)(uintptr_t)err);
}

static void user_endpoint_recv_caps_service_task(void *arg) {
    (void)arg;
    int ep_cap = sys_cap_self_slot(CAP_OBJ_ENDPOINT, 0);
    uint8_t msg_buf[KERN_EP_MSG_SIZE];
    uint32_t *msg = (uint32_t *)msg_buf;
    cap_id_t caps[IPC_CAPS_MAX];
    uint8_t cap_count = 0;
    int err;

    for (uint32_t i = 0; i < sizeof(msg_buf); i++) {
        msg_buf[i] = 0;
    }
    for (uint32_t i = 0; i < IPC_CAPS_MAX; i++) {
        caps[i] = KERN_INVALID_ID;
    }

    err = sys_ep_recv_caps(ep_cap, msg_buf, caps, &cap_count, 1000);
    if (err == KERN_OK && cap_count != 1) {
        err = KERN_ERR;
    }

    if (err == KERN_OK) {
        err = sys_sem_post(caps[0]);
    }

    if (err == KERN_OK) {
        *msg += 2;
        err = sys_ep_reply(ep_cap, msg_buf);
    }

    sys_task_exit((void *)(uintptr_t)err);
}

static void user_endpoint_recv_mem_cap_task(void *arg) {
    (void)arg;
    int ep_cap = sys_cap_self_slot(CAP_OBJ_ENDPOINT, 0);
    uint8_t msg_buf[KERN_EP_MSG_SIZE];
    uint32_t *msg = (uint32_t *)msg_buf;
    cap_id_t caps[IPC_CAPS_MAX];
    uint8_t cap_count = 0;
    int err;

    for (uint32_t i = 0; i < sizeof(msg_buf); i++) {
        msg_buf[i] = 0;
    }
    for (uint32_t i = 0; i < IPC_CAPS_MAX; i++) {
        caps[i] = KERN_INVALID_ID;
    }

    err = sys_ep_recv_caps(ep_cap, msg_buf, caps, &cap_count, 1000);
    if (err == KERN_OK && cap_count != 1) {
        err = KERN_ERR_RESOURCE;
    }
    if (err == KERN_OK && sys_mem_size(caps[0]) != 48) {
        err = KERN_ERR_STATE;
    }
    if (err == KERN_OK) {
        *msg += 3;
        err = sys_ep_reply(ep_cap, msg_buf);
    }

    sys_task_exit((void *)(uintptr_t)err);
}

static void user_endpoint_send_wait_task(void *arg) {
    (void)arg;
    int ep_cap = sys_cap_self_slot(CAP_OBJ_ENDPOINT, 0);
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
    (void)arg;
    int ep_cap = sys_cap_self_slot(CAP_OBJ_ENDPOINT, 0);
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
    (void)arg;
    int sem_cap = sys_cap_self_slot(CAP_OBJ_SEMAPHORE, 0);
    int err = sys_sem_wait(sem_cap, 1000);
    sys_task_exit((void *)(intptr_t)err);
}

static void user_sem_wait_timeout_task(void *arg) {
    (void)arg;
    int sem_cap = sys_cap_self_slot(CAP_OBJ_SEMAPHORE, 0);
    int err = sys_sem_wait(sem_cap, 2);
    sys_task_exit((void *)(intptr_t)err);
}

static void user_mutex_lock_task(void *arg) {
    (void)arg;
    int mutex_cap = sys_cap_self_slot(CAP_OBJ_MUTEX, 0);
    int err = sys_mutex_lock(mutex_cap, 1000);
    if (err == KERN_OK) {
        err = sys_mutex_unlock(mutex_cap);
    }
    sys_task_exit((void *)(intptr_t)err);
}

static void user_mutex_lock_timeout_task(void *arg) {
    (void)arg;
    int mutex_cap = sys_cap_self_slot(CAP_OBJ_MUTEX, 0);
    int err = sys_mutex_lock(mutex_cap, 2);
    sys_task_exit((void *)(intptr_t)err);
}

static void user_mqueue_recv_task(void *arg) {
    (void)arg;
    int mq_cap = sys_cap_self_slot(CAP_OBJ_MQUEUE, 0);
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
    (void)arg;
    int mq_cap = sys_cap_self_slot(CAP_OBJ_MQUEUE, 0);
    uint8_t msg_buf[KERN_MSG_MAX_SIZE];

    for (uint32_t i = 0; i < sizeof(msg_buf); i++) {
        msg_buf[i] = 0;
    }

    int err = sys_mqueue_recv(mq_cap, msg_buf, 2);
    sys_task_exit((void *)(intptr_t)err);
}

static void user_mqueue_send_task(void *arg) {
    (void)arg;
    int mq_cap = sys_cap_self_slot(CAP_OBJ_MQUEUE, 0);
    uint8_t msg_buf[KERN_MSG_MAX_SIZE];
    uint32_t *msg = (uint32_t *)msg_buf;

    for (uint32_t i = 0; i < sizeof(msg_buf); i++) {
        msg_buf[i] = 0;
    }
    *msg = 0x4d515345U;

    int err = sys_mqueue_send(mq_cap, msg_buf, 1000);
    sys_task_exit((void *)(intptr_t)err);
}

static void user_event_wait_task(void *arg) {
    (void)arg;
    int event_cap = sys_cap_self_slot(CAP_OBJ_EVENT, 0);
    int err = sys_event_wait(event_cap, 0x4U, 1000);
    sys_task_exit((void *)(intptr_t)err);
}

static void user_event_wait_timeout_task(void *arg) {
    (void)arg;
    int event_cap = sys_cap_self_slot(CAP_OBJ_EVENT, 0);
    int err = sys_event_wait(event_cap, 0x8U, 2);
    sys_task_exit((void *)(intptr_t)err);
}

static void user_channel_recv_task(void *arg) {
    (void)arg;
    int ch_cap = sys_cap_self_slot(CAP_OBJ_CHANNEL, 0);
    uint8_t msg_buf[KERN_CH_MSG_SIZE];
    uint32_t *msg = (uint32_t *)msg_buf;

    for (uint32_t i = 0; i < sizeof(msg_buf); i++) {
        msg_buf[i] = 0;
    }

    int err = sys_ch_recv(ch_cap, msg_buf, 1000);
    if (err == KERN_OK && *msg != 0x43485258U) {
        err = KERN_ERR_STATE;
    }
    sys_task_exit((void *)(intptr_t)err);
}

static void user_channel_recv_timeout_task(void *arg) {
    (void)arg;
    int ch_cap = sys_cap_self_slot(CAP_OBJ_CHANNEL, 0);
    uint8_t msg_buf[KERN_CH_MSG_SIZE];

    for (uint32_t i = 0; i < sizeof(msg_buf); i++) {
        msg_buf[i] = 0;
    }

    int err = sys_ch_recv(ch_cap, msg_buf, 2);
    sys_task_exit((void *)(intptr_t)err);
}

static void user_channel_send_twice_task(void *arg) {
    (void)arg;
    int ch_cap = sys_cap_self_slot(CAP_OBJ_CHANNEL, 0);
    uint8_t msg_buf[KERN_CH_MSG_SIZE];
    uint32_t *msg = (uint32_t *)msg_buf;

    for (uint32_t i = 0; i < sizeof(msg_buf); i++) {
        msg_buf[i] = 0;
    }

    *msg = 0x43485331U;
    int err = sys_ch_send(ch_cap, msg_buf, 1000);
    if (err == KERN_OK) {
        *msg = 0x43485332U;
        err = sys_ch_send(ch_cap, msg_buf, 1000);
    }
    if (err == KERN_OK) {
        (void)sys_task_delay(20);
    }

    sys_task_exit((void *)(intptr_t)err);
}

static void user_channel_send_caps_task(void *arg) {
    (void)arg;
    int ch_cap = sys_cap_self_slot(CAP_OBJ_CHANNEL, 0);
    cap_id_t src_cap = sys_cap_self_slot(CAP_OBJ_ENDPOINT, 0);
    uint8_t msg_buf[KERN_CH_MSG_SIZE];
    uint32_t *msg = (uint32_t *)msg_buf;
    ipc_cap_xfer_t xfers[IPC_CAPS_MAX];

    for (uint32_t i = 0; i < sizeof(msg_buf); i++) {
        msg_buf[i] = 0;
    }
    for (uint32_t i = 0; i < IPC_CAPS_MAX; i++) {
        xfers[i].src_cap = KERN_INVALID_ID;
        xfers[i].rights = 0;
        xfers[i].flags = IPC_CAP_COPY;
    }

    *msg = 0x43484353U;
    xfers[0].src_cap = src_cap;
    xfers[0].rights = CAP_READ;
    xfers[0].flags = IPC_CAP_COPY;

    int err = sys_ch_send_caps(ch_cap, msg_buf, xfers, 1, 1000);
    sys_task_exit((void *)(intptr_t)err);
}

static void user_channel_recv_caps_task(void *arg) {
    (void)arg;
    int ch_cap = sys_cap_self_slot(CAP_OBJ_CHANNEL, 0);
    uint8_t msg_buf[KERN_CH_MSG_SIZE];
    uint32_t *msg = (uint32_t *)msg_buf;
    cap_id_t caps[IPC_CAPS_MAX];
    uint8_t cap_count = 0;

    for (uint32_t i = 0; i < sizeof(msg_buf); i++) {
        msg_buf[i] = 0;
    }
    for (uint32_t i = 0; i < IPC_CAPS_MAX; i++) {
        caps[i] = KERN_INVALID_ID;
    }

    int err = sys_ch_recv_caps(ch_cap, msg_buf, caps, &cap_count, 1000);
    if (err == KERN_OK && *msg != 0x43485243U) {
        err = KERN_ERR_STATE;
    }
    if (err == KERN_OK && cap_count != 1) {
        err = KERN_ERR_RESOURCE;
    }
    if (err == KERN_OK) {
        err = sys_sem_post(caps[0]);
    }

    sys_task_exit((void *)(intptr_t)err);
}
#endif

static void test_user_endpoint_service_nonblocking(void) {
    test_section("Test 9: User endpoint service sleepable recv");

#if MPU_ENABLE && IPC_ENDPOINT && CAP_ENABLE
    ep_id_t ep = endpoint_create("u_svc", sizeof(uint32_t), 2);
    TEST_ASSERT(ep >= 0, "service endpoint created");
    if (ep < 0) return;

    cap_id_t ep_cap = cap_create(endpoint_obj_for_cap(ep), CAP_OBJ_ENDPOINT, CAP_FULL, 0);
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

static void test_user_endpoint_reply_cap(void) {
    test_section("Test 10: User endpoint reply cap");

#if MPU_ENABLE && IPC_ENDPOINT && CAP_ENABLE
    ep_id_t ep = endpoint_create("u_r cap", sizeof(uint32_t), 2);
    TEST_ASSERT(ep >= 0, "reply-cap endpoint created");
    if (ep < 0) return;

    cap_id_t ep_cap = cap_create(endpoint_obj_for_cap(ep), CAP_OBJ_ENDPOINT, CAP_FULL, 0);
    TEST_ASSERT(ep_cap >= 0, "reply-cap endpoint cap created");
    if (ep_cap < 0) {
        endpoint_delete(ep);
        return;
    }

    task_id_t service = task_create_user("u_ep_rcap",
                                         user_endpoint_reply_cap_service_task,
                                         (void *)(uintptr_t)ep_cap,
                                         5, 768);
    TEST_ASSERT(service >= 0, "reply-cap service created");
    if (service < 0) {
        cap_delete(ep_cap);
        endpoint_delete(ep);
        return;
    }

    kern_err_t err = cap_transfer(ep_cap, (uint8_t)service);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "reply-cap endpoint cap transferred");
    if (err != KERN_OK) {
        (void)task_delete(service);
        endpoint_delete(ep);
        return;
    }

    task_start(service);

    uint32_t msg = 31;
    err = endpoint_send(ep, &msg, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "kernel client reply-cap send OK");
    TEST_ASSERT_EQ(231, (int)msg, "user service replied through reply cap");

    void *retval = NULL;
    kern_err_t join_err = task_join(service, &retval, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)join_err, "reply-cap service joined OK");
    int rv = (int)(uintptr_t)retval;
    TEST_ASSERT((rv & 0xF000) != 0x1000, "reply cap service recv stage OK");
    TEST_ASSERT(rv != 0x2100, "reply cap endpoint cap resolve OK");
    TEST_ASSERT(rv != 0x2200, "reply cap endpoint binding exists");
    TEST_ASSERT((rv & 0xF000) != 0x2000, "reply cap service take stage OK");
    TEST_ASSERT((rv & 0xF000) != 0x3000, "reply cap service reply stage OK");
    TEST_ASSERT((rv & 0xF000) != 0x4000, "reply cap service single-use stage OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)(uintptr_t)retval,
                   "reply cap syscall result OK");

    endpoint_delete(ep);
#else
    test_skip("MPU, endpoint, or capability disabled");
#endif
}

static void test_user_endpoint_reply_cap_timeout(void) {
    test_section("Test 10b: User endpoint reply cap timeout invalidation");

#if MPU_ENABLE && IPC_ENDPOINT && CAP_ENABLE
    ep_id_t ep = endpoint_create("u_rcap_to", sizeof(uint32_t), 2);
    TEST_ASSERT(ep >= 0, "reply-cap-timeout endpoint created");
    if (ep < 0) return;

    cap_id_t ep_cap = cap_create(endpoint_obj_for_cap(ep), CAP_OBJ_ENDPOINT, CAP_FULL, 0);
    TEST_ASSERT(ep_cap >= 0, "reply-cap-timeout endpoint cap created");
    if (ep_cap < 0) {
        endpoint_delete(ep);
        return;
    }

    task_id_t service = task_create_user("u_ep_rcto",
                                         user_endpoint_reply_cap_timeout_service_task,
                                         (void *)(uintptr_t)ep_cap,
                                         5, 768);
    TEST_ASSERT(service >= 0, "reply-cap-timeout service created");
    if (service < 0) {
        cap_delete(ep_cap);
        endpoint_delete(ep);
        return;
    }

    kern_err_t err = cap_transfer(ep_cap, (uint8_t)service);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "reply-cap-timeout endpoint cap transferred");
    if (err != KERN_OK) {
        (void)task_delete(service);
        endpoint_delete(ep);
        return;
    }

    task_start(service);

    uint32_t msg = 41;
    err = endpoint_send(ep, &msg, 3);
    TEST_ASSERT_EQ((int)KERN_ERR_TIMEOUT, (int)err,
                   "reply cap client timed out");
    TEST_ASSERT_EQ(41, (int)msg, "timed-out client buffer unchanged");

    void *retval = NULL;
    kern_err_t join_err = task_join(service, &retval, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)join_err,
                   "reply-cap-timeout service joined OK");
    int rv = (int)(uintptr_t)retval;
    TEST_ASSERT((rv & 0xF000) != 0x1000,
                "reply cap timeout service recv stage OK");
    TEST_ASSERT((rv & 0xF000) != 0x2000,
                "reply cap timeout service take stage OK");
    TEST_ASSERT((rv & 0xF000) != 0x3000,
                "reply cap timeout service delay stage OK");
    TEST_ASSERT(rv != 0x4000, "reply cap invalid after client timeout");
    TEST_ASSERT_EQ((int)KERN_OK, rv,
                   "reply cap timeout service result OK");

    endpoint_delete(ep);
#else
    test_skip("MPU, endpoint, or capability disabled");
#endif
}

static void test_user_endpoint_recv_sleep_timeout(void) {
    test_section("Test 11: User endpoint recv sleep timeout");

#if MPU_ENABLE && IPC_ENDPOINT && CAP_ENABLE
    ep_id_t ep = endpoint_create("u_svc_to", sizeof(uint32_t), 1);
    TEST_ASSERT(ep >= 0, "timeout endpoint created");
    if (ep < 0) return;

    cap_id_t ep_cap = cap_create(endpoint_obj_for_cap(ep), CAP_OBJ_ENDPOINT, CAP_FULL, 0);
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

    cap_id_t ep_cap = cap_create(endpoint_obj_for_cap(ep), CAP_OBJ_ENDPOINT, CAP_FULL, 0);
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

static void test_user_endpoint_send_caps_sleepable(void) {
    test_section("Test 11b: User endpoint send_caps sleepable");

#if MPU_ENABLE && IPC_ENDPOINT && CAP_ENABLE
    ep_id_t ep = endpoint_create("u_caps", sizeof(uint32_t), 2);
    TEST_ASSERT(ep >= 0, "send_caps endpoint created");
    if (ep < 0) return;

    cap_id_t ep_cap = cap_create(endpoint_obj_for_cap(ep), CAP_OBJ_ENDPOINT, CAP_FULL, 0);
    TEST_ASSERT(ep_cap >= 0, "send_caps endpoint cap created");
    if (ep_cap < 0) {
        endpoint_delete(ep);
        return;
    }

    test_cap_obj_t cap_object;
    TEST_CAP_OBJ_INIT(&cap_object, CAP_OBJ_ENDPOINT);
    cap_id_t src_cap = cap_create(&cap_object, CAP_OBJ_ENDPOINT,
                                  CAP_FULL, 0);
    TEST_ASSERT(src_cap >= 0, "send_caps source cap created");
    if (src_cap < 0) {
        cap_delete(ep_cap);
        endpoint_delete(ep);
        return;
    }

    task_id_t client = task_create_user("u_ep_caps",
                                        user_endpoint_send_caps_task,
                                        NULL,
                                        5, 768);
    TEST_ASSERT(client >= 0, "send_caps user client created");
    if (client < 0) {
        cap_delete(src_cap);
        cap_delete(ep_cap);
        endpoint_delete(ep);
        return;
    }

    kern_err_t err = cap_transfer(ep_cap, (uint8_t)client);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "send_caps endpoint cap transferred");
    if (err != KERN_OK) {
        (void)task_delete(client);
        cap_delete(src_cap);
        endpoint_delete(ep);
        return;
    }

    err = cap_transfer(src_cap, (uint8_t)client);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "send_caps source cap transferred");
    if (err != KERN_OK) {
        (void)task_delete(client);
        endpoint_delete(ep);
        return;
    }

    task_start(client);

    uint32_t msg = 0;
    cap_id_t caps[IPC_CAPS_MAX];
    uint8_t cap_count = 0;
    for (uint32_t i = 0; i < IPC_CAPS_MAX; i++) {
        caps[i] = KERN_INVALID_ID;
    }

    err = endpoint_recv_caps(ep, &msg, caps, &cap_count, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "kernel received user send_caps request");
    TEST_ASSERT_EQ(55, (int)msg, "kernel received send_caps payload");
    TEST_ASSERT_EQ(1, (int)cap_count, "kernel received one copied cap");
    void *ptr = cap_resolve(caps[0], CAP_OBJ_ENDPOINT, CAP_READ);
    TEST_ASSERT(ptr == &cap_object, "kernel received copied endpoint cap");

    msg += 1;
    err = endpoint_reply(ep, &msg);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "kernel replied to send_caps client");

    void *retval = NULL;
    kern_err_t join_err = task_join(client, &retval, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)join_err, "send_caps client joined OK");
    int rv = (int)(uintptr_t)retval;
    TEST_ASSERT_EQ((int)KERN_OK, rv,
                   "sleepable ep_send_caps returned OK");

    if (caps[0] >= 0) {
        cap_delete(caps[0]);
    }
    endpoint_delete(ep);
#else
    test_skip("MPU, endpoint, or capability disabled");
#endif
}

static void test_user_endpoint_recv_caps_sleepable(void) {
    test_section("Test 11c: User endpoint recv_caps sleepable");

#if MPU_ENABLE && IPC_ENDPOINT && CAP_ENABLE
    ep_id_t ep = endpoint_create("u_rcaps", sizeof(uint32_t), 2);
    TEST_ASSERT(ep >= 0, "recv_caps endpoint created");
    if (ep < 0) return;

    cap_id_t ep_cap = cap_create(endpoint_obj_for_cap(ep), CAP_OBJ_ENDPOINT, CAP_FULL, 0);
    TEST_ASSERT(ep_cap >= 0, "recv_caps endpoint cap created");
    if (ep_cap < 0) {
        endpoint_delete(ep);
        return;
    }

    sem_id_t sem = sem_create(0, 1);
    TEST_ASSERT(sem >= 0, "recv_caps semaphore created");
    if (sem < 0) {
        cap_delete(ep_cap);
        endpoint_delete(ep);
        return;
    }

    cap_id_t sem_cap = cap_create_for(NULL, sem_obj_for_cap(sem), CAP_OBJ_SEMAPHORE,
                                  CAP_WRITE | CAP_TRANSFER);
    TEST_ASSERT(sem_cap >= 0, "recv_caps semaphore cap created");
    if (sem_cap < 0) {
        sem_delete(sem);
        cap_delete(ep_cap);
        endpoint_delete(ep);
        return;
    }

    task_id_t service = task_create_user("u_ep_rcaps",
                                         user_endpoint_recv_caps_service_task,
                                         (void *)(uintptr_t)ep_cap,
                                         5, 768);
    TEST_ASSERT(service >= 0, "recv_caps service created");
    if (service < 0) {
        cap_delete(sem_cap);
        sem_delete(sem);
        cap_delete(ep_cap);
        endpoint_delete(ep);
        return;
    }

    kern_err_t err = cap_transfer(ep_cap, (uint8_t)service);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "recv_caps endpoint cap transferred");
    if (err != KERN_OK) {
        (void)task_delete(service);
        cap_delete(sem_cap);
        sem_delete(sem);
        endpoint_delete(ep);
        return;
    }

    task_start(service);

    ipc_cap_xfer_t xfer;
    xfer.src_cap = sem_cap;
    xfer.rights = CAP_WRITE;
    xfer.flags = IPC_CAP_COPY;

    uint32_t msg = 70;
    err = endpoint_send_caps(ep, &msg, &xfer, 1, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "kernel endpoint_send_caps to user recv_caps OK");
    TEST_ASSERT_EQ(72, (int)msg, "user recv_caps service replied");

    err = sem_wait(sem, 0);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "user recv_caps service used transferred sem cap");

    void *retval = NULL;
    kern_err_t join_err = task_join(service, &retval, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)join_err, "recv_caps service joined OK");
    int rv = (int)(uintptr_t)retval;
    TEST_ASSERT_EQ((int)KERN_OK, rv,
                   "sleepable ep_recv_caps service result OK");

    cap_delete(sem_cap);
    sem_delete(sem);
    endpoint_delete(ep);
#else
    test_skip("MPU, endpoint, semaphore, or capability disabled");
#endif
}

static void test_user_endpoint_recv_mem_cap_sleepable(void) {
    test_section("Test 11d: User endpoint recv memory cap sleepable");

#if MPU_ENABLE && IPC_ENDPOINT && CAP_ENABLE && MEM_DYNAMIC
    ep_id_t ep = endpoint_create("u_rmem", sizeof(uint32_t), 2);
    TEST_ASSERT(ep >= 0, "recv mem cap endpoint created");
    if (ep < 0) return;

    cap_id_t ep_cap = cap_create(endpoint_obj_for_cap(ep), CAP_OBJ_ENDPOINT, CAP_FULL, 0);
    TEST_ASSERT(ep_cap >= 0, "recv mem cap endpoint cap created");
    if (ep_cap < 0) {
        endpoint_delete(ep);
        return;
    }

    cap_id_t mem_cap = kmem_alloc_cap(48,
                                      CAP_READ | CAP_WRITE |
                                      CAP_TRANSFER | CAP_MANAGE);
    TEST_ASSERT(mem_cap >= 0, "recv mem cap source created");
    if (mem_cap < 0) {
        cap_delete(ep_cap);
        endpoint_delete(ep);
        return;
    }

    task_id_t service = task_create_user("u_ep_rmem",
                                         user_endpoint_recv_mem_cap_task,
                                         (void *)(uintptr_t)ep_cap,
                                         5, 768);
    TEST_ASSERT(service >= 0, "recv mem cap service created");
    if (service < 0) {
        kmem_free_cap(mem_cap);
        cap_delete(ep_cap);
        endpoint_delete(ep);
        return;
    }

    kern_err_t err = cap_transfer(ep_cap, (uint8_t)service);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "recv mem cap endpoint cap transferred");
    if (err != KERN_OK) {
        (void)task_delete(service);
        kmem_free_cap(mem_cap);
        endpoint_delete(ep);
        return;
    }

    task_start(service);

    ipc_cap_xfer_t xfer;
    xfer.src_cap = mem_cap;
    xfer.rights = CAP_READ;
    xfer.flags = IPC_CAP_COPY;

    uint32_t msg = 80;
    err = endpoint_send_caps(ep, &msg, &xfer, 1, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "kernel endpoint_send_caps memory cap OK");
    TEST_ASSERT_EQ(83, (int)msg, "user recv memory cap service replied");

    void *retval = NULL;
    kern_err_t join_err = task_join(service, &retval, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)join_err,
                   "recv memory cap service joined OK");
    int rv = (int)(uintptr_t)retval;
    TEST_ASSERT_EQ((int)KERN_OK, rv,
                   "sleepable ep_recv_caps memory cap service result OK");

    void *base = NULL;
    size_t size = 0;
    err = kmem_get_bounds(mem_cap, &base, &size);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "original memory cap still resolves after IPC copy");
    TEST_ASSERT_EQ(48, (int)size, "original memory cap size preserved");

    kmem_free_cap(mem_cap);
    endpoint_delete(ep);
#else
    test_skip("MPU, endpoint, capability, or dynamic memory disabled");
#endif
}

static void test_user_endpoint_send_sleep_timeout(void) {
    test_section("Test 12: User endpoint send sleep timeout");

#if MPU_ENABLE && IPC_ENDPOINT && CAP_ENABLE
    ep_id_t ep = endpoint_create("u_cli_to", sizeof(uint32_t), 1);
    TEST_ASSERT(ep >= 0, "send-timeout endpoint created");
    if (ep < 0) return;

    cap_id_t ep_cap = cap_create(endpoint_obj_for_cap(ep), CAP_OBJ_ENDPOINT, CAP_FULL, 0);
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

    cap_id_t ep_cap = cap_create(endpoint_obj_for_cap(ep), CAP_OBJ_ENDPOINT, CAP_FULL, 0);
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
    /* Do not use a one-tick timing guess here: on SMP the test runner and the
     * new user task may be dispatched on different loaded cores.  Delete only
     * after the syscall has actually entered its sleepable wait state. */
    for (uint32_t wait = 0;
         wait < 100U && task_get_state(client) != TASK_STATE_BLOCKED;
         wait++) {
        task_delay(1);
    }

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

    cap_id_t ep_cap = cap_create(endpoint_obj_for_cap(ep), CAP_OBJ_ENDPOINT, CAP_FULL, 0);
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

static void test_user_sem_wait_sleepable(void) {
    test_section("Test 16: User semaphore wait sleepable");

#if MPU_ENABLE && CAP_ENABLE
    sem_id_t sem = sem_create(0, 1);
    TEST_ASSERT(sem >= 0, "sleepable semaphore created");
    if (sem < 0) return;

    cap_id_t sem_cap = cap_create_for(NULL, sem_obj_for_cap(sem), CAP_OBJ_SEMAPHORE, CAP_FULL);
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

    cap_id_t sem_cap = cap_create_for(NULL, sem_obj_for_cap(sem), CAP_OBJ_SEMAPHORE, CAP_FULL);
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

static void test_user_sem_wait_delete_wakeup(void) {
    test_section("Test 18: User semaphore wait delete wakeup");

#if MPU_ENABLE && CAP_ENABLE
    sem_id_t sem = sem_create(0, 1);
    TEST_ASSERT(sem >= 0, "delete-wakeup semaphore created");
    if (sem < 0) return;

    cap_id_t sem_cap = cap_create_for(NULL, sem_obj_for_cap(sem), CAP_OBJ_SEMAPHORE, CAP_FULL);
    TEST_ASSERT(sem_cap >= 0, "delete-wakeup semaphore cap created");
    if (sem_cap < 0) {
        sem_delete(sem);
        return;
    }

    task_id_t waiter = task_create_user("u_sem_del",
                                        user_sem_wait_task,
                                        (void *)(uintptr_t)sem_cap,
                                        5, 512);
    TEST_ASSERT(waiter >= 0, "delete-wakeup semaphore waiter created");
    if (waiter < 0) {
        cap_delete(sem_cap);
        sem_delete(sem);
        return;
    }

    kern_err_t err = cap_transfer(sem_cap, (uint8_t)waiter);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "delete-wakeup sem cap transferred");
    if (err != KERN_OK) {
        (void)task_delete(waiter);
        sem_delete(sem);
        return;
    }

    task_start(waiter);
    /* SMP 下 1 tick 不足以保证 waiter 先阻塞 (尤其 core1 忙时):
     * 轮询等到真正 BLOCKED 再删除,消除"删先于等"的测试竞态
     * (否则 waiter 的 SVC 命中已回收的 cap → PARAM 而非 NOEXIST)。 */
    for (int i = 0; i < 100 &&
                task_get_state(waiter) != TASK_STATE_BLOCKED; i++) {
        task_delay(1);
    }

    err = sem_delete(sem);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "sem_delete woke syscall waiter");

    void *retval = NULL;
    kern_err_t join_err = task_join(waiter, &retval, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)join_err, "sem delete waiter joined OK");
    TEST_ASSERT_EQ((int)KERN_ERR_NOEXIST, (int)(intptr_t)retval,
                   "sleepable sem wait returned noexist after delete");
#else
    test_skip("MPU or capability disabled");
#endif
}

static void test_user_mutex_lock_sleepable(void) {
    test_section("Test 19: User mutex lock sleepable");

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

    cap_id_t mutex_cap = cap_create_for(NULL, mutex_obj_for_cap(mid), CAP_OBJ_MUTEX, CAP_FULL);
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
    test_section("Test 20: User mutex lock sleep timeout");

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

    cap_id_t mutex_cap = cap_create_for(NULL, mutex_obj_for_cap(mid), CAP_OBJ_MUTEX, CAP_FULL);
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

    cap_id_t mq_cap = cap_create_for(NULL, mqueue_obj_for_cap(mq), CAP_OBJ_MQUEUE, CAP_FULL);
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

    cap_id_t mq_cap = cap_create_for(NULL, mqueue_obj_for_cap(mq), CAP_OBJ_MQUEUE, CAP_FULL);
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

    cap_id_t mq_cap = cap_create_for(NULL, mqueue_obj_for_cap(mq), CAP_OBJ_MQUEUE, CAP_FULL);
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

static void test_user_event_wait_sleepable(void) {
    test_section("Test 23: User event wait sleepable");

#if MPU_ENABLE && CAP_ENABLE
    event_id_t eid = event_create(0);
    TEST_ASSERT(eid >= 0, "sleepable event created");
    if (eid < 0) return;

    cap_id_t event_cap = cap_create_for(NULL, event_obj_for_cap(eid), CAP_OBJ_EVENT, CAP_FULL);
    TEST_ASSERT(event_cap >= 0, "sleepable event cap created");
    if (event_cap < 0) {
        event_delete(eid);
        return;
    }

    task_id_t waiter = task_create_user("u_evt_wait",
                                        user_event_wait_task,
                                        (void *)(uintptr_t)event_cap,
                                        5, 512);
    TEST_ASSERT(waiter >= 0, "sleepable event waiter created");
    if (waiter < 0) {
        cap_delete(event_cap);
        event_delete(eid);
        return;
    }

    kern_err_t err = cap_transfer(event_cap, (uint8_t)waiter);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "sleepable event cap transferred");
    if (err != KERN_OK) {
        (void)task_delete(waiter);
        event_delete(eid);
        return;
    }

    task_start(waiter);
    task_delay(1);

    err = event_set(eid, 0x4U);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "kernel event_set woke syscall");

    void *retval = NULL;
    kern_err_t join_err = task_join(waiter, &retval, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)join_err, "event waiter joined OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                   "sleepable event wait returned OK");

    event_delete(eid);
#else
    test_skip("MPU or capability disabled");
#endif
}

static void test_user_event_wait_sleep_timeout(void) {
    test_section("Test 24: User event wait sleep timeout");

#if MPU_ENABLE && CAP_ENABLE
    event_id_t eid = event_create(0);
    TEST_ASSERT(eid >= 0, "timeout event created");
    if (eid < 0) return;

    cap_id_t event_cap = cap_create_for(NULL, event_obj_for_cap(eid), CAP_OBJ_EVENT, CAP_FULL);
    TEST_ASSERT(event_cap >= 0, "timeout event cap created");
    if (event_cap < 0) {
        event_delete(eid);
        return;
    }

    task_id_t waiter = task_create_user("u_evt_to",
                                        user_event_wait_timeout_task,
                                        (void *)(uintptr_t)event_cap,
                                        5, 512);
    TEST_ASSERT(waiter >= 0, "timeout event waiter created");
    if (waiter < 0) {
        cap_delete(event_cap);
        event_delete(eid);
        return;
    }

    kern_err_t err = cap_transfer(event_cap, (uint8_t)waiter);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "timeout event cap transferred");
    if (err != KERN_OK) {
        (void)task_delete(waiter);
        event_delete(eid);
        return;
    }

    task_start(waiter);

    void *retval = NULL;
    kern_err_t join_err = task_join(waiter, &retval, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)join_err,
                   "event timeout waiter joined OK");
    TEST_ASSERT_EQ((int)KERN_ERR_TIMEOUT, (int)(intptr_t)retval,
                   "sleepable event wait returned timeout");

    event_delete(eid);
#else
    test_skip("MPU or capability disabled");
#endif
}

static void test_user_event_wait_delete_wakeup(void) {
    test_section("Test 25: User event wait delete wakeup");

#if MPU_ENABLE && CAP_ENABLE
    event_id_t eid = event_create(0);
    TEST_ASSERT(eid >= 0, "delete-wakeup event created");
    if (eid < 0) return;

    cap_id_t event_cap = cap_create_for(NULL, event_obj_for_cap(eid), CAP_OBJ_EVENT, CAP_FULL);
    TEST_ASSERT(event_cap >= 0, "delete-wakeup event cap created");
    if (event_cap < 0) {
        event_delete(eid);
        return;
    }

    task_id_t waiter = task_create_user("u_evt_del",
                                        user_event_wait_task,
                                        (void *)(uintptr_t)event_cap,
                                        5, 512);
    TEST_ASSERT(waiter >= 0, "delete-wakeup event waiter created");
    if (waiter < 0) {
        cap_delete(event_cap);
        event_delete(eid);
        return;
    }

    kern_err_t err = cap_transfer(event_cap, (uint8_t)waiter);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "delete-wakeup event cap transferred");
    if (err != KERN_OK) {
        (void)task_delete(waiter);
        event_delete(eid);
        return;
    }

    task_start(waiter);
    /* SMP 下 1 tick 不足以保证 waiter 先阻塞 (尤其 core1 忙时):
     * 轮询等到真正 BLOCKED 再删除,消除"删先于等"的测试竞态
     * (否则 waiter 的 SVC 命中已回收的 cap → PARAM 而非 NOEXIST)。 */
    for (int i = 0; i < 100 &&
                task_get_state(waiter) != TASK_STATE_BLOCKED; i++) {
        task_delay(1);
    }

    err = event_delete(eid);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "event_delete woke syscall waiter");

    void *retval = NULL;
    kern_err_t join_err = task_join(waiter, &retval, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)join_err, "event delete waiter joined OK");
    TEST_ASSERT_EQ((int)KERN_ERR_NOEXIST, (int)(intptr_t)retval,
                   "sleepable event wait returned noexist after delete");
#else
    test_skip("MPU or capability disabled");
#endif
}

static void test_user_channel_recv_sleepable(void) {
    test_section("Test 26: User channel recv sleepable");

#if MPU_ENABLE && CAP_ENABLE && IPC_CHANNEL
    ch_id_t ch = channel_create(sizeof(uint32_t), 0);
    TEST_ASSERT(ch >= 0, "sleepable channel created");
    if (ch < 0) return;

    cap_id_t ch_cap = cap_create(channel_obj_for_cap(ch), CAP_OBJ_CHANNEL, CAP_FULL, 0);
    TEST_ASSERT(ch_cap >= 0, "sleepable channel cap created");
    if (ch_cap < 0) {
        channel_delete(ch);
        return;
    }

    task_id_t waiter = task_create_user("u_ch_recv",
                                        user_channel_recv_task,
                                        (void *)(uintptr_t)ch_cap,
                                        5, 512);
    TEST_ASSERT(waiter >= 0, "sleepable channel recv task created");
    if (waiter < 0) {
        cap_delete(ch_cap);
        channel_delete(ch);
        return;
    }

    task_id_t self = task_self();
    kern_err_t err = channel_connect(ch, waiter, self);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "sleepable channel connected");
    if (err != KERN_OK) {
        (void)task_delete(waiter);
        channel_delete(ch);
        return;
    }

    err = cap_transfer(ch_cap, (uint8_t)waiter);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "sleepable channel cap transferred");
    if (err != KERN_OK) {
        (void)task_delete(waiter);
        channel_delete(ch);
        return;
    }

    task_start(waiter);
    task_delay(1);

    uint32_t msg = 0x43485258U;
    err = channel_send(ch, &msg, 0);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "kernel send woke channel recv syscall");

    void *retval = NULL;
    kern_err_t join_err = task_join(waiter, &retval, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)join_err, "channel recv waiter joined OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                   "sleepable channel recv returned OK");

    channel_delete(ch);
#else
    test_skip("MPU, capability, or channel disabled");
#endif
}

static void test_user_channel_recv_sleep_timeout(void) {
    test_section("Test 27: User channel recv sleep timeout");

#if MPU_ENABLE && CAP_ENABLE && IPC_CHANNEL
    ch_id_t ch = channel_create(sizeof(uint32_t), 0);
    TEST_ASSERT(ch >= 0, "timeout channel created");
    if (ch < 0) return;

    cap_id_t ch_cap = cap_create(channel_obj_for_cap(ch), CAP_OBJ_CHANNEL, CAP_FULL, 0);
    TEST_ASSERT(ch_cap >= 0, "timeout channel cap created");
    if (ch_cap < 0) {
        channel_delete(ch);
        return;
    }

    task_id_t waiter = task_create_user("u_ch_to",
                                        user_channel_recv_timeout_task,
                                        (void *)(uintptr_t)ch_cap,
                                        5, 512);
    TEST_ASSERT(waiter >= 0, "timeout channel recv task created");
    if (waiter < 0) {
        cap_delete(ch_cap);
        channel_delete(ch);
        return;
    }

    task_id_t self = task_self();
    kern_err_t err = channel_connect(ch, waiter, self);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "timeout channel connected");
    if (err != KERN_OK) {
        (void)task_delete(waiter);
        channel_delete(ch);
        return;
    }

    err = cap_transfer(ch_cap, (uint8_t)waiter);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "timeout channel cap transferred");
    if (err != KERN_OK) {
        (void)task_delete(waiter);
        channel_delete(ch);
        return;
    }

    task_start(waiter);

    void *retval = NULL;
    kern_err_t join_err = task_join(waiter, &retval, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)join_err,
                   "channel timeout waiter joined OK");
    TEST_ASSERT_EQ((int)KERN_ERR_TIMEOUT, (int)(intptr_t)retval,
                   "sleepable channel recv returned timeout");

    channel_delete(ch);
#else
    test_skip("MPU, capability, or channel disabled");
#endif
}

static void test_user_channel_recv_delete_wakeup(void) {
    test_section("Test 28: User channel recv delete wakeup");

#if MPU_ENABLE && CAP_ENABLE && IPC_CHANNEL
    ch_id_t ch = channel_create(sizeof(uint32_t), 0);
    TEST_ASSERT(ch >= 0, "delete-wakeup channel created");
    if (ch < 0) return;

    cap_id_t ch_cap = cap_create(channel_obj_for_cap(ch), CAP_OBJ_CHANNEL, CAP_FULL, 0);
    TEST_ASSERT(ch_cap >= 0, "delete-wakeup channel cap created");
    if (ch_cap < 0) {
        channel_delete(ch);
        return;
    }

    task_id_t waiter = task_create_user("u_ch_del",
                                        user_channel_recv_task,
                                        (void *)(uintptr_t)ch_cap,
                                        5, 512);
    TEST_ASSERT(waiter >= 0, "delete-wakeup channel waiter created");
    if (waiter < 0) {
        cap_delete(ch_cap);
        channel_delete(ch);
        return;
    }

    task_id_t self = task_self();
    kern_err_t err = channel_connect(ch, waiter, self);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "delete-wakeup channel connected");
    if (err != KERN_OK) {
        (void)task_delete(waiter);
        channel_delete(ch);
        return;
    }

    err = cap_transfer(ch_cap, (uint8_t)waiter);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "delete-wakeup channel cap transferred");
    if (err != KERN_OK) {
        (void)task_delete(waiter);
        channel_delete(ch);
        return;
    }

    task_start(waiter);
    /* SMP 下 1 tick 不足以保证 waiter 先阻塞 (尤其 core1 忙时):
     * 轮询等到真正 BLOCKED 再删除,消除"删先于等"的测试竞态
     * (否则 waiter 的 SVC 命中已回收的 cap → PARAM 而非 NOEXIST)。 */
    for (int i = 0; i < 100 &&
                task_get_state(waiter) != TASK_STATE_BLOCKED; i++) {
        task_delay(1);
    }

    err = channel_delete(ch);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "channel_delete woke syscall waiter");

    void *retval = NULL;
    kern_err_t join_err = task_join(waiter, &retval, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)join_err, "channel delete waiter joined OK");
    TEST_ASSERT_EQ((int)KERN_ERR_NOEXIST, (int)(intptr_t)retval,
                   "sleepable channel recv returned noexist after delete");
#else
    test_skip("MPU, capability, or channel disabled");
#endif
}

static void test_user_channel_send_sleepable(void) {
    test_section("Test 29: User channel send sleepable");

#if MPU_ENABLE && CAP_ENABLE && IPC_CHANNEL
    ch_id_t ch = channel_create(sizeof(uint32_t), 0);
    TEST_ASSERT(ch >= 0, "send channel created");
    if (ch < 0) return;

    cap_id_t ch_cap = cap_create(channel_obj_for_cap(ch), CAP_OBJ_CHANNEL, CAP_FULL, 0);
    TEST_ASSERT(ch_cap >= 0, "send channel cap created");
    if (ch_cap < 0) {
        channel_delete(ch);
        return;
    }

    task_id_t sender = task_create_user("u_ch_send",
                                        user_channel_send_twice_task,
                                        (void *)(uintptr_t)ch_cap,
                                        5, 512);
    TEST_ASSERT(sender >= 0, "sleepable channel send task created");
    if (sender < 0) {
        cap_delete(ch_cap);
        channel_delete(ch);
        return;
    }

    task_id_t self = task_self();
    kern_err_t err = channel_connect(ch, sender, self);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "send channel connected");
    if (err != KERN_OK) {
        (void)task_delete(sender);
        channel_delete(ch);
        return;
    }

    err = cap_transfer(ch_cap, (uint8_t)sender);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "send channel cap transferred");
    if (err != KERN_OK) {
        (void)task_delete(sender);
        channel_delete(ch);
        return;
    }

    task_start(sender);

    uint32_t got = 0;
    /* A bounded blocking receive is the synchronization primitive being
     * tested.  delay(1)+NOWAIT was scheduler-speed dependent on dual core. */
    err = channel_recv(ch, &got, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "kernel recv opened channel slot");
    TEST_ASSERT_EQ((int)0x43485331U, (int)got,
                   "kernel received first channel msg");

    got = 0;
    err = channel_recv(ch, &got, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "kernel received second channel msg");
    TEST_ASSERT_EQ((int)0x43485332U, (int)got,
                   "sleepable channel send copied message");

    void *retval = NULL;
    kern_err_t join_err = task_join(sender, &retval, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)join_err, "channel sender joined OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                   "sleepable channel send returned OK");

    channel_delete(ch);
#else
    test_skip("MPU, capability, or channel disabled");
#endif
}

static void test_user_channel_send_caps_sleepable(void) {
    test_section("Test 30: User channel send_caps sleepable");

#if MPU_ENABLE && CAP_ENABLE && IPC_CHANNEL
    ch_id_t ch = channel_create(sizeof(uint32_t), 0);
    TEST_ASSERT(ch >= 0, "send_caps channel created");
    if (ch < 0) return;

    cap_id_t ch_cap = cap_create(channel_obj_for_cap(ch), CAP_OBJ_CHANNEL, CAP_FULL, 0);
    TEST_ASSERT(ch_cap >= 0, "send_caps channel cap created");
    if (ch_cap < 0) {
        channel_delete(ch);
        return;
    }

    test_cap_obj_t cap_object;
    TEST_CAP_OBJ_INIT(&cap_object, CAP_OBJ_CHANNEL);
    cap_id_t src_cap = cap_create(&cap_object, CAP_OBJ_ENDPOINT,
                                  CAP_FULL, 0);
    TEST_ASSERT(src_cap >= 0, "channel send_caps source cap created");
    if (src_cap < 0) {
        cap_delete(ch_cap);
        channel_delete(ch);
        return;
    }

    task_id_t sender = task_create_user("u_ch_caps",
                                        user_channel_send_caps_task,
                                        NULL,
                                        5, 768);
    TEST_ASSERT(sender >= 0, "channel send_caps user created");
    if (sender < 0) {
        cap_delete(src_cap);
        cap_delete(ch_cap);
        channel_delete(ch);
        return;
    }

    task_id_t self = task_self();
    kern_err_t err = channel_connect(ch, sender, self);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "send_caps channel connected");
    if (err != KERN_OK) {
        (void)task_delete(sender);
        cap_delete(src_cap);
        channel_delete(ch);
        return;
    }

    err = cap_transfer(ch_cap, (uint8_t)sender);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "send_caps channel cap transferred");
    if (err != KERN_OK) {
        (void)task_delete(sender);
        cap_delete(src_cap);
        channel_delete(ch);
        return;
    }

    err = cap_transfer(src_cap, (uint8_t)sender);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "channel source cap transferred");
    if (err != KERN_OK) {
        (void)task_delete(sender);
        channel_delete(ch);
        return;
    }

    task_start(sender);

    uint32_t msg = 0;
    cap_id_t caps[IPC_CAPS_MAX];
    uint8_t cap_count = 0;
    for (uint32_t i = 0; i < IPC_CAPS_MAX; i++) {
        caps[i] = KERN_INVALID_ID;
    }

    err = channel_recv_caps(ch, &msg, caps, &cap_count, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "kernel received user channel send_caps request");
    TEST_ASSERT_EQ((int)0x43484353U, (int)msg,
                   "kernel received channel send_caps payload");
    TEST_ASSERT_EQ(1, (int)cap_count,
                   "kernel received one channel copied cap");
    void *ptr = cap_resolve(caps[0], CAP_OBJ_ENDPOINT, CAP_READ);
    TEST_ASSERT(ptr == &cap_object,
                "kernel received copied cap from channel send_caps");

    void *retval = NULL;
    kern_err_t join_err = task_join(sender, &retval, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)join_err, "channel send_caps joined OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                   "sleepable ch_send_caps returned OK");

    if (caps[0] >= 0) {
        cap_delete(caps[0]);
    }
    channel_delete(ch);
#else
    test_skip("MPU, capability, or channel disabled");
#endif
}

static void test_user_channel_recv_caps_sleepable(void) {
    test_section("Test 31: User channel recv_caps sleepable");

#if MPU_ENABLE && CAP_ENABLE && IPC_CHANNEL
    ch_id_t ch = channel_create(sizeof(uint32_t), 0);
    TEST_ASSERT(ch >= 0, "recv_caps channel created");
    if (ch < 0) return;

    cap_id_t ch_cap = cap_create(channel_obj_for_cap(ch), CAP_OBJ_CHANNEL, CAP_FULL, 0);
    TEST_ASSERT(ch_cap >= 0, "recv_caps channel cap created");
    if (ch_cap < 0) {
        channel_delete(ch);
        return;
    }

    sem_id_t sem = sem_create(0, 1);
    TEST_ASSERT(sem >= 0, "channel recv_caps semaphore created");
    if (sem < 0) {
        cap_delete(ch_cap);
        channel_delete(ch);
        return;
    }

    cap_id_t sem_cap = cap_create_for(NULL, sem_obj_for_cap(sem), CAP_OBJ_SEMAPHORE,
                                  CAP_WRITE | CAP_TRANSFER);
    TEST_ASSERT(sem_cap >= 0, "channel recv_caps semaphore cap created");
    if (sem_cap < 0) {
        sem_delete(sem);
        cap_delete(ch_cap);
        channel_delete(ch);
        return;
    }

    task_id_t service = task_create_user("u_ch_rcaps",
                                         user_channel_recv_caps_task,
                                         (void *)(uintptr_t)ch_cap,
                                         5, 768);
    TEST_ASSERT(service >= 0, "channel recv_caps service created");
    if (service < 0) {
        cap_delete(sem_cap);
        sem_delete(sem);
        cap_delete(ch_cap);
        channel_delete(ch);
        return;
    }

    task_id_t self = task_self();
    kern_err_t err = channel_connect(ch, self, service);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "recv_caps channel connected");
    if (err != KERN_OK) {
        (void)task_delete(service);
        cap_delete(sem_cap);
        sem_delete(sem);
        channel_delete(ch);
        return;
    }

    err = cap_transfer(ch_cap, (uint8_t)service);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "recv_caps channel cap transferred");
    if (err != KERN_OK) {
        (void)task_delete(service);
        cap_delete(sem_cap);
        sem_delete(sem);
        channel_delete(ch);
        return;
    }

    task_start(service);
    task_delay(1);

    ipc_cap_xfer_t xfer;
    xfer.src_cap = sem_cap;
    xfer.rights = CAP_WRITE;
    xfer.flags = IPC_CAP_COPY;

    uint32_t msg = 0x43485243U;
    err = channel_send_caps(ch, &msg, &xfer, 1, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "kernel channel_send_caps to user recv_caps OK");

    void *retval = NULL;
    kern_err_t join_err = task_join(service, &retval, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)join_err,
                   "channel recv_caps service joined OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                   "sleepable ch_recv_caps returned OK");

    /* channel_send_caps is one-way: enqueue success does not mean the peer
     * has already run.  Joining establishes that sys_sem_post completed before
     * the nonblocking semaphore observation. */
    err = sem_wait(sem, 0);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "user channel recv_caps used transferred sem cap");

    cap_delete(sem_cap);
    sem_delete(sem);
    channel_delete(ch);
#else
    test_skip("MPU, capability, or channel disabled");
#endif
}

/*============================================================================
 * Syscall 用户契约测试模块入口
 *============================================================================*/

static void test_syscall_user_module(void) {
    test_user_local_cptr_lifecycle();
    test_syscall_security_negative();
    test_task_create_user_gate();
    test_user_timer_endpoint_notification();
    test_user_mem_cap_syscalls();
    test_user_shm_map_syscalls();
    test_user_shm_map_region_exhaustion();
    test_user_endpoint_service_nonblocking();
    test_user_endpoint_reply_cap();
    test_user_endpoint_reply_cap_timeout();
    test_user_endpoint_recv_sleep_timeout();
    test_user_endpoint_send_sleep_reply();
    test_user_endpoint_send_caps_sleepable();
    test_user_endpoint_recv_caps_sleepable();
    test_user_endpoint_recv_mem_cap_sleepable();
    test_user_endpoint_send_sleep_timeout();
    test_user_endpoint_send_sleep_delete();
    test_user_endpoint_send_nowait_timeout();
    test_user_sem_wait_sleepable();
    test_user_sem_wait_sleep_timeout();
    test_user_sem_wait_delete_wakeup();
    test_user_mutex_lock_sleepable();
    test_user_mutex_lock_sleep_timeout();
    test_user_mqueue_recv_sleepable();
    test_user_mqueue_recv_sleep_timeout();
    test_user_mqueue_send_sleepable();
    test_user_event_wait_sleepable();
    test_user_event_wait_sleep_timeout();
    test_user_event_wait_delete_wakeup();
    test_user_channel_recv_sleepable();
    test_user_channel_recv_sleep_timeout();
    test_user_channel_recv_delete_wakeup();
    test_user_channel_send_sleepable();
    test_user_channel_send_caps_sleepable();
    test_user_channel_recv_caps_sleepable();
}

TEST_ABI_MODULE(syscall_user, test_syscall_user_module);

#endif /* SYSCALL_ENABLE */
#endif /* TEST_ENABLE */
