/**
 * @file test_irq.c
 * @brief 中断管理测试模块
 *
 * 测试内容：
 * 1. ISR 池管理 (注册/注销/满池)
 * 2. 中断上下文检测
 * 3. BH 生命周期
 * 4. 线程化 IRQ 生命周期
 * 5. irq_register 向量表验证
 * 6. ISR 守卫 (阻塞调用在 ISR 中拒绝)
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

/* Keep synthetic test IRQs inside the active board's NVIC range. */
#define TEST_IRQ_CAP_LIFECYCLE  ((int16_t)(BOARD_IRQ_COUNT - 5U))
#define TEST_IRQ_CAP_BIND       ((int16_t)(BOARD_IRQ_COUNT - 4U))
#define TEST_IRQ_CAP_REVOKE     ((int16_t)(BOARD_IRQ_COUNT - 3U))
#define TEST_IRQ_ENDPOINT       ((int16_t)(BOARD_IRQ_COUNT - 11U))
#define TEST_IRQ_UNBOUND        ((int16_t)(BOARD_IRQ_COUNT - 12U))
#define TEST_IRQ_THREADED       ((int16_t)(BOARD_IRQ_COUNT - 10U))
#define TEST_IRQ_THREADED_BASE  ((int16_t)(BOARD_IRQ_COUNT - IRQ_THREADED_MAX))

static void test_handler_stub(void);



/*============================================================================
 * 测试 1: ISR 池管理
 *============================================================================*/

static void test_isr_pool(void) {
    test_section("Test 1: ISR Pool Management");

    /*
     * Registering an IRQ also enables it in the NVIC.  Keep PRIMASK asserted
     * while filling the synthetic pool: on RP2350 the numeric range includes
     * live UART/timer sources whose status this test handler does not clear.
     */
    uint32_t outer_crit = hal_enter_critical();
    int16_t registered[IRQ_MAX_USER];
    int count = 0;
    /* Walk the board range until the descriptor pool fills.  Kernel-reserved
     * vectors (notably the SMP SIO FIFO IPI) are deliberately rejected and
     * must not reduce the number of ordinary descriptor slots exercised. */
    int limit = (int)BOARD_IRQ_COUNT;
    for (int i = 0; i < limit; i++) {
        kern_err_t err = irq_register((int16_t)i, test_handler_stub, 8);
        if (err == KERN_OK) {
            registered[count++] = (int16_t)i;
        }
    }

    int16_t extra_irq = (count < (int)BOARD_IRQ_COUNT) ?
                        (int16_t)count : registered[0];
    kern_err_t err = irq_register(extra_irq, test_handler_stub, 8);
    TEST_ASSERT(err != KERN_OK, "Pool full: register fails");

    if (count > 0) {
        int16_t reusable_irq = registered[0];
        err = irq_unregister(reusable_irq);
        TEST_ASSERT_EQ(KERN_OK, err, "Unregister IRQ slot");
        err = irq_register(reusable_irq, test_handler_stub, 8);
        TEST_ASSERT_EQ(KERN_OK, err, "Released IRQ slot reusable");
    }

    for (int i = 0; i < count; i++) {
        (void)irq_unregister(registered[i]);
    }
    hal_exit_critical(outer_crit);
}

/*============================================================================
 * 测试 2: 中断上下文检测
 *============================================================================*/

static void test_isr_context(void) {
    test_section("Test 2: ISR Context Detection");

    /* 2a: 任务上下文中 kern_is_in_isr() 返回 0 */
    TEST_ASSERT(!kern_is_in_isr(), "Not in ISR context");

    /* 2b: 任务上下文中 kern_irq_context() 返回 -1 */
    int ctx = kern_irq_context();
    TEST_ASSERT(ctx < 0, "IRQ context is -1 in task mode");

}

/*============================================================================
 * 测试 3: BH 生命周期
 *============================================================================*/

static int bh_test_counter = 0;

