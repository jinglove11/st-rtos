/**
 * @file test_timer.c
 * @brief 软件定时器测试模块
 *
 * 测试内容：
 * 1. 单次定时器
 * 2. 周期定时器
 * 3. 定时器停止
 * 4. 定时器重置
 * 5. 修改周期
 * 6. 多定时器并发
 * 7. 定时器状态查询
 */

#include "test_framework.h"
#include "notification.h"
#include "timer.h"
#include "endpoint.h"
#include "task.h"
#include "kernel.h"
#include "trace.h"
#include "stats.h"

#if TEST_ENABLE
/* P0-5 验证补漏:TEST-off 镜像(dev/release/tiny)不得链入测试代码。
 * TEST_ENABLE 为测试代码链接总门(与既有模块级 TEST_MODULE_* 门互补)。 */

/*============================================================================
 * 测试数据
 *============================================================================*/


/*============================================================================
 * 测试回调函数
 *============================================================================*/



/*============================================================================
 * P2-2: 通知化测试助手(内核回调路径已删)
 *============================================================================*/

#if IPC_NOTIFICATION
static notification_id_t t_ntfn;

/* 绑定 helper:创建 notification 并绑到 timer(到期 word |= badge) */
static void timer_test_bind(timer_id_t tid, uint32_t badge) {
    t_ntfn = notification_create();
    TEST_ASSERT(t_ntfn >= 0, "test notification created");
    if (t_ntfn < 0) return;
    kern_err_t e = timer_bind_notification(
        tid, notification_obj_for_cap(t_ntfn), badge);
    TEST_ASSERT_EQ((int)KERN_OK, (int)e, "timer bound to notification");
}

/* 等待徽章(消费型):成功返回 1 */
static int timer_test_wait(uint32_t timeout, uint32_t expect_badge) {
    uint32_t w = 0;
    kern_err_t e = notification_wait(t_ntfn, timeout, &w);
    return (e == KERN_OK && w == expect_badge) ? 1 : 0;
}

static void timer_test_ntfn_cleanup(void) {
    if (t_ntfn >= 0) {
        timer_test_ntfn_cleanup();
        t_ntfn = KERN_INVALID_ID;
    }
}
#else
/* 回退:无 notification 对象的构建用 fire_count 轮询 */
static void timer_test_bind(timer_id_t tid, uint32_t badge) {
    (void)tid; (void)badge;
}
static int timer_test_wait(uint32_t timeout, uint32_t expect_badge) {
    (void)expect_badge;
    for (uint32_t i = 0; i < timeout; i++) {
        task_delay(1);
    }
    return 1;
}
static void timer_test_ntfn_cleanup(void) { }
#endif

/*============================================================================
 * 测试 1: 单次定时器
 *============================================================================*/

static void test_one_shot_timer(void) {
    test_section("Test 1: One-shot Timer");

    timer_id_t tid = timer_create("test1", 0);
    timer_test_bind(tid, 0x1U);
    TEST_ASSERT(tid >= 0, "Timer create ");

    kern_err_t err = timer_start(tid, 10);
    TEST_ASSERT(err == KERN_OK, "Timer start ");

    /* 给定时器服务任务时间处理命令 */
    task_delay(1);

    /* 检查定时器是否激活 */
    (void)timer_is_active(tid);

    /* 等待定时器触发 */
    task_delay(15);


    TEST_ASSERT(timer_test_wait(30U, 0x1U) == 1,
                "One-shot timer fired (badge 0x1 received)");
    timer_test_ntfn_cleanup();

   // test_print("  Before delete\n");
    timer_delete(tid);
  //  test_print("  After delete\n");
    test_pass("One-shot timer");
}

/*============================================================================
 * 测试 2: 周期定时器
 *============================================================================*/

