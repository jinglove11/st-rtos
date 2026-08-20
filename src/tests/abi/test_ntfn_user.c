/**
 * @file test_ntfn_user.c
 * @brief P1-1 (A4) slice 2: notification 对象 — ABI 层用户任务契约
 *
 * 用户任务经 SVC 直呼 sys_ntfn_* 家族,固化对外契约:
 *   - create/mint/signal/poll/delete 全链(字位 = mint 徽章,非参数)
 *   - rights 契约:signal 需 CAP_WRITE,wait/poll 需 CAP_READ,delete 需 CAP_MANAGE
 *   - SVC 阻塞 wait 被跨上下文 signal 唤醒,字 copy 到用户内存
 *   - 用户→用户跨任务 signal(badge 随 cap 走)
 *   - 超时路径
 *
 * 与 k/test_notification.c(内核白盒,notification_wait 手动阻塞协议)
 * 互补:这里专测 SVC 两阶段 continuation 路径 + 用户指针 copyout。
 */

#include "test_framework.h"
#include "kernel.h"
#include "notification.h"
#include "task.h"
#include "capability.h"
#include "user_api.h"

#if TEST_ENABLE

#if IPC_NOTIFICATION && TEST_MODULE_NOTIFICATION && CAP_ENABLE

#include <stdint.h>

#define NTFN_USER_JOIN_TICKS 30000U

/*============================================================================
 * Case 1: 单用户任务全链(create/mint/signal/poll/delete)
 *============================================================================*/

static void user_ntfn_basic_task(void *arg) {
    (void)arg;

    int full = sys_ntfn_create();
    if (full < 0) {
        sys_task_exit((void *)(intptr_t)full);
    }

    /* 无徽章的 signal cap 是 no-op(word |= 0)——seL4 语义 */
    if (sys_ntfn_signal(full) != KERN_OK) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_STATE);
    }
    uint32_t w = 0xEEEEU;
    if (sys_ntfn_poll(full, &w) != KERN_OK || w != 0U) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_STATE);
    }

    /* mint 出带徽章 signal cap:字位来自徽章 */
    int sig = sys_cap_mint(full, CAP_WRITE, 0x11U);
    if (sig < 0) {
        sys_task_exit((void *)(intptr_t)sig);
    }
    if (sys_ntfn_signal(sig) != KERN_OK) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_STATE);
    }
    w = 0xEEEEU;
    if (sys_ntfn_poll(full, &w) != KERN_OK || w != 0x11U) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_STATE);
    }
    /* 消费后为空 */
    if (sys_ntfn_poll(full, &w) != KERN_OK || w != 0U) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_STATE);
    }

    /* delete 需 MANAGE;mint 出的子 cap 随对象吊销 */
    if (sys_ntfn_delete(full) != KERN_OK) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_STATE);
    }
    if (sys_ntfn_signal(sig) != KERN_ERR_CAP) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_STATE);
    }

    sys_task_exit((void *)(intptr_t)KERN_OK);
}

static void test_ntfn_user_basic(void) {
    test_section("ABI Test 1: user create/mint/signal/poll/delete chain");
    test_user_case("ntfn basic chain", user_ntfn_basic_task,
                   KERN_OK, 10000U);
}

/*============================================================================
 * Case 2: rights 契约(READ 不能 signal,WRITE 不能 wait/poll)
 *============================================================================*/

static void user_ntfn_rights_task(void *arg) {
    (void)arg;

    int full = sys_ntfn_create();
    if (full < 0) {
        sys_task_exit((void *)(intptr_t)full);
    }

    int rd = sys_cap_mint(full, CAP_READ, 0x1U);
    int wr = sys_cap_mint(full, CAP_WRITE, 0x2U);
    if (rd < 0 || wr < 0) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_RESOURCE);
    }

    uint32_t w = 0U;
    if (sys_ntfn_signal(rd) != KERN_ERR_CAP) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_STATE);
    }
    if (sys_ntfn_wait(wr, 1U, &w) != KERN_ERR_CAP) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_STATE);
    }
    if (sys_ntfn_poll(wr, &w) != KERN_ERR_CAP) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_STATE);
    }
    /* READ 权限可以 poll(fast path 读) */
    if (sys_ntfn_poll(rd, &w) != KERN_OK || w != 0U) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_STATE);
    }

    (void)sys_ntfn_delete(full);
    sys_task_exit((void *)(intptr_t)KERN_OK);
}