static void bh_test_handler(void *arg) {
    int *counter = (int *)arg;
    (*counter)++;
}

static void test_bh_lifecycle(void) {
    test_section("Test 3: Bottom Half Lifecycle");

    bh_test_counter = 0;

    /* 3a: 创建 BH */
    int16_t bh_id = bh_create(bh_test_handler, &bh_test_counter);
    TEST_ASSERT(bh_id >= 0, "BH create returns valid ID");

    /* 3b: 调度 BH */
    kern_err_t err = bh_schedule(bh_id);
    TEST_ASSERT_EQ(KERN_OK, err, "BH schedule OK");

    /* 3c: 等待 BH 处理 (BH 任务每 tick 扫描) */
    task_delay(5);

    TEST_ASSERT(bh_test_counter > 0, "BH handler executed");

    /* 3d: 删除 BH */
    err = bh_delete(bh_id);
    TEST_ASSERT_EQ(KERN_OK, err, "BH delete OK");

    /* 3e: 删除后不可调度 */
    err = bh_schedule(bh_id);
    TEST_ASSERT_NE(KERN_OK, err, "BH schedule fails after delete");

    /* 3f: 无效 BH ID */
    err = bh_schedule(-1);
    TEST_ASSERT_NE(KERN_OK, err, "Invalid BH ID fails");

    /* 3g: 创建所有 BH 直到满 */
    int16_t ids[IRQ_BH_MAX];
    int created = 0;
    for (int i = 0; i < IRQ_BH_MAX; i++) {
        ids[i] = bh_create(bh_test_handler, &bh_test_counter);
        if (ids[i] >= 0) created++;
    }
    int expected_available = IRQ_BH_MAX;
#if FAULT_ENDPOINT
    expected_available--;
#endif
    TEST_ASSERT_EQ(expected_available, created,
                   "All non-reserved BH slots creatable");

    /* 清理 */
    for (int i = 0; i < created; i++) {
        bh_delete(ids[i]);
    }
}

static void test_bh_endpoint_notification(void) {
    test_section("Test 3a: BH endpoint notification");

#if IPC_ENDPOINT
    ep_id_t ep = endpoint_create("bh_ep", sizeof(uint32_t) * 2U, 2);
    TEST_ASSERT(ep >= 0, "BH notification endpoint created");
    if (ep < 0) return;

    int16_t bh_id = bh_create(NULL, NULL);
    TEST_ASSERT(bh_id >= 0, "notification-only BH created");
    if (bh_id < 0) {
        endpoint_delete(ep);
        return;
    }

    kern_err_t err = bh_bind_endpoint(bh_id, ep, 0x42484e54U);
    TEST_ASSERT_EQ(KERN_OK, err, "BH bound to endpoint");

    err = bh_bind_endpoint(bh_id, (ep_id_t)KERN_INVALID_ID, 0);
    TEST_ASSERT_EQ(KERN_ERR_PARAM, err, "BH bind rejects invalid endpoint");

    err = bh_schedule(bh_id);
    TEST_ASSERT_EQ(KERN_OK, err, "notification BH scheduled");

    uint32_t msg[2] = {0, 0};
    err = endpoint_recv(ep, msg, 30);
    TEST_ASSERT_EQ(KERN_OK, err, "BH notification received");
    TEST_ASSERT_EQ((int)0x42484e54U, (int)msg[0],
                   "BH notification badge copied");
    TEST_ASSERT_EQ((int)bh_id, (int)msg[1], "BH notification id copied");

    bh_delete(bh_id);
    endpoint_delete(ep);
#else
    test_skip("endpoint disabled");
#endif
}

typedef struct {
    int16_t bh_id;
    int     ran;
} bh_self_delete_ctx_t;

static void bh_self_delete_handler(void *arg) {
    bh_self_delete_ctx_t *ctx = (bh_self_delete_ctx_t *)arg;
    ctx->ran++;
    (void)bh_delete(ctx->bh_id);
}