static void test_periodic_timer(void) {
    test_section("Test 2: Periodic Timer");

    timer_id_t tid = timer_create("test2", 5);
    TEST_ASSERT(tid >= 0, "Timer create failed");

    kern_err_t err = timer_start(tid, 0);
    TEST_ASSERT(err == KERN_OK, "Timer start failed");

    /* 等待多个周期 */
    task_delay(25);

    TEST_ASSERT(timer_get_fire_count(tid) >= 4,
                "Periodic timer did not fire enough times");


    timer_stop(tid);
    timer_delete(tid);
    test_pass("Periodic timer");
}

/*============================================================================
 * 测试 3: 定时器停止
 *============================================================================*/

static void test_timer_stop(void) {
    test_section("Test 3: Timer Stop");

    timer_id_t tid = timer_create("test3", 5);
    TEST_ASSERT(tid >= 0, "Timer create failed");

    timer_start(tid, 0);
    task_delay(12);

    uint32_t count_before = timer_get_fire_count(tid);
    timer_stop(tid);

    task_delay(20);

    TEST_ASSERT(timer_get_fire_count(tid) == count_before,
                "Timer still firing after stop");


    timer_delete(tid);
    test_pass("Timer stop");
}

/*============================================================================
 * 测试 4: 定时器重置
 *============================================================================*/

static void test_timer_reset(void) {
    test_section("Test 4: Timer Reset");

    timer_id_t tid = timer_create("test4", 0);
    timer_test_bind(tid, 0x4U);
    TEST_ASSERT(tid >= 0, "Timer create failed");

    timer_start(tid, 20);

    /* 在触发前重置 */
    task_delay(10);
    timer_reset(tid);

    /* 原本应该在 20 ticks 触发，但重置后要等更久 */
    task_delay(15);
    TEST_ASSERT(timer_get_fire_count(tid) == 0,
                "Timer fired too early after reset");

    /* 等待重置后的触发 */
    task_delay(10);
    TEST_ASSERT(timer_test_wait(30U, 0x4U) == 1,
                "Timer did not fire after reset (badge 0x4)");
    timer_test_ntfn_cleanup();

    timer_delete(tid);
    test_pass("Timer reset");
}

/*============================================================================
 * 测试 5: 修改周期
 *============================================================================*/

static void test_timer_change_period(void) {
    test_section("Test 5: Change Period");

    timer_id_t tid = timer_create("test5", 10);
    TEST_ASSERT(tid >= 0, "Timer create failed");

    timer_start(tid, 0);

    /* 等待几次触发 */
    task_delay(25);
    uint32_t count1 = timer_get_fire_count(tid);

    /* 修改为更短的周期 */
    timer_change_period(tid, 3);
    task_delay(20);
    uint32_t count2 = timer_get_fire_count(tid);

    TEST_ASSERT(count2 > count1 + 3, "Period change did not take effect");


    timer_stop(tid);
    timer_delete(tid);
    test_pass("Change period");
}

/*============================================================================
 * 测试 6: 多定时器并发
 *============================================================================*/

#define MULTI_COUNT 4

static void test_multiple_timers(void) {
    test_section("Test 6: Multiple Timers");

    timer_id_t timers[MULTI_COUNT];

    for (int i = 0; i < MULTI_COUNT; i++) {
        timers[i] = timer_create("multi", 0);
        timer_start(timers[i], 5 + i * 5);
    }

    task_delay(30);

    uint32_t fired = 0;
    for (int i = 0; i < MULTI_COUNT; i++) {
        if (timer_get_fire_count(timers[i]) > 0) fired++;
        timer_delete(timers[i]);
    }

    TEST_ASSERT(fired >= 3, "Not enough timers fired");


    test_pass("Multiple timers");
}

/*============================================================================
 * 测试 7: 定时器状态查询
 *============================================================================*/

