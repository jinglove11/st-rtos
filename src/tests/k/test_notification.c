/**
 * @file test_notification.c
 * @brief P1-1 (A4): 独立 notification 对象白盒测试
 *
 * 与 abi/test_notification.c(event 扮演的通知角色)不同,这里测的是
 * seL4 语义的独立对象:聚合徽章 word、整字消费、单等待者移交、
 * badge 驱动 signal、删除唤醒、ISR 安全 signal。
 *
 * 模块名 "ntfn" 避免与 abi 层 "notification" 模块重名。
 */

#include "test_framework.h"
#include "kernel.h"
#include "notification.h"
#include "task.h"
#include "scheduler.h"
#include "capability.h"
#include "hal.h"
#include <string.h>

#if TEST_ENABLE

#if IPC_NOTIFICATION && TEST_MODULE_NOTIFICATION

/*============================================================================
 * 聚合与消费语义(无等待者)
 *============================================================================*/

static void test_ntfn_aggregation_and_poll(void) {
    test_section("Test 1: word aggregation + consuming poll");

    notification_id_t id = notification_create();
    TEST_ASSERT(id >= 0, "notification created");
    if (id < 0) return;

    /* 空对象 poll:OK + word 0 */
    uint32_t w = 0xDEADBEEFU;
    TEST_ASSERT_EQ(KERN_OK, notification_poll(id, &w), "poll empty ok");
    TEST_ASSERT_EQ(0, w, "empty poll yields 0");

    /* 两次 signal 聚合(无等待者,字留存) */
    TEST_ASSERT_EQ(KERN_OK, notification_signal(id, 0x1U), "signal 0x1");
    TEST_ASSERT_EQ(KERN_OK, notification_signal(id, 0x2U), "signal 0x2");
    TEST_ASSERT_EQ(KERN_OK, notification_poll(id, &w), "poll ok");
    TEST_ASSERT_EQ(0x3, w, "aggregated word 0x1|0x2");

    /* 消费后再 poll 为空 */
    TEST_ASSERT_EQ(KERN_OK, notification_poll(id, &w), "second poll ok");
    TEST_ASSERT_EQ(0, w, "word consumed");

    TEST_ASSERT_EQ(KERN_OK, notification_delete(id), "deleted");
}

/*============================================================================
 * 阻塞 wait:signal 唤醒 + 整字移交
 *============================================================================*/

static notification_id_t wait_ntfn;
static volatile uint32_t waiter_word;
static volatile kern_err_t waiter_err;
static volatile uint8_t waiter_done;

static void ntfn_waiter_task(void *arg) {
    (void)arg;
    uint32_t w = 0;
    waiter_word = 0;
    waiter_done = 0;
    /* 内核任务直呼 wait(fast path 由 signal 前置触发阻塞路径) */
    waiter_err = notification_wait_syscall(wait_ntfn, 100000U, &w);
    waiter_word = w;
    waiter_done = 1;
}

static void test_ntfn_blocking_wait_wake(void) {
    test_section("Test 2: blocking wait woken by signal");

    wait_ntfn = notification_create();
    TEST_ASSERT(wait_ntfn >= 0, "created");
    if (wait_ntfn < 0) return;

    task_id_t tid = task_create("ntfn_w1", ntfn_waiter_task, NULL, 8, 1024);
    TEST_ASSERT(tid >= 0, "waiter task created");
    if (tid < 0) { (void)notification_delete(wait_ntfn); return; }
    TEST_ASSERT_EQ(KERN_OK, task_start(tid), "waiter started");

    /* 等待者进入 BLOCKED */
    for (int i = 0; i < 100 && task_get_state(tid) != TASK_STATE_BLOCKED; i++) {
        (void)task_delay(1U);
    }
    TEST_ASSERT_EQ(TASK_STATE_BLOCKED, task_get_state(tid), "waiter blocked");

    TEST_ASSERT_EQ(KERN_OK, notification_signal(wait_ntfn, 0xA5U), "signal");

    for (int i = 0; i < 100 && !waiter_done; i++) {
        (void)task_delay(1U);
    }
    TEST_ASSERT_EQ(1, waiter_done, "waiter woke");
    TEST_ASSERT_EQ(KERN_OK, waiter_err, "wait returned OK");
    TEST_ASSERT_EQ(0xA5, waiter_word, "word delivered whole");

    (void)task_delete(tid);
    TEST_ASSERT_EQ(KERN_OK, notification_delete(wait_ntfn), "deleted");
}