static void test_ntfn_user_rights(void) {
    test_section("ABI Test 2: signal=WRITE, wait/poll=READ rights");
    test_user_case("ntfn rights", user_ntfn_rights_task,
                   KERN_OK, 10000U);
}

/*============================================================================
 * Case 3: SVC 阻塞 wait 超时
 *============================================================================*/

static void user_ntfn_timeout_task(void *arg) {
    (void)arg;

    int full = sys_ntfn_create();
    if (full < 0) {
        sys_task_exit((void *)(intptr_t)full);
    }
    uint32_t w = 0xDEADU;
    kern_err_t r = sys_ntfn_wait(full, 5U, &w);
    if (r == KERN_ERR_TIMEOUT) {
        r = (kern_err_t)KERN_OK;
    }
    (void)sys_ntfn_delete(full);
    sys_task_exit((void *)(intptr_t)r);
}

static void test_ntfn_user_timeout(void) {
    test_section("ABI Test 3: user wait timeout via SVC");
    test_user_case("ntfn user timeout", user_ntfn_timeout_task,
                   KERN_OK, 10000U);
}

/*============================================================================
 * Case 4: SVC 阻塞 wait 被内核 signal 唤醒(用户内存 copyout)
 *============================================================================*/

static void user_ntfn_block_waiter(void *arg) {
    (void)arg;

    int cap = sys_cap_self_slot(CAP_OBJ_NOTIFICATION, 0);
    if (cap < 0) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_CAP);
    }
    uint32_t w = 0U;
    kern_err_t r = sys_ntfn_wait(cap, 100000U, &w);
    if (r != KERN_OK) {
        sys_task_exit((void *)(intptr_t)r);
    }
    sys_task_exit((void *)(intptr_t)(w == 0xA5A5U ? KERN_OK
                                                  : KERN_ERR_STATE));
}

static void test_ntfn_user_block_wake(void) {
    test_section("ABI Test 4: blocked user waiter woken by kernel signal");

    notification_id_t id = notification_create();
    TEST_ASSERT(id >= 0, "kernel-created ntfn");
    if (id < 0) return;

    task_id_t tid = task_create_user("ntfn_uwait", user_ntfn_block_waiter,
                                     NULL, 9, 1024);
    TEST_ASSERT(tid >= 0, "user waiter created");
    tcb_t *utcb = tid >= 0 ? task_get_tcb(tid) : NULL;
    if (tid < 0 || utcb == NULL) {
        (void)notification_delete(id);
        return;
    }
    cap_id_t cap = cap_create_for(utcb, notification_obj_for_cap(id),
                                  CAP_OBJ_NOTIFICATION,
                                  CAP_READ | CAP_WRITE | CAP_MANAGE);
    TEST_ASSERT(cap >= 0, "user waiter holds cap");
    if (cap < 0) {
        (void)task_delete(tid);
        (void)notification_delete(id);
        return;
    }
    (void)task_start(tid);

    for (int i = 0; i < 200 &&
         task_get_state(tid) != TASK_STATE_BLOCKED; i++) {
        (void)task_delay(1U);
    }
    TEST_ASSERT_EQ(TASK_STATE_BLOCKED, task_get_state(tid),
                   "user waiter blocked in SVC");

    TEST_ASSERT_EQ(KERN_OK, notification_signal(id, 0xA5A5U),
                   "kernel signal");

    uintptr_t retval = 0;
    kern_err_t jerr = task_join(tid, (void **)&retval, NTFN_USER_JOIN_TICKS);
    TEST_ASSERT_EQ(KERN_OK, jerr, "waiter joined");
    TEST_ASSERT_EQ((int32_t)KERN_OK, (int32_t)(intptr_t)retval,
                   "user got whole word in user memory");

    (void)task_delete(tid);
    TEST_ASSERT_EQ(KERN_OK, notification_delete(id), "cleanup");
}

/*============================================================================
 * Case 5: 用户→用户跨任务 signal(badge 随 signaler 的 cap 走)
 *============================================================================*/