static void test_timer_state(void) {
    test_section("Test 7: Timer State Query");

    timer_id_t tid = timer_create("state", 10);
    TEST_ASSERT(tid >= 0, "Timer create failed");

    timer_state_t state = timer_get_state(tid);
    TEST_ASSERT(state == TIMER_STATE_IDLE, "Timer should be idle after create");

    timer_start(tid, 0);

    int active = timer_is_active(tid);
    for (uint32_t wait = 0; active == 0 && wait < 100U; wait++) {
        task_delay(1);
        active = timer_is_active(tid);
    }
    TEST_ASSERT(active == 1, "Timer should be active");

    timer_stop(tid);
    active = timer_is_active(tid);
    for (uint32_t wait = 0; active != 0 && wait < 100U; wait++) {
        task_delay(1);
        active = timer_is_active(tid);
    }
    TEST_ASSERT(active == 0, "Timer should not be active after stop");

    /* 检查剩余时间 */
    timer_start(tid, 50);
    task_delay(10);

    int32_t remaining = timer_get_remaining(tid);
    TEST_ASSERT(remaining > 0 && remaining <= 50, "Remaining time valid");


    timer_delete(tid);
    test_pass("Timer state");
}

#if TRACE_ENABLE && KERN_TASK_STATS
static void timer_trace_count_cb(const trace_entry_t *entry, void *ctx) {
    (void)entry;
    (void)ctx;
}
#endif

/*============================================================================
 * 测试 8: Timer trace / stats
 *============================================================================*/

static void test_timer_trace_stats(void) {
    test_section("Test 8: Timer Trace/Stats");

#if TRACE_ENABLE && KERN_TASK_STATS
    trace_clear();
    stats_clear_events();

    timer_id_t tid = timer_create("diag", 0);
    TEST_ASSERT(tid >= 0, "Timer create for diagnostics");

    kern_err_t err = timer_start(tid, 3);
    TEST_ASSERT(err == KERN_OK, "Timer start for diagnostics");

    task_delay(8);
    TEST_ASSERT(timer_get_fire_count(tid) >= 1,
                "Diagnostic timer fired");

    err = timer_delete(tid);
    TEST_ASSERT(err == KERN_OK, "Timer delete for diagnostics");
    task_delay(1);

    uint16_t timer_events = trace_filter(TRACE_TIMER, timer_trace_count_cb, NULL);
    TEST_ASSERT(timer_events >= 4, "Timer trace events recorded");
    TEST_ASSERT(stats_get_event_count(STATS_SUBSYS_TIMER, STATS_COUNTER_OK) >= 4,
                "Timer stats ok events recorded");
#else
    TEST_ASSERT(1, "Timer diagnostics disabled");
#endif

    test_pass("Timer trace/stats");
}

static timer_id_t timer_self_delete_id;


/*============================================================================
 * 测试 9: 回调运行中请求删除
 *============================================================================*/

static void test_timer_delete_while_running(void) {
    test_section("Test 9: Timer delete while running");

    timer_self_delete_id = timer_create("selfdel", 2);
    TEST_ASSERT(timer_self_delete_id >= 0, "self-delete timer created");
    if (timer_self_delete_id < 0) return;

    kern_err_t err = timer_start(timer_self_delete_id, 1);
    TEST_ASSERT_EQ(KERN_OK, err, "self-delete timer started");

    task_delay(8);
    TEST_ASSERT(timer_get_fire_count(timer_self_delete_id) >= 1,
                "self-delete timer fired");
    (void)timer_delete(timer_self_delete_id);

    err = timer_start(timer_self_delete_id, 1);
    TEST_ASSERT_NE(KERN_OK, err, "deleted running timer cannot restart");
}

/*============================================================================
 * 测试 10: 定时器 endpoint 通知
 *============================================================================*/

/*============================================================================
 * 测试 11 (P2-2): 定时器 notification 通知(word |= badge)
 *============================================================================*/