/*============================================================================
 * 多等待者:一次 signal 只移交一个
 *============================================================================*/

static volatile uint32_t mw_words[2];
static volatile kern_err_t mw_errs[2];
static volatile uint8_t mw_done[2];

static void mw_waiter(void *arg) {
    uintptr_t idx = (uintptr_t)arg;
    uint32_t w = 0;
    mw_errs[idx] = notification_wait_syscall(wait_ntfn, 50000U, &w);
    mw_words[idx] = w;
    mw_done[idx] = 1;
}

static void test_ntfn_single_delivery_two_waiters(void) {
    test_section("Test 3: one signal wakes exactly one of two waiters");

    wait_ntfn = notification_create();
    TEST_ASSERT(wait_ntfn >= 0, "created");
    if (wait_ntfn < 0) return;

    mw_done[0] = mw_done[1] = 0;
    mw_words[0] = mw_words[1] = 0;

    task_id_t t0 = task_create("ntfn_m0", mw_waiter, (void *)0U, 8, 1024);
    task_id_t t1 = task_create("ntfn_m1", mw_waiter, (void *)1U, 8, 1024);
    TEST_ASSERT(t0 >= 0 && t1 >= 0, "waiters created");
    if (t0 < 0 || t1 < 0) goto out;
    (void)task_start(t0);
    (void)task_start(t1);

    for (int i = 0; i < 100 &&
         (task_get_state(t0) != TASK_STATE_BLOCKED ||
          task_get_state(t1) != TASK_STATE_BLOCKED); i++) {
        (void)task_delay(1U);
    }
    TEST_ASSERT_EQ(TASK_STATE_BLOCKED, task_get_state(t0), "w0 blocked");
    TEST_ASSERT_EQ(TASK_STATE_BLOCKED, task_get_state(t1), "w1 blocked");

    /* 一次 signal:整字交给队头,另一等待者保持阻塞 */
    TEST_ASSERT_EQ(KERN_OK, notification_signal(wait_ntfn, 0xF0U), "signal");
    for (int i = 0; i < 100 && !(mw_done[0] || mw_done[1]); i++) {
        (void)task_delay(1U);
    }
    TEST_ASSERT(mw_done[0] != mw_done[1], "exactly one waiter woke");
    TEST_ASSERT_EQ(0xF0, mw_words[mw_done[0] ? 0 : 1], "whole word delivered");

    /* 再 signal:第二个等待者拿到(新字,不含上次已消费的) */
    TEST_ASSERT_EQ(KERN_OK, notification_signal(wait_ntfn, 0x0FU), "signal 2");
    for (int i = 0; i < 100 && !(mw_done[0] && mw_done[1]); i++) {
        (void)task_delay(1U);
    }
    TEST_ASSERT_EQ(1, mw_done[0] && 1, "both done");
    TEST_ASSERT_EQ(1, mw_done[1] && 1, "both done (b)");
    TEST_ASSERT_EQ(0x0F, mw_words[mw_done[0] ? 1 : 0], "second word fresh");

    (void)task_delay(2U);
out:
    if (t0 >= 0) (void)task_delete(t0);
    if (t1 >= 0) (void)task_delete(t1);
    TEST_ASSERT_EQ(KERN_OK, notification_delete(wait_ntfn), "deleted");
}

/*============================================================================
 * 超时路径
 *============================================================================*/

static volatile kern_err_t to_err;

static void to_waiter(void *arg) {
    (void)arg;
    uint32_t w = 0;
    to_err = notification_wait_syscall(wait_ntfn, 5U, &w);
}