static void user_ntfn_cross_waiter(void *arg) {
    (void)arg;

    int cap = sys_cap_self_slot(CAP_OBJ_NOTIFICATION, 0);
    if (cap < 0) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_CAP);
    }
    uint32_t w = 0U;
    kern_err_t r = sys_ntfn_wait(cap, 100000U, &w);
    if (r != KERN_OK) {
        sys_task_exit((void *)(intptr_t)r);
    }
    /* 字必须等于 signaler cap 的徽章 0x40,而非任何参数 */
    sys_task_exit((void *)(intptr_t)(w == 0x40U ? KERN_OK : KERN_ERR_STATE));
}

static void user_ntfn_cross_signaler(void *arg) {
    (void)arg;

    int cap = sys_cap_self_slot(CAP_OBJ_NOTIFICATION, 0);
    if (cap < 0) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_CAP);
    }
    kern_err_t r = sys_ntfn_signal(cap);
    sys_task_exit((void *)(intptr_t)r);
}

static void test_ntfn_user_cross_signal(void) {
    test_section("ABI Test 5: user-to-user signal, word = signaler badge");

    notification_id_t id = notification_create();
    TEST_ASSERT(id >= 0, "created");
    if (id < 0) return;

    task_id_t waiter = task_create_user("ntfn_cwait", user_ntfn_cross_waiter,
                                        NULL, 9, 1024);
    TEST_ASSERT(waiter >= 0, "cross waiter created");
    tcb_t *wtcb = waiter >= 0 ? task_get_tcb(waiter) : NULL;
    task_id_t signaler = task_create_user("ntfn_csig",
                                          user_ntfn_cross_signaler,
                                          NULL, 9, 1024);
    TEST_ASSERT(signaler >= 0, "cross signaler created");
    tcb_t *stcb = signaler >= 0 ? task_get_tcb(signaler) : NULL;
    if (waiter < 0 || signaler < 0 || wtcb == NULL || stcb == NULL) {
        goto out;
    }

    void *obj = notification_obj_for_cap(id);
    uint32_t gen = ((const kobject_header_t *)obj)->generation;
    /* waiter 只拿 READ(wait 足够);signaler 拿带 0x40 徽章的 WRITE cap */
    cap_id_t wcap = cap_create_for(wtcb, obj, CAP_OBJ_NOTIFICATION, CAP_READ);
    cap_id_t scap = cap_create_for_gen_badge(stcb, obj,
                                             CAP_OBJ_NOTIFICATION,
                                             CAP_WRITE, gen, 0x40U);
    TEST_ASSERT(wcap >= 0 && scap >= 0, "caps handed to both users");
    if (wcap < 0 || scap < 0) {
        goto out;
    }

    (void)task_start(waiter);
    for (int i = 0; i < 200 &&
         task_get_state(waiter) != TASK_STATE_BLOCKED; i++) {
        (void)task_delay(1U);
    }
    TEST_ASSERT_EQ(TASK_STATE_BLOCKED, task_get_state(waiter),
                   "cross waiter blocked");

    (void)task_start(signaler);
    uintptr_t sret = 0;
    TEST_ASSERT_EQ(KERN_OK,
                   task_join(signaler, (void **)&sret, NTFN_USER_JOIN_TICKS),
                   "signaler joined");
    TEST_ASSERT_EQ((int32_t)KERN_OK, (int32_t)(intptr_t)sret,
                   "signaler exit ok");

    uintptr_t wret = 0;
    TEST_ASSERT_EQ(KERN_OK,
                   task_join(waiter, (void **)&wret, NTFN_USER_JOIN_TICKS),
                   "waiter joined");
    TEST_ASSERT_EQ((int32_t)KERN_OK, (int32_t)(intptr_t)wret,
                   "waiter received signaler badge 0x40");

out:
    if (waiter >= 0) (void)task_delete(waiter);
    if (signaler >= 0) (void)task_delete(signaler);
    TEST_ASSERT_EQ(KERN_OK, notification_delete(id), "cleanup");
}

/*============================================================================
 * 模块入口
 *============================================================================*/

static void test_ntfn_user_module(void) {
    test_ntfn_user_basic();
    test_ntfn_user_rights();
    test_ntfn_user_timeout();
    test_ntfn_user_block_wake();
    test_ntfn_user_cross_signal();
}

TEST_ABI_MODULE(ntfn_user, test_ntfn_user_module);

#endif /* IPC_NOTIFICATION && TEST_MODULE_NOTIFICATION && CAP_ENABLE */

#endif /* TEST_ENABLE */