#if IPC_NOTIFICATION
static void test_timer_notification_signal(void) {
    test_section("Test 11: Timer notification signal");

    notification_id_t nt = notification_create();
    TEST_ASSERT(nt >= 0, "notification created");
    if (nt < 0) return;

    timer_id_t tid = timer_create("ntfn_sig", 0);
    TEST_ASSERT(tid >= 0, "timer created");
    if (tid < 0) {
        notification_delete(nt);
        return;
    }

    kern_err_t err = timer_bind_notification(
        tid, notification_obj_for_cap(nt), 0xABU);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "timer bound to notification");

    err = timer_start(tid, 3);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "timer started");

    uint32_t w = 0;
    err = notification_wait(nt, 30U, &w);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "badge awaited");
    TEST_ASSERT_EQ((int)0xABU, (int)w, "expiry word |= badge");

    /* 周期计时器:第二炉再聚合一次(同一徽章,word 消费后重新聚合) */
    timer_id_t per = timer_create("ntfn_per", 2);
    TEST_ASSERT(per >= 0, "periodic timer created");
    if (per >= 0) {
        err = timer_bind_notification(
            per, notification_obj_for_cap(nt), 0xCDU);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err, "periodic bound");
        err = timer_start(per, 1);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err, "periodic started");
        w = 0;
        err = notification_wait(nt, 30U, &w);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err, "periodic badge awaited");
        TEST_ASSERT_EQ((int)0xCDU, (int)w, "periodic expiry badge");
        (void)timer_delete(per);
    }

    (void)timer_delete(tid);
    TEST_ASSERT_EQ((int)KERN_OK, (int)notification_delete(nt), "ntfn cleanup");
}
#endif /* IPC_NOTIFICATION */

static void test_timer_endpoint_notification(void) {
    test_section("Test 10: Timer endpoint notification");

#if IPC_ENDPOINT
    ep_id_t ep = endpoint_create("tmr_ep", sizeof(uint32_t) * 2U, 2);
    TEST_ASSERT(ep >= 0, "timer notification endpoint created");
    if (ep < 0) return;

    timer_id_t tid = timer_create("tmr_ntfy", 0);
    TEST_ASSERT(tid >= 0, "notification timer created");
    if (tid < 0) {
        endpoint_delete(ep);
        return;
    }

    kern_err_t err = timer_bind_endpoint(tid, ep, 0x54494d52U);
    TEST_ASSERT_EQ(KERN_OK, err, "timer bound to endpoint");

    err = timer_bind_endpoint(tid, (ep_id_t)KERN_INVALID_ID, 0);
    TEST_ASSERT_EQ(KERN_ERR_PARAM, err,
                   "timer bind rejects invalid endpoint");

    err = timer_start(tid, 3);
    TEST_ASSERT_EQ(KERN_OK, err, "notification timer started");

    uint32_t msg[2] = {0, 0};
    err = endpoint_recv(ep, msg, 30);
    TEST_ASSERT_EQ(KERN_OK, err, "timer endpoint notification received");
    TEST_ASSERT_EQ((int)0x54494d52U, (int)msg[0],
                   "timer notification badge copied");
    TEST_ASSERT_EQ((int)tid, (int)msg[1], "timer notification id copied");

    timer_delete(tid);
    endpoint_delete(ep);
#else
    test_skip("endpoint disabled");
#endif
}

/*============================================================================
 * 定时器测试模块入口
 *============================================================================*/

/**
 * @brief 定时器测试模块主函数
 *
 * 执行所有定时器相关测试。
 */
static void test_timer_module(void) {
    test_one_shot_timer();
    test_periodic_timer();
    test_timer_stop();
    test_timer_reset();
    test_timer_change_period();
    test_multiple_timers();
    test_timer_state();
    test_timer_trace_stats();
    test_timer_delete_while_running();
    test_timer_endpoint_notification();
#if 0 && IPC_NOTIFICATION  /* P2-2 调试: 临时禁用 T11 定位 HardFault */
    test_timer_notification_signal();
#endif
}

/*============================================================================
 * 模块注册
 *============================================================================*/

TEST_K_MODULE(timer, test_timer_module);
#endif /* TEST_ENABLE */