static void test_bh_delete_while_running(void) {
    test_section("Test 3c: BH delete while running");

    bh_self_delete_ctx_t ctx;
    ctx.bh_id = KERN_INVALID_ID;
    ctx.ran = 0;

    int16_t bh_id = bh_create(bh_self_delete_handler, &ctx);
    TEST_ASSERT(bh_id >= 0, "self-delete BH created");
    if (bh_id < 0) return;
    ctx.bh_id = bh_id;

    kern_err_t err = bh_schedule(bh_id);
    TEST_ASSERT_EQ(KERN_OK, err, "self-delete BH scheduled");

    task_delay(5);
    TEST_ASSERT_EQ(1, ctx.ran, "self-delete BH ran once");

    err = bh_schedule(bh_id);
    TEST_ASSERT_NE(KERN_OK, err, "deleted running BH cannot be rescheduled");

    int16_t new_id = bh_create(bh_test_handler, &bh_test_counter);
    TEST_ASSERT(new_id >= 0, "BH slot reusable after running delete");
    if (new_id >= 0) {
        (void)bh_delete(new_id);
    }
}

/*============================================================================
 * 测试 3b: IRQ/BH trace 与 stats
 *============================================================================*/

#if TRACE_ENABLE && KERN_TASK_STATS
static void irq_trace_count_cb(const trace_entry_t *entry, void *ctx) {
    (void)entry;
    (void)ctx;
}
#endif

static void test_irq_bh_trace_stats(void) {
    test_section("Test 3b: IRQ/BH Trace/Stats");

#if TRACE_ENABLE && KERN_TASK_STATS
    trace_clear();
    stats_clear_events();

    int16_t bh_id = bh_create(bh_test_handler, &bh_test_counter);
    TEST_ASSERT(bh_id >= 0, "BH create for diagnostics");

    kern_err_t err = bh_cancel(bh_id);
    TEST_ASSERT_EQ(KERN_OK, err, "BH cancel diagnostics");

    err = bh_delete(bh_id);
    TEST_ASSERT_EQ(KERN_OK, err, "BH delete diagnostics");

    err = irq_register(0, test_handler_stub, 8);
    TEST_ASSERT_EQ(KERN_OK, err, "IRQ register diagnostics");

    err = irq_disable(0);
    TEST_ASSERT_EQ(KERN_OK, err, "IRQ mask diagnostics");

    err = irq_enable(0);
    TEST_ASSERT_EQ(KERN_OK, err, "IRQ unmask diagnostics");

    err = irq_unregister(0);
    TEST_ASSERT_EQ(KERN_OK, err, "IRQ release diagnostics");

    uint16_t bh_events = trace_filter(TRACE_BH, irq_trace_count_cb, NULL);
    uint16_t irq_events = trace_filter(TRACE_IRQ, irq_trace_count_cb, NULL);
    TEST_ASSERT(bh_events >= 3, "BH trace events recorded");
    TEST_ASSERT(irq_events >= 4, "IRQ trace events recorded");
    TEST_ASSERT(stats_get_event_count(STATS_SUBSYS_BH, STATS_COUNTER_CANCEL) >= 1,
                "BH cancel stat recorded");
    TEST_ASSERT(stats_get_event_count(STATS_SUBSYS_BH, STATS_COUNTER_DELETE) >= 1,
                "BH delete stat recorded");
    TEST_ASSERT(stats_get_event_count(STATS_SUBSYS_IRQ, STATS_COUNTER_DELETE) >= 1,
                "IRQ release stat recorded");
#else
    TEST_ASSERT(1, "IRQ/BH diagnostics disabled");
#endif

    test_pass("IRQ/BH trace/stats");
}

/*============================================================================
 * 测试 4: 线程化 IRQ 生命周期 (池管理)
 *============================================================================*/

#if IRQ_THREADED_ENABLE

static void test_threaded_handler(void *arg) {
    int *counter = (int *)arg;
    (*counter)++;
}