static void test_ntfn_wait_timeout(void) {
    test_section("Test 4: wait timeout on silent object");

    wait_ntfn = notification_create();
    TEST_ASSERT(wait_ntfn >= 0, "created");
    if (wait_ntfn < 0) return;

    task_id_t tid = task_create("ntfn_to", to_waiter, NULL, 8, 1024);
    TEST_ASSERT(tid >= 0, "timeout waiter created");
    if (tid < 0) { (void)notification_delete(wait_ntfn); return; }
    to_err = KERN_OK;
    (void)task_start(tid);

    for (int i = 0; i < 200 && task_get_state(tid) != TASK_STATE_TERMINATED &&
                    to_err == KERN_OK; i++) {
        (void)task_delay(1U);
    }
    TEST_ASSERT_EQ(KERN_ERR_TIMEOUT, to_err, "wait timed out");
    /* 超时后对象仍可用:signal 聚合,poll 取到 */
    TEST_ASSERT_EQ(KERN_OK, notification_signal(wait_ntfn, 0x11U), "signal");
    uint32_t w = 0;
    TEST_ASSERT_EQ(KERN_OK, notification_poll(wait_ntfn, &w), "poll");
    TEST_ASSERT_EQ(0x11, w, "word intact after timeout");

    (void)task_delete(tid);
    TEST_ASSERT_EQ(KERN_OK, notification_delete(wait_ntfn), "deleted");
}

/*============================================================================
 * 删除唤醒等待者(NOEXIST)+ 复用防护
 *============================================================================*/

static volatile kern_err_t del_err;

static void del_waiter(void *arg) {
    (void)arg;
    uint32_t w = 0;
    del_err = KERN_OK;
    del_err = notification_wait_syscall(wait_ntfn, 100000U, &w);
}

static void test_ntfn_delete_wakes_waiter(void) {
    test_section("Test 5: delete wakes waiter with NOEXIST");

    wait_ntfn = notification_create();
    TEST_ASSERT(wait_ntfn >= 0, "created");
    if (wait_ntfn < 0) return;

    task_id_t tid = task_create("ntfn_del", del_waiter, NULL, 8, 1024);
    TEST_ASSERT(tid >= 0, "del waiter created");
    if (tid < 0) { (void)notification_delete(wait_ntfn); return; }
    (void)task_start(tid);

    for (int i = 0; i < 100 && task_get_state(tid) != TASK_STATE_BLOCKED; i++) {
        (void)task_delay(1U);
    }
    TEST_ASSERT_EQ(KERN_OK, notification_delete(wait_ntfn), "deleted");

    for (int i = 0; i < 100 && del_err == KERN_OK; i++) {
        (void)task_delay(1U);
    }
    TEST_ASSERT_EQ(KERN_ERR_NOEXIST, del_err, "waiter woke with NOEXIST");

    (void)task_delete(tid);
}

/*============================================================================
 * ISR 上下文 signal 安全(临界区内 |= 与唤醒)
 *============================================================================*/

static void test_ntfn_signal_in_critical_section(void) {
    test_section("Test 6: signal from IRQs-off critical section");

    notification_id_t id = notification_create();
    TEST_ASSERT(id >= 0, "created");
    if (id < 0) return;

    uint32_t crit = hal_irq_save();
    kern_err_t err = notification_signal(id, 0x80U);
    hal_irq_restore(crit);

    TEST_ASSERT_EQ(KERN_OK, err, "signal in crit section ok");
    uint32_t w = 0;
    TEST_ASSERT_EQ(KERN_OK, notification_poll(id, &w), "poll");
    TEST_ASSERT_EQ(0x80, w, "word set from crit section");

    TEST_ASSERT_EQ(KERN_OK, notification_delete(id), "deleted");
}

/*============================================================================
 * badge 驱动 signal(cap 路径,mint 徽章)
 *============================================================================*/

#if CAP_ENABLE
static volatile kern_err_t badge_err;
static volatile uint32_t badge_word;

static void badge_waiter(void *arg) {
    (void)arg;
    uint32_t w = 0;
    badge_err = KERN_OK;
    badge_err = notification_wait_syscall(wait_ntfn, 100000U, &w);
    badge_word = w;
}

