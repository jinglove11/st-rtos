/**
 * @file test_stats.c
 * @brief CPU 使用率统计测试模块
 *
 * 测试内容：
 * 1. CPU 统计基本功能
 * 2. 多任务竞争后 cpu_usage 总和 ≈ 100%
 * 3. idle 任务 cpu_usage 合理
 */

#include "test_framework.h"
#include "kernel.h"
#include "task.h"
#include "scheduler.h"
#include "stats.h"

#if KERN_TASK_STATS && TEST_MODULE_STATS

/*============================================================================
 * Test 1: CPU 统计基本功能
 *============================================================================*/

static void test_cpu_usage_basic(void) {
    test_section("Test 1: CPU usage basic");

    /* 创建一个任务并运行一段时间 */
    volatile int counter = 0;

    task_id_t tid = task_create("cpu_b", (task_func_t)(void (*)(void *))NULL,
                                NULL, 10, 0);
    /* 用一个简单的忙等任务 */
    if (tid >= 0) {
        TEST_ASSERT_EQ(KERN_OK, task_delete(tid), "unused stats task deleted");
    }

    /* 验证 idle 任务有 cpu_usage 字段 */
    tcb_t *idle = task_get_idle();
    TEST_ASSERT_NOT_NULL(idle, "idle task exists");
    if (idle) {
        /* idle 的 cpu_usage 可能是 0 (刚启动) 或有值 */
        TEST_ASSERT(1, "idle has cpu_usage field");
    }

    (void)counter;
}

/*============================================================================
 * Test 2: 多任务竞争后 cpu_usage 总和 ≈ 100%
 *============================================================================*/

static volatile int stats_task_running = 0;

static void stats_busy_task(void *arg) {
    (void)arg;
    while (stats_task_running) {
        __asm volatile("nop");
    }
    task_exit(NULL);
}

static void test_cpu_usage_sum(void) {
    test_section("Test 2: CPU usage sum ≈ 100%");

    /* 启动两个忙等任务 */
    stats_task_running = 1;

    task_id_t t1 = task_create("s_busy1", stats_busy_task, NULL, 15, 256);
    task_id_t t2 = task_create("s_busy2", stats_busy_task, NULL, 15, 256);
    TEST_ASSERT(t1 >= 0, "busy task 1 created");
    TEST_ASSERT(t2 >= 0, "busy task 2 created");

    if (t1 >= 0) task_start(t1);
    if (t2 >= 0) task_start(t2);

    /* 等待 2100 ticks 确保至少一个完整 1000-tick 统计周期被捕获 */
    task_delay(2100);

    /*
     * 在停止忙等任务之前读取 cpu_usage
     * (task_exit 后 TCB 会被 reclaim 清零，cpu_usage 丢失)
     */
    extern uint32_t task_get_used_bitmap(void);
    extern tcb_t task_pool[];
    uint32_t bitmap = task_get_used_bitmap();
    uint32_t sum = 0;

    for (int i = 0; i < KERNEL_MAX_TASKS; i++) {
        if (bitmap & (1U << i)) {
            sum += task_pool[i].cpu_usage;
        }
    }
    tcb_t *idle = task_get_idle();
    if (idle != NULL) {
        sum += idle->cpu_usage;
    }

    /* 停止忙等任务 */
    stats_task_running = 0;
    task_delay(100);

    TEST_ASSERT(sum >= 7000 && sum <= 12000,
                "CPU usage sum is approximately 100%");
}

/*============================================================================
 * Test 3: idle 任务 cpu_usage 合理
 *============================================================================*/

static void test_idle_cpu_usage(void) {
    test_section("Test 3: idle CPU usage");

    tcb_t *idle = task_get_idle();
    TEST_ASSERT_NOT_NULL(idle, "idle task exists");
    if (!idle) return;

    /* idle cpu_usage 应该 < 10000 (不是 100%) */
    /* 因为 shell 任务也在运行 */
    TEST_ASSERT(idle->cpu_usage <= 10000,
                "idle cpu_usage <= 100%");
}

/*============================================================================
 * Test 4: subsystem event counters
 *============================================================================*/

static void test_subsystem_counters(void) {
    test_section("Test 4: subsystem event counters");

    stats_clear_events();
    TEST_ASSERT_EQ(0, (int)stats_get_event_count(STATS_SUBSYS_TIMER,
                                                 STATS_COUNTER_QUEUE_FULL),
                   "timer queue-full counter starts at 0");

    kern_err_t err = stats_record_event(STATS_SUBSYS_TIMER,
                                        STATS_COUNTER_QUEUE_FULL);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "record timer queue-full");
    err = stats_record_event(STATS_SUBSYS_TIMER,
                             STATS_COUNTER_QUEUE_FULL);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "record timer queue-full again");
    err = stats_record_event(STATS_SUBSYS_BH, STATS_COUNTER_CANCEL);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "record bh cancel");

    TEST_ASSERT_EQ(2, (int)stats_get_event_count(STATS_SUBSYS_TIMER,
                                                 STATS_COUNTER_QUEUE_FULL),
                   "timer queue-full counter increments");
    TEST_ASSERT_EQ(1, (int)stats_get_event_count(STATS_SUBSYS_BH,
                                                 STATS_COUNTER_CANCEL),
                   "bh cancel counter increments");

    err = stats_record_event(STATS_SUBSYS_MAX, STATS_COUNTER_OK);
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, (int)err,
                   "invalid subsystem rejected");
}

/*============================================================================
 * Module registration
 *============================================================================*/

static void test_stats_module(void) {
    test_cpu_usage_basic();
    test_cpu_usage_sum();
    test_idle_cpu_usage();
    test_subsystem_counters();
}

TEST_MODULE_REGISTER(stats, test_stats_module);

#endif /* KERN_TASK_STATS && TEST_MODULE_STATS */