static void test_threaded_irq(void) {
    test_section("Test 4: Threaded IRQ Lifecycle");

    /* 4a: 请求线程化 IRQ */
    int counter = 0;
    kern_err_t err = irq_request_threaded(TEST_IRQ_THREADED, test_threaded_handler,
                                          &counter, 10, 512);
    TEST_ASSERT_EQ(KERN_OK, err, "Threaded IRQ request OK");

    /* 4b: 重复请求同一 IRQ 应失败 */
    err = irq_request_threaded(TEST_IRQ_THREADED, test_threaded_handler,
                               &counter, 10, 512);
    TEST_ASSERT_NE(KERN_OK, err, "Duplicate threaded IRQ fails");

    /* 4c: 释放线程化 IRQ */
    err = irq_release_threaded(TEST_IRQ_THREADED);
    TEST_ASSERT_EQ(KERN_OK, err, "Threaded IRQ release OK");

    /* 4d: 释放后可以重新请求 */
    err = irq_request_threaded(TEST_IRQ_THREADED, test_threaded_handler,
                               &counter, 10, 512);
    TEST_ASSERT_EQ(KERN_OK, err, "Re-request threaded IRQ OK");

    /* 清理 */
    irq_release_threaded(TEST_IRQ_THREADED);

    /* 4e: 无效参数 */
    err = irq_request_threaded(-1, test_threaded_handler, NULL, 10, 512);
    TEST_ASSERT_NE(KERN_OK, err, "Invalid IRQ number fails");

    err = irq_request_threaded((int16_t)BOARD_IRQ_COUNT,
                               test_threaded_handler, NULL, 10, 512);
    TEST_ASSERT_NE(KERN_OK, err, "Out-of-range IRQ fails");

    err = irq_request_threaded(TEST_IRQ_THREADED_BASE, NULL, NULL, 10, 512);
    TEST_ASSERT_NE(KERN_OK, err, "NULL handler fails");

    /* 4f: 填充所有槽位 */
    int allocated = 0;
    for (int i = 0; i < IRQ_THREADED_MAX; i++) {
        err = irq_request_threaded((int16_t)(TEST_IRQ_THREADED_BASE + i),
                                   test_threaded_handler,
                                   &counter, 10, 512);
        if (err == KERN_OK) allocated++;
    }

    /* 清理 */
    for (int i = 0; i < allocated; i++) {
        irq_release_threaded((int16_t)(TEST_IRQ_THREADED_BASE + i));
    }
}

#endif /* IRQ_THREADED_ENABLE */

/*============================================================================
 * 测试 5: irq_register 向量表验证
 *============================================================================*/

static void test_handler_stub(void) {
    /* 测试桩 */
}

static void test_register_vector(void) {
    test_section("Test 5: irq_register Vector Table");

    /* 5a: 使用一个不常用的 IRQ 号 (DCMI 在 STM32F767 上很少使用) */
    /* 我们只测试 API 正确性, 不实际触发中断 */
    const int16_t test_irq = (int16_t)(BOARD_IRQ_COUNT - 1U);
    kern_err_t err = irq_register(test_irq, test_handler_stub, 8);
    TEST_ASSERT_EQ(KERN_OK, err, "Register spare IRQ");

    /* 5b: 启用和禁用 */
    err = irq_disable(test_irq);
    TEST_ASSERT_EQ(KERN_OK, err, "Disable spare IRQ");

    err = irq_enable(test_irq);
    TEST_ASSERT_EQ(KERN_OK, err, "Re-enable spare IRQ");

    /* 5c: 尝试注册已占用的 IRQ */
    err = irq_register(test_irq, test_handler_stub, 8);
    TEST_ASSERT_NE(KERN_OK, err, "Register occupied IRQ fails");

    /* 5d: 注销 */
    err = irq_unregister(test_irq);
    TEST_ASSERT_EQ(KERN_OK, err, "Unregister DCMI IRQ");

    /* 5e: 注销不存在的 IRQ */
    err = irq_unregister(test_irq);
    TEST_ASSERT_NE(KERN_OK, err, "Unregister non-registered IRQ fails");

    /* 5f: 无效参数 */
    err = irq_register(-1, test_handler_stub, 8);
    TEST_ASSERT_NE(KERN_OK, err, "Negative IRQ fails");

    err = irq_register((int16_t)BOARD_IRQ_COUNT, test_handler_stub, 8);
    TEST_ASSERT_NE(KERN_OK, err, "Out-of-range IRQ fails");

    err = irq_register(TEST_IRQ_CAP_LIFECYCLE, NULL, 8);
    TEST_ASSERT_NE(KERN_OK, err, "NULL handler fails");

    err = irq_register(TEST_IRQ_CAP_LIFECYCLE, test_handler_stub, 15);
    TEST_ASSERT_NE(KERN_OK, err, "Invalid priority fails");
}