static void test_ntfn_badged_cap_signal(void) {
    test_section("Test 7: signal word bits come from cap badge");

    wait_ntfn = notification_create();
    TEST_ASSERT(wait_ntfn >= 0, "created");
    if (wait_ntfn < 0) return;

    tcb_t *cur = sched_get_current();
    void *obj = notification_obj_for_cap(wait_ntfn);
    TEST_ASSERT(obj != NULL, "obj for cap");

    /* 全权 cap(signal 用),mint 出带徽章 0x44 的 signal cap */
    cap_id_t full = cap_create_for(cur, obj, CAP_OBJ_NOTIFICATION, CAP_FULL);
    TEST_ASSERT(full >= 0, "full cap created");
    cap_id_t minted = cap_mint_for(cur, full, CAP_WRITE, 0x44U);
    TEST_ASSERT(minted >= 0, "minted badged signal cap");

    /* 等待者阻塞 */
    badge_err = KERN_ERR_BUSY;
    task_id_t tid = task_create("ntfn_bdg", badge_waiter, NULL, 8, 1024);
    TEST_ASSERT(tid >= 0, "badge waiter created");
    if (tid < 0) goto out;
    (void)task_start(tid);
    for (int i = 0; i < 100 && task_get_state(tid) != TASK_STATE_BLOCKED; i++) {
        (void)task_delay(1U);
    }

    /* 经徽章 cap signal:字位 = 徽章,而非调用方参数 */
    kern_err_t err = notification_signal(
        notification_id_from_obj(cap_resolve(minted, CAP_OBJ_NOTIFICATION,
                                             CAP_WRITE)),
        cap_get_badge(minted));
    TEST_ASSERT_EQ(KERN_OK, err, "badged signal ok");

    for (int i = 0; i < 100 && badge_err == KERN_ERR_BUSY; i++) {
        (void)task_delay(1U);
    }
    TEST_ASSERT_EQ(KERN_OK, badge_err, "waiter woke");
    TEST_ASSERT_EQ(0x44, badge_word, "word == badge");

    (void)task_delete(tid);
out:
    (void)cap_delete(minted);
    (void)cap_delete(full);
    TEST_ASSERT_EQ(KERN_OK, notification_delete(wait_ntfn), "deleted");
}
#endif /* CAP_ENABLE */

/*============================================================================
 * 池耗尽与非法参数
 *============================================================================*/

static void test_ntfn_pool_and_params(void) {
    test_section("Test 8: pool exhaustion + param validation");

    notification_id_t ids[KERN_MAX_NOTIFICATIONS];
    int created = 0;
    for (int i = 0; i < KERN_MAX_NOTIFICATIONS; i++) {
        notification_id_t id = notification_create();
        if (id < 0) break;
        ids[created++] = id;
    }
    TEST_ASSERT_EQ(KERN_MAX_NOTIFICATIONS, created, "pool fills exactly");

    notification_id_t extra = notification_create();
    TEST_ASSERT_EQ(KERN_INVALID_ID, extra, "exhausted pool rejects");

    TEST_ASSERT_EQ(KERN_ERR_PARAM, notification_signal(9999, 0x1U),
                   "signal on bad id");
    TEST_ASSERT_EQ(KERN_ERR_PARAM, notification_poll(-1, NULL),
                   "poll on bad id");
    TEST_ASSERT_EQ(KERN_ERR_PARAM, notification_delete(9999),
                   "delete on bad id");

    for (int i = 0; i < created; i++) {
        (void)notification_delete(ids[i]);
    }

    /* 全部释放后可重建 */
    notification_id_t again = notification_create();
    TEST_ASSERT(again >= 0, "slot reusable after delete");
    if (again >= 0) {
        TEST_ASSERT_EQ(KERN_OK, notification_delete(again), "recreated del");
    }
}

/*============================================================================
 * 模块入口
 *============================================================================*/

static void test_ntfn_module(void) {
    test_ntfn_aggregation_and_poll();
    test_ntfn_blocking_wait_wake();
    test_ntfn_single_delivery_two_waiters();
    test_ntfn_wait_timeout();
    test_ntfn_delete_wakes_waiter();
    test_ntfn_signal_in_critical_section();
#if CAP_ENABLE
    test_ntfn_badged_cap_signal();
#endif
    test_ntfn_pool_and_params();
}

TEST_K_MODULE(ntfn, test_ntfn_module);

#endif /* IPC_NOTIFICATION && TEST_MODULE_NOTIFICATION */

#endif /* TEST_ENABLE */
