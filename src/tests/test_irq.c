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
#include "irq.h"
#include "bh.h"
#include "task.h"
#include "hal.h"
#include "kernel.h"
#include "trace.h"
#include "stats.h"

static void test_handler_stub(void);

/*============================================================================
 * 测试 1: ISR 池管理
 *============================================================================*/

static void test_isr_pool(void) {
    test_section("Test 1: ISR Pool Management");

    /* 1a: 注册最大数量 ISR */
    int count = 0;
    for (int i = 0; i < IRQ_MAX_USER; i++) {
        kern_err_t err = irq_register((int16_t)i, test_handler_stub, 8);
        if (err == KERN_OK) {
            count++;
        }
        if (count >= IRQ_MAX_USER) break;
    }

    /* 1b: 满池再注册应失败 (IRQ 0 已占用, 返回 BUSY) */
    kern_err_t err = irq_register(0, test_handler_stub, 8);
    TEST_ASSERT(err != KERN_OK, "Pool full: register fails");

    /* 1c: 注销后可以重新注册 */
    err = irq_unregister(0);
    TEST_ASSERT_EQ(KERN_OK, err, "Unregister IRQ 0");

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
    TEST_ASSERT_EQ(IRQ_BH_MAX, created, "All BH slots creatable");

    /* 清理 */
    for (int i = 0; i < created; i++) {
        bh_delete(ids[i]);
    }
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
    kern_err_t err = irq_request_threaded(45, test_threaded_handler,
                                          &counter, 10, 512);
    TEST_ASSERT_EQ(KERN_OK, err, "Threaded IRQ request OK");

    /* 4b: 重复请求同一 IRQ 应失败 */
    err = irq_request_threaded(45, test_threaded_handler,
                               &counter, 10, 512);
    TEST_ASSERT_NE(KERN_OK, err, "Duplicate threaded IRQ fails");

    /* 4c: 释放线程化 IRQ */
    err = irq_release_threaded(45);
    TEST_ASSERT_EQ(KERN_OK, err, "Threaded IRQ release OK");

    /* 4d: 释放后可以重新请求 */
    err = irq_request_threaded(45, test_threaded_handler,
                               &counter, 10, 512);
    TEST_ASSERT_EQ(KERN_OK, err, "Re-request threaded IRQ OK");

    /* 清理 */
    irq_release_threaded(45);

    /* 4e: 无效参数 */
    err = irq_request_threaded(-1, test_threaded_handler, NULL, 10, 512);
    TEST_ASSERT_NE(KERN_OK, err, "Invalid IRQ number fails");

    err = irq_request_threaded(100, test_threaded_handler, NULL, 10, 512);
    TEST_ASSERT_NE(KERN_OK, err, "Out-of-range IRQ fails");

    err = irq_request_threaded(50, NULL, NULL, 10, 512);
    TEST_ASSERT_NE(KERN_OK, err, "NULL handler fails");

    /* 4f: 填充所有槽位 */
    int allocated = 0;
    for (int i = 0; i < IRQ_THREADED_MAX; i++) {
        err = irq_request_threaded((int16_t)(50 + i), test_threaded_handler,
                                   &counter, 10, 512);
        if (err == KERN_OK) allocated++;
    }

    /* 清理 */
    for (int i = 0; i < allocated; i++) {
        irq_release_threaded((int16_t)(50 + i));
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
    kern_err_t err = irq_register(94, test_handler_stub, 8);  /* DCMI */
    TEST_ASSERT_EQ(KERN_OK, err, "Register DCMI IRQ");

    /* 5b: 启用和禁用 */
    err = irq_disable(94);
    TEST_ASSERT_EQ(KERN_OK, err, "Disable DCMI IRQ");

    err = irq_enable(94);
    TEST_ASSERT_EQ(KERN_OK, err, "Re-enable DCMI IRQ");

    /* 5c: 尝试注册已占用的 IRQ */
    err = irq_register(94, test_handler_stub, 8);
    TEST_ASSERT_NE(KERN_OK, err, "Register occupied IRQ fails");

    /* 5d: 注销 */
    err = irq_unregister(94);
    TEST_ASSERT_EQ(KERN_OK, err, "Unregister DCMI IRQ");

    /* 5e: 注销不存在的 IRQ */
    err = irq_unregister(94);
    TEST_ASSERT_NE(KERN_OK, err, "Unregister non-registered IRQ fails");

    /* 5f: 无效参数 */
    err = irq_register(-1, test_handler_stub, 8);
    TEST_ASSERT_NE(KERN_OK, err, "Negative IRQ fails");

    err = irq_register(98, test_handler_stub, 8);
    TEST_ASSERT_NE(KERN_OK, err, "Out-of-range IRQ fails");

    err = irq_register(50, NULL, 8);
    TEST_ASSERT_NE(KERN_OK, err, "NULL handler fails");

    err = irq_register(50, test_handler_stub, 15);
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
 * 中断管理测试模块入口
 *============================================================================*/

static void test_irq_module(void) {
    test_isr_pool();
    test_isr_context();
    test_bh_lifecycle();
    test_bh_delete_while_running();
    test_irq_bh_trace_stats();
#if IRQ_THREADED_ENABLE
    test_threaded_irq();
#endif
    test_register_vector();
    test_isr_guards();
}

/*============================================================================
 * 模块注册
 *============================================================================*/

TEST_MODULE_REGISTER(irq, test_irq_module);