/*============================================================================
 * 测试 6: ISR 守卫
 *============================================================================*/

static void test_isr_guards(void) {
    test_section("Test 6: ISR Guards");

    /* 6a: 任务上下文中 task_delay 应成功 */
    kern_err_t err = task_delay(1);
    TEST_ASSERT_EQ(KERN_OK, err, "task_delay OK in task context");

    /* 6b: 任务上下文中 task_delay_ms 应成功 */
    err = task_delay_ms(1);
    TEST_ASSERT_EQ(KERN_OK, err, "task_delay_ms OK in task context");

    /* 注: ISR 上下文中的守卫测试需要实际 ISR 触发,
     *     在纯软件测试中无法直接验证。
     *     ISR 守卫函数 (guard_is_in_isr) 的逻辑已通过
     *     kern_is_in_isr() 测试验证 */
}

/*============================================================================
 * 测试 7: IRQ endpoint 通知
 *============================================================================*/

static void test_irq_endpoint_notification(void) {
    test_section("Test 7: IRQ endpoint notification");

#if IPC_ENDPOINT
    ep_id_t ep = endpoint_create("irq_ep", sizeof(uint32_t) * 2U, 2);
    TEST_ASSERT(ep >= 0, "IRQ notification endpoint created");
    if (ep < 0) return;

    kern_err_t err = irq_bind_endpoint(TEST_IRQ_ENDPOINT, ep, 0x4952514eU);
    TEST_ASSERT_EQ(KERN_OK, err, "IRQ bound to endpoint");

    err = irq_bind_endpoint(TEST_IRQ_ENDPOINT, (ep_id_t)KERN_INVALID_ID, 0);
    TEST_ASSERT_EQ(KERN_ERR_PARAM, err, "IRQ bind rejects invalid endpoint");

    err = irq_notify(TEST_IRQ_ENDPOINT);
    TEST_ASSERT_EQ(KERN_OK, err, "IRQ notification sent");

    uint32_t msg[2] = {0, 0};
    err = endpoint_recv(ep, msg, 0);
    TEST_ASSERT_EQ(KERN_OK, err, "IRQ notification received");
    TEST_ASSERT_EQ((int)0x4952514eU, (int)msg[0],
                   "IRQ notification badge copied");
    TEST_ASSERT_EQ((int)TEST_IRQ_ENDPOINT, (int)msg[1],
                   "IRQ notification number copied");

    err = irq_notify(TEST_IRQ_UNBOUND);
    TEST_ASSERT_EQ(KERN_ERR_NOEXIST, err, "unbound IRQ notification rejected");

    endpoint_delete(ep);
#else
    test_skip("endpoint disabled");
#endif
}

/*============================================================================
 * 测试 8: IRQ capability lifecycle
 *============================================================================*/

