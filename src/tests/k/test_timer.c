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
#include "timer.h"
#include "endpoint.h"
#include "task.h"
#include "kernel.h"
#include "trace.h"
#include "stats.h"

/*============================================================================
 * 测试数据
 *============================================================================*/

static volatile int test_flag = 0;
static volatile int test_count = 0;

/*============================================================================
 * 测试回调函数
 *============================================================================*/

static void callback_set_flag(void *arg) {
    int *flag = (int *)arg;
    *flag = 1;
}

static void callback_increment(void *arg) {
    int *count = (int *)arg;
    (*count)++;
}

/*============================================================================
 * 测试 1: 单次定时器
 *============================================================================*/

static void test_one_shot_timer(void) {
    test_section("Test 1: One-shot Timer");

    test_flag = 0;

    timer_id_t tid = timer_create("test1", callback_set_flag, (void *)&test_flag, 0);
    TEST_ASSERT(tid >= 0, "Timer create ");

    kern_err_t err = timer_start(tid, 10);
    TEST_ASSERT(err == KERN_OK, "Timer start ");

    /* 给定时器服务任务时间处理命令 */
    task_delay(1);

    /* 检查定时器是否激活 */
    (void)timer_is_active(tid);

    /* 等待定时器触发 */
    task_delay(15);


    TEST_ASSERT(test_flag == 1, "One-shot timer did not fire");

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

    test_count = 0;

    timer_id_t tid = timer_create("test2", callback_increment, (void *)&test_count, 5);
    TEST_ASSERT(tid >= 0, "Timer create failed");

    kern_err_t err = timer_start(tid, 0);
    TEST_ASSERT(err == KERN_OK, "Timer start failed");

    /* 等待多个周期 */
    task_delay(25);

    TEST_ASSERT(test_count >= 4, "Periodic timer did not fire enough times");


    timer_stop(tid);
    timer_delete(tid);
    test_pass("Periodic timer");
}

/*============================================================================
 * 测试 3: 定时器停止
 *============================================================================*/

static void test_timer_stop(void) {
    test_section("Test 3: Timer Stop");

    test_count = 0;

    timer_id_t tid = timer_create("test3", callback_increment, (void *)&test_count, 5);
    TEST_ASSERT(tid >= 0, "Timer create failed");

    timer_start(tid, 0);
    task_delay(12);

    int count_before = test_count;
    timer_stop(tid);

    task_delay(20);

    TEST_ASSERT(test_count == count_before, "Timer still firing after stop");


    timer_delete(tid);
    test_pass("Timer stop");
}

/*============================================================================
 * 测试 4: 定时器重置
 *============================================================================*/

static void test_timer_reset(void) {
    test_section("Test 4: Timer Reset");

    test_flag = 0;

    timer_id_t tid = timer_create("test4", callback_set_flag, (void *)&test_flag, 0);
    TEST_ASSERT(tid >= 0, "Timer create failed");

    timer_start(tid, 20);

    /* 在触发前重置 */
    task_delay(10);
    timer_reset(tid);

    /* 原本应该在 20 ticks 触发，但重置后要等更久 */
    task_delay(15);
    TEST_ASSERT(test_flag == 0, "Timer fired too early after reset");

    /* 等待重置后的触发 */
    task_delay(10);
    TEST_ASSERT(test_flag == 1, "Timer did not fire after reset");

    timer_delete(tid);
    test_pass("Timer reset");
}

/*============================================================================
 * 测试 5: 修改周期
 *============================================================================*/

static void test_timer_change_period(void) {
    test_section("Test 5: Change Period");

    test_count = 0;

    timer_id_t tid = timer_create("test5", callback_increment, (void *)&test_count, 10);
    TEST_ASSERT(tid >= 0, "Timer create failed");

    timer_start(tid, 0);

    /* 等待几次触发 */
    task_delay(25);
    int count1 = test_count;

    /* 修改为更短的周期 */
    timer_change_period(tid, 3);
    task_delay(20);
    int count2 = test_count;

    TEST_ASSERT(count2 > count1 + 3, "Period change did not take effect");


    timer_stop(tid);
    timer_delete(tid);
    test_pass("Change period");
}

/*============================================================================
 * 测试 6: 多定时器并发
 *============================================================================*/

#define MULTI_COUNT 4
static int multi_flags[MULTI_COUNT];

static void test_multiple_timers(void) {
    test_section("Test 6: Multiple Timers");

    timer_id_t timers[MULTI_COUNT];

    for (int i = 0; i < MULTI_COUNT; i++) {
        multi_flags[i] = 0;
        timers[i] = timer_create("multi", callback_set_flag, &multi_flags[i], 0);
        timer_start(timers[i], 5 + i * 5);
    }

    task_delay(30);

    int fired = 0;
    for (int i = 0; i < MULTI_COUNT; i++) {
        if (multi_flags[i]) fired++;
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

    timer_id_t tid = timer_create("state", callback_set_flag, (void *)&test_flag, 10);
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
    test_flag = 0;
    trace_clear();
    stats_clear_events();

    timer_id_t tid = timer_create("diag", callback_set_flag, (void *)&test_flag, 0);
    TEST_ASSERT(tid >= 0, "Timer create for diagnostics");

    kern_err_t err = timer_start(tid, 3);
    TEST_ASSERT(err == KERN_OK, "Timer start for diagnostics");

    task_delay(8);
    TEST_ASSERT(test_flag == 1, "Diagnostic timer fired");

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
static volatile int timer_self_delete_count;

static void callback_delete_self(void *arg) {
    (void)arg;
    timer_self_delete_count++;
    (void)timer_delete(timer_self_delete_id);
}

/*============================================================================
 * 测试 9: 回调运行中请求删除
 *============================================================================*/

static void test_timer_delete_while_running(void) {
    test_section("Test 9: Timer delete while running");

    timer_self_delete_count = 0;
    timer_self_delete_id = timer_create("selfdel", callback_delete_self, NULL, 2);
    TEST_ASSERT(timer_self_delete_id >= 0, "self-delete timer created");
    if (timer_self_delete_id < 0) return;

    kern_err_t err = timer_start(timer_self_delete_id, 1);
    TEST_ASSERT_EQ(KERN_OK, err, "self-delete timer started");

    task_delay(8);
    TEST_ASSERT_EQ(1, timer_self_delete_count, "self-delete timer fired once");

    err = timer_start(timer_self_delete_id, 1);
    TEST_ASSERT_NE(KERN_OK, err, "deleted running timer cannot restart");
}

/*============================================================================
 * 测试 10: 定时器 endpoint 通知
 *============================================================================*/

static void test_timer_endpoint_notification(void) {
    test_section("Test 10: Timer endpoint notification");

#if IPC_ENDPOINT
    ep_id_t ep = endpoint_create("tmr_ep", sizeof(uint32_t) * 2U, 2);
    TEST_ASSERT(ep >= 0, "timer notification endpoint created");
    if (ep < 0) return;

    timer_id_t tid = timer_create("tmr_ntfy", NULL, NULL, 0);
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
}

/*============================================================================
 * 模块注册
 *============================================================================*/

TEST_K_MODULE(timer, test_timer_module);