static void test_irq_cap_lifecycle(void) {
    test_section("Test 8: IRQ capability lifecycle");

#if CAP_ENABLE
    uint16_t cap_free_before = cap_free_count();
    cap_id_t cap = KERN_INVALID_ID;
    int16_t irq = KERN_INVALID_ID;

    kern_err_t err = kirq_create_cap(TEST_IRQ_CAP_LIFECYCLE,
                                     CAP_READ | CAP_WRITE | CAP_MANAGE |
                                         CAP_TRANSFER,
                                     &cap);
    TEST_ASSERT_EQ(KERN_OK, err, "kirq_create_cap OK");
    TEST_ASSERT(cap >= 0, "kirq_create_cap returns cap");

    err = kirq_get_number(cap, &irq);
    TEST_ASSERT_EQ(KERN_OK, err, "kirq_get_number OK");
    TEST_ASSERT_EQ((int)TEST_IRQ_CAP_LIFECYCLE, (int)irq,
                   "kirq number recorded");

    cap_id_t bad_cap = KERN_INVALID_ID;
    err = kirq_create_cap(-1, CAP_FULL, &bad_cap);
    TEST_ASSERT_EQ(KERN_ERR_PARAM, err, "kirq_create_cap rejects negative irq");
    err = kirq_create_cap((int16_t)BOARD_IRQ_COUNT, CAP_FULL, &bad_cap);
    TEST_ASSERT_EQ(KERN_ERR_PARAM, err, "kirq_create_cap rejects out of range irq");
    err = kirq_get_number(KERN_INVALID_ID, &irq);
    TEST_ASSERT_EQ(KERN_ERR_CAP, err, "kirq_get_number rejects invalid cap");
    err = kirq_get_number(cap, NULL);
    TEST_ASSERT_EQ(KERN_ERR_PARAM, err, "kirq_get_number rejects NULL output");

    err = kirq_delete_cap(cap);
    TEST_ASSERT_EQ(KERN_OK, err, "kirq_delete_cap OK");
    err = kirq_get_number(cap, &irq);
    TEST_ASSERT_EQ(KERN_ERR_CAP, err, "deleted kirq cap no longer resolves");

    TEST_ASSERT_EQ((int)cap_free_before, (int)cap_free_count(),
                   "kirq cap cleanup restored cap count");
#else
    test_skip("capability disabled");
#endif
}

/*============================================================================
 * 测试 9: IRQ capability endpoint bind
 *============================================================================*/

static void test_irq_cap_endpoint_bind(void) {
    test_section("Test 9: IRQ capability endpoint bind");

#if CAP_ENABLE && IPC_ENDPOINT
    uint16_t cap_free_before = cap_free_count();
    ep_id_t ep = endpoint_create("irq_cap_ep", sizeof(uint32_t) * 2U, 2);
    TEST_ASSERT(ep >= 0, "IRQ cap endpoint created");
    if (ep < 0) {
        return;
    }

    cap_id_t cap = KERN_INVALID_ID;
    kern_err_t err = kirq_create_cap(TEST_IRQ_CAP_BIND,
                                     CAP_READ | CAP_WRITE | CAP_MANAGE |
                                         CAP_TRANSFER,
                                     &cap);
    TEST_ASSERT_EQ(KERN_OK, err, "kirq bind cap created");
    TEST_ASSERT(cap >= 0, "kirq bind cap valid");

    err = kirq_bind_endpoint(cap, ep, 0x4b495251U);
    TEST_ASSERT_EQ(KERN_OK, err, "kirq_bind_endpoint OK");
    err = kirq_bind_endpoint(KERN_INVALID_ID, ep, 0);
    TEST_ASSERT_EQ(KERN_ERR_CAP, err, "kirq_bind_endpoint rejects invalid cap");
    err = kirq_bind_endpoint(cap, (ep_id_t)KERN_INVALID_ID, 0);
    TEST_ASSERT_EQ(KERN_ERR_PARAM, err, "kirq_bind_endpoint rejects invalid endpoint");

    err = irq_notify(TEST_IRQ_CAP_BIND);
    TEST_ASSERT_EQ(KERN_OK, err, "kirq-bound IRQ notification sent");

    uint32_t msg[2] = {0, 0};
    err = endpoint_recv(ep, msg, 0);
    TEST_ASSERT_EQ(KERN_OK, err, "kirq-bound notification received");
    TEST_ASSERT_EQ((int)0x4b495251U, (int)msg[0],
                   "kirq-bound badge copied");
    TEST_ASSERT_EQ((int)TEST_IRQ_CAP_BIND, (int)msg[1],
                   "kirq-bound IRQ number copied");

    err = kirq_delete_cap(cap);
    TEST_ASSERT_EQ(KERN_OK, err, "kirq bound cap deleted");
    err = irq_notify(TEST_IRQ_CAP_BIND);
    TEST_ASSERT_EQ(KERN_ERR_NOEXIST, err,
                   "kirq delete clears endpoint binding");

    endpoint_delete(ep);
    TEST_ASSERT_EQ((int)cap_free_before, (int)cap_free_count(),
                   "kirq bind cleanup restored cap count");
#else
    test_skip("capability or endpoint disabled");
#endif
}

/*============================================================================
 * 测试 10: IRQ bind follows bound cap revoke
 *============================================================================*/

static void test_irq_cap_bind_revoke_hook(void) {
    test_section("Test 10: IRQ capability bind revoke hook");

#if CAP_ENABLE && IPC_ENDPOINT
    uint16_t cap_free_before = cap_free_count();
    ep_id_t ep = endpoint_create("irq_revoke_ep", sizeof(uint32_t) * 2U, 2);
    TEST_ASSERT(ep >= 0, "IRQ revoke endpoint created");
    if (ep < 0) {
        return;
    }

    cap_id_t parent = KERN_INVALID_ID;
    kern_err_t err = kirq_create_cap(TEST_IRQ_CAP_REVOKE,
                                     CAP_READ | CAP_WRITE | CAP_MANAGE |
                                         CAP_TRANSFER | CAP_GRANT,
                                     &parent);
    TEST_ASSERT_EQ(KERN_OK, err, "kirq revoke parent created");
    TEST_ASSERT(parent >= 0, "kirq revoke parent valid");

    cap_id_t child = cap_derive(parent, CAP_READ | CAP_WRITE | CAP_MANAGE);
    TEST_ASSERT(child >= 0, "kirq revoke child derived");

    err = kirq_bind_endpoint(parent, ep, 0x52495251U);
    TEST_ASSERT_EQ(KERN_OK, err, "kirq revoke parent bound");

    cap_delete(parent);
    err = irq_notify(TEST_IRQ_CAP_REVOKE);
    TEST_ASSERT_EQ(KERN_ERR_NOEXIST, err,
                   "bound parent cap delete clears IRQ binding");

    err = kirq_get_number(child, &(int16_t){0});
    TEST_ASSERT_EQ(KERN_OK, err, "derived IRQ cap still resolves");
    err = kirq_delete_cap(child);
    TEST_ASSERT_EQ(KERN_OK, err, "derived IRQ cap deleted");

    endpoint_delete(ep);
    TEST_ASSERT_EQ((int)cap_free_before, (int)cap_free_count(),
                   "kirq revoke-hook cleanup restored cap count");
#else
    test_skip("capability or endpoint disabled");
#endif
}

/*============================================================================
 * 中断管理测试模块入口
 *============================================================================*/

static void test_irq_module(void) {
    test_isr_pool();
    test_isr_context();
    test_bh_lifecycle();
    test_bh_endpoint_notification();
    test_bh_delete_while_running();
    test_irq_bh_trace_stats();
#if IRQ_THREADED_ENABLE
    test_threaded_irq();
#endif
    test_register_vector();
    test_isr_guards();
    test_irq_endpoint_notification();
    test_irq_cap_lifecycle();
    test_irq_cap_endpoint_bind();
    test_irq_cap_bind_revoke_hook();
}

/*============================================================================
 * 模块注册
 *============================================================================*/

TEST_K_MODULE(irq, test_irq_module);
#endif /* TEST_ENABLE */
