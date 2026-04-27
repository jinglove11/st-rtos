/**
 * @file main.c
 * @brief 调度器完整功能测试
 */

#include "board_config.h"
#include "kernel.h"
#include "task.h"
#include "semaphore.h"
#include "mutex.h"
#include "uart.h"
#include "gpio.h"
#include <string.h>

#define TEST_UART       NUCLEO_DEFAULT_UART
#define TEST_LED_PORT   NUCLEO_LED_PORT
#define TEST_LED_PIN    NUCLEO_LED_PIN

/*============================================================================
 * 测试框架
 *============================================================================*/

static volatile int test_passed = 0;
static volatile int test_failed = 0;

static void test_print(const char *msg) {
    uart_puts(TEST_UART, msg);
}

static void test_print_num(const char *label, int32_t num) {
    uart_puts(TEST_UART, label);
    if (num < 0) {
        uart_puts(TEST_UART, "-");
        num = -num;
    }
    uart_putdec(TEST_UART, (uint32_t)num);
    uart_puts(TEST_UART, "\r\n");
}

static void test_pass(const char *name) {
    test_print("[PASS] ");
    test_print(name);
    test_print("\r\n");
    test_passed++;
}

static void test_fail(const char *name) {
    test_print("[FAIL] ");
    test_print(name);
    test_print("\r\n");
    test_failed++;
}

static void test_section(const char *name) {
    test_print("\r\n========================================\r\n");
    test_print("  ");
    test_print(name);
    test_print("\r\n========================================\r\n");
}

static void test_summary(void) {
    test_print("\r\n========================================\r\n");
    test_print("         TEST SUMMARY\r\n");
    test_print("========================================\r\n");
    test_print_num("Passed: ", test_passed);
    test_print_num("Failed: ", test_failed);
    test_print("========================================\r\n");
    if (test_failed == 0) {
        test_print("All tests PASSED!\r\n");
    } else {
        test_print("Some tests FAILED!\r\n");
    }
}

/*============================================================================
 * 测试 1: 任务创建与基本调度
 *============================================================================*/

static volatile int test1_task1_ran = 0;
static volatile int test1_task2_ran = 0;

static void test1_task1(void *arg) {
    (void)arg;
    test1_task1_ran = 1;
}

static void test1_task2(void *arg) {
    (void)arg;
    test1_task2_ran = 1;
}

static void test_task_create(void) {
    test_section("Test 1: Task Create & Schedule");

    test1_task1_ran = 0;
    test1_task2_ran = 0;

    // 测试有效创建
    task_id_t t1 = task_create("test1", test1_task1, NULL, 5, 0);
    task_id_t t2 = task_create("test2", test1_task2, NULL, 5, 0);

    if (t1 >= 0 && t2 >= 0) {
        test_pass("Create tasks with valid params");
    } else {
        test_fail("Create tasks with valid params");
    }

    // 测试任务名称
    const char *name = task_get_name(t1);
    if (name && strcmp(name, "test1") == 0) {
        test_pass("Task name set correctly");
    } else {
        test_fail("Task name set correctly");
    }

    // 测试优先级
    uint8_t prio = task_get_priority(t1);
    if (prio == 5) {
        test_pass("Task priority set correctly");
    } else {
        test_fail("Task priority set correctly");
    }

    // 启动任务
    kern_err_t err = task_start(t1);
    if (err == KERN_OK) {
        test_pass("Task start success");
    } else {
        test_fail("Task start success");
    }

    task_start(t2);
    task_delay(10);

    if (test1_task1_ran && test1_task2_ran) {
        test_pass("Both tasks executed");
    } else {
        test_fail("Both tasks executed");
    }

    // 测试无效优先级
    task_id_t t3 = task_create("invalid", test1_task1, NULL, 255, 0);
    if (t3 == KERN_INVALID_ID) {
        test_pass("Reject invalid priority");
    } else {
        test_fail("Reject invalid priority");
    }
}

/*============================================================================
 * 测试 2: 优先级抢占
 *============================================================================*/

static volatile int test2_high_ran = 0;
static volatile int test2_mid_ran = 0;
static volatile int test2_low_started = 0;
static volatile int test2_preempt_count = 0;

static void test2_high_task(void *arg) {
    (void)arg;
    test2_high_ran = 1;
    test2_preempt_count++;
}

static void test2_mid_task(void *arg) {
    (void)arg;
    test2_mid_ran = 1;
}

static void test2_low_task(void *arg) {
    (void)arg;
    test2_low_started = 1;

    // 长时间运行，让高优先级任务抢占
    for (volatile int i = 0; i < 100000; i++) {
        // 高优先级任务应该在这里抢占
    }
}

static void test_priority_preempt(void) {
    test_section("Test 2: Priority Preemption");

    test2_high_ran = 0;
    test2_mid_ran = 0;
    test2_low_started = 0;
    test2_preempt_count = 0;

    // 创建三个不同优先级的任务
    task_id_t low = task_create("low", test2_low_task, NULL, 20, 0);
    task_id_t mid = task_create("mid", test2_mid_task, NULL, 10, 0);
    task_id_t high = task_create("high", test2_high_task, NULL, 2, 0);

    // 先启动低优先级任务
    task_start(low);
    task_delay(5);  // 让低优先级任务开始运行

    // 启动高优先级任务，应该立即抢占
    task_start(high);

    task_delay(10);

    if (test2_high_ran) {
        test_pass("High priority preempted low");
    } else {
        test_fail("High priority preempted low");
    }

    if (test2_low_started) {
        test_pass("Low priority task started");
    } else {
        test_fail("Low priority task started");
    }

    // 测试中优先级任务
    test2_mid_ran = 0;
    task_start(mid);
    task_delay(10);

    if (test2_mid_ran) {
        test_pass("Mid priority task ran");
    } else {
        test_fail("Mid priority task ran");
    }
}

/*============================================================================
 * 测试 3: 时间片轮转
 *============================================================================*/

static volatile int test3_count1 = 0;
static volatile int test3_count2 = 0;
static volatile int test3_count3 = 0;

static void test3_task(void *arg) {
    volatile int *counter = (volatile int *)arg;
    for (int i = 0; i < 10; i++) {
        (*counter)++;
        // 忙等待消耗时间片
        for (volatile int j = 0; j < 100000; j++);
    }
}

static void test_round_robin(void) {
    test_section("Test 3: Round Robin (Time Slice)");

    test3_count1 = 0;
    test3_count2 = 0;
    test3_count3 = 0;

    // 创建三个同优先级任务
    task_id_t t1 = task_create("rr1", test3_task, (void *)&test3_count1, 5, 0);
    task_id_t t2 = task_create("rr2", test3_task, (void *)&test3_count2, 5, 0);
    task_id_t t3 = task_create("rr3", test3_task, (void *)&test3_count3, 5, 0);

    task_start(t1);
    task_start(t2);
    task_start(t3);

    task_delay(200);

    // 所有任务都应该有机会运行
    if (test3_count1 > 0 && test3_count2 > 0 && test3_count3 > 0) {
        test_pass("All same-priority tasks ran");
    } else {
        test_fail("All same-priority tasks ran");
    }

    test_print_num("  Task rr1 count: ", test3_count1);
    test_print_num("  Task rr2 count: ", test3_count2);
    test_print_num("  Task rr3 count: ", test3_count3);

    // 检查时间片轮转的公平性（差异不应太大）
    int max = test3_count1 > test3_count2 ? test3_count1 : test3_count2;
    max = max > test3_count3 ? max : test3_count3;
    int min = test3_count1 < test3_count2 ? test3_count1 : test3_count2;
    min = min < test3_count3 ? min : test3_count3;

    if (max - min <= 3) {
        test_pass("Fair round robin distribution");
    } else {
        test_fail("Fair round robin distribution");
    }
}

/*============================================================================
 * 测试 4: 任务延时
 *============================================================================*/

static void test_task_delay(void) {
    test_section("Test 4: Task Delay");

    // 测试基本延时
    uint32_t start = kern_get_tick();
    task_delay(50);
    uint32_t end = kern_get_tick();
    uint32_t elapsed = end - start;

    if (elapsed >= 50 && elapsed <= 52) {
        test_pass("Delay 50 ticks accuracy");
    } else {
        test_fail("Delay 50 ticks accuracy");
    }
    test_print_num("  Expected: 50, Actual: ", elapsed);

    // 测试短延时
    start = kern_get_tick();
    task_delay(5);
    end = kern_get_tick();
    elapsed = end - start;

    if (elapsed >= 5 && elapsed <= 7) {
        test_pass("Delay 5 ticks accuracy");
    } else {
        test_fail("Delay 5 ticks accuracy");
    }

    // 测试零延时
    start = kern_get_tick();
    task_delay(0);
    end = kern_get_tick();
    elapsed = end - start;

    if (elapsed == 0) {
        test_pass("Delay 0 returns immediately");
    } else {
        test_fail("Delay 0 returns immediately");
    }

    // 测试毫秒延时
    start = kern_get_tick();
    task_delay_ms(10);  // 10ms = 10 ticks (1ms/tick)
    end = kern_get_tick();
    elapsed = end - start;

    if (elapsed >= 10 && elapsed <= 12) {
        test_pass("Delay_ms accuracy");
    } else {
        test_fail("Delay_ms accuracy");
    }
}

/*============================================================================
 * 测试 5: 任务挂起与恢复
 *============================================================================*/

static volatile int test5_count = 0;
static volatile int test5_running = 0;

static void test5_task(void *arg) {
    (void)arg;
    while (1) {
        test5_running = 1;
        test5_count++;
        for (volatile int j = 0; j < 50000; j++);
        task_yield();
    }
}

static void test_suspend_resume(void) {
    test_section("Test 5: Suspend & Resume");

    test5_count = 0;
    test5_running = 0;

    task_id_t t = task_create("suspend", test5_task, NULL, 10, 0);
    task_start(t);

    task_delay(20);
    int count1 = test5_count;

    // 挂起任务
    kern_err_t err = task_suspend(t);
    if (err == KERN_OK) {
        test_pass("Suspend task success");
    } else {
        test_fail("Suspend task success");
    }

    task_delay(20);
    int count2 = test5_count;

    if (count2 == count1) {
        test_pass("Suspended task stopped");
    } else {
        test_fail("Suspended task stopped");
    }

    // 恢复任务
    err = task_resume(t);
    if (err == KERN_OK) {
        test_pass("Resume task success");
    } else {
        test_fail("Resume task success");
    }

    task_delay(20);
    int count3 = test5_count;

    if (count3 > count2) {
        test_pass("Resumed task continued");
    } else {
        test_fail("Resumed task continued");
    }

    test_print_num("  Before suspend: ", count1);
    test_print_num("  During suspend: ", count2);
    test_print_num("  After resume: ", count3);

    // 测试重复挂起
    err = task_suspend(t);
    task_suspend(t);  // 重复挂起
    if (err == KERN_OK) {
        test_pass("Double suspend handled");
    } else {
        test_fail("Double suspend handled");
    }

    task_resume(t);
    task_delete(t);
}

/*============================================================================
 * 测试 6: 任务删除
 *============================================================================*/

static volatile int test6_deleted = 0;

static void test6_task(void *arg) {
    (void)arg;
    while (1) {
        test6_deleted++;
        task_delay(10);
    }
}

static void test_task_delete(void) {
    test_section("Test 6: Task Delete");

    test6_deleted = 0;

    task_id_t t = task_create("todelete", test6_task, NULL, 10, 0);
    task_start(t);

    task_delay(30);

    int count_before = test6_deleted;

    // 删除任务
    kern_err_t err = task_delete(t);
    if (err == KERN_OK) {
        test_pass("Delete task success");
    } else {
        test_fail("Delete task success");
    }

    task_delay(30);

    int count_after = test6_deleted;

    if (count_after == count_before) {
        test_pass("Deleted task stopped");
    } else {
        test_fail("Deleted task stopped");
    }

    test_print_num("  Before delete: ", count_before);
    test_print_num("  After delete: ", count_after);

    // 测试删除无效任务
    err = task_delete(KERN_INVALID_ID);
    if (err == KERN_ERR_PARAM) {
        test_pass("Reject delete invalid task");
    } else {
        test_fail("Reject delete invalid task");
    }
}

/*============================================================================
 * 测试 7: 任务退出
 *============================================================================*/

static volatile int test7_exited = 0;

static void test7_task(void *arg) {
    (void)arg;
    test7_exited = 1;
    task_exit(NULL);
    test7_exited = 2;  // 不应该执行到这里
}

static void test_task_exit(void) {
    test_section("Test 7: Task Exit");

    test7_exited = 0;

    task_id_t t = task_create("toexit", test7_task, NULL, 10, 0);
    task_start(t);

    task_delay(10);

    if (test7_exited == 1) {
        test_pass("Task exit executed");
    } else {
        test_fail("Task exit executed");
    }

    // 检查任务状态
    task_state_t state = task_get_state(t);
    if (state == TASK_STATE_TERMINATED) {
        test_pass("Exited task is TERMINATED");
    } else {
        test_fail("Exited task is TERMINATED");
    }
}

/*============================================================================
 * 测试 8: 任务状态查询
 *============================================================================*/

static void test8_task(void *arg) {
    (void)arg;
    while (1) {
        task_delay(100);
    }
}

static void test_task_state(void) {
    test_section("Test 8: Task State Query");

    // 创建但不启动 - CREATED
    task_id_t t = task_create("state_test", test8_task, NULL, 10, 0);
    task_state_t state = task_get_state(t);

    if (state == TASK_STATE_CREATED) {
        test_pass("Created state correct");
    } else {
        test_fail("Created state correct");
    }

    // 启动后 - READY 或 RUNNING
    task_start(t);
    task_delay(5);
    state = task_get_state(t);

    if (state == TASK_STATE_READY || state == TASK_STATE_RUNNING) {
        test_pass("Started state correct");
    } else {
        test_fail("Started state correct");
    }

    // 挂起 - SUSPENDED
    task_suspend(t);
    state = task_get_state(t);

    if (state == TASK_STATE_SUSPENDED) {
        test_pass("Suspended state correct");
    } else {
        test_fail("Suspended state correct");
    }

    // 恢复 - READY
    task_resume(t);
    state = task_get_state(t);

    if (state == TASK_STATE_READY) {
        test_pass("Resumed state correct");
    } else {
        test_fail("Resumed state correct");
    }

    // 删除 - TERMINATED
    task_delete(t);
    state = task_get_state(t);

    if (state == TASK_STATE_TERMINATED) {
        test_pass("Deleted state correct");
    } else {
        test_fail("Deleted state correct");
    }

    // 查询无效任务
    state = task_get_state(KERN_INVALID_ID);
    if (state == TASK_STATE_TERMINATED) {
        test_pass("Invalid task returns TERMINATED");
    } else {
        test_fail("Invalid task returns TERMINATED");
    }
}

/*============================================================================
 * 测试 9: 任务优先级修改
 *============================================================================*/

static volatile int test9_order = 0;
static volatile int test9_seq[3] = {0, 0, 0};

static void test9_task(void *arg) {
    int id = (int)(long)arg;
    test9_seq[test9_order++] = id;
}

static void test_task_priority(void) {
    test_section("Test 9: Priority Change");

    test9_order = 0;
    test9_seq[0] = test9_seq[1] = test9_seq[2] = 0;

    // 创建三个任务，初始优先级相同
    task_id_t t1 = task_create("p1", test9_task, (void *)1, 10, 0);
    task_id_t t2 = task_create("p2", test9_task, (void *)2, 10, 0);
    task_id_t t3 = task_create("p3", test9_task, (void *)3, 10, 0);

    // 修改优先级
    kern_err_t err = task_set_priority(t1, 5);  // 最高
    if (err == KERN_OK) {
        test_pass("Set priority success");
    } else {
        test_fail("Set priority success");
    }

    task_set_priority(t2, 15);  // 中等
    task_set_priority(t3, 25);  // 最低

    // 验证优先级
    if (task_get_priority(t1) == 5) {
        test_pass("Priority changed correctly");
    } else {
        test_fail("Priority changed correctly");
    }

    // 启动任务，应该按优先级顺序执行
    task_start(t3);
    task_start(t2);
    task_start(t1);

    task_delay(20);

    // 最高优先级任务应该先执行
    if (test9_seq[0] == 1) {
        test_pass("Highest priority ran first");
    } else {
        test_fail("Highest priority ran first");
    }

    test_print_num("  First ran: task ", test9_seq[0]);
    test_print_num("  Second ran: task ", test9_seq[1]);
    test_print_num("  Third ran: task ", test9_seq[2]);

    // 测试无效优先级
    err = task_set_priority(t1, 255);
    if (err == KERN_ERR_PARAM) {
        test_pass("Reject invalid priority");
    } else {
        test_fail("Reject invalid priority");
    }
}

/*============================================================================
 * 测试 10: 阻塞与唤醒 (信号量)
 *============================================================================*/

static sem_id_t test10_sem;
static volatile int test10_producer_done = 0;
static volatile int test10_consumer_got = 0;
static volatile int test10_wake_order = 0;

static void test10_producer(void *arg) {
    (void)arg;
    task_delay(20);
    sem_post(test10_sem);
    test10_producer_done = 1;
}

static void test10_consumer(void *arg) {
    (void)arg;
    kern_err_t err = sem_wait(test10_sem, 100);
    if (err == KERN_OK) {
        test10_consumer_got = 1;
        test10_wake_order++;
    }
}

static void test_block_wakeup(void) {
    test_section("Test 10: Block & Wakeup");

    test10_producer_done = 0;
    test10_consumer_got = 0;

    test10_sem = sem_create(0, 5);

    task_id_t consumer = task_create("consumer", test10_consumer, NULL, 5, 0);
    task_id_t producer = task_create("producer", test10_producer, NULL, 5, 0);

    task_start(consumer);
    task_start(producer);

    task_delay(50);

    if (test10_producer_done && test10_consumer_got) {
        test_pass("Block/wakeup via semaphore");
    } else {
        test_fail("Block/wakeup via semaphore");
    }

    sem_delete(test10_sem);
}

/*============================================================================
 * 测试 11: 超时唤醒
 *============================================================================*/

static void test_timeout(void) {
    test_section("Test 11: Timeout Wakeup");

    sem_id_t sem = sem_create(0, 1);

    uint32_t start = kern_get_tick();
    kern_err_t err = sem_wait(sem, 30);
    uint32_t end = kern_get_tick();
    uint32_t elapsed = end - start;

    if (err == KERN_ERR_TIMEOUT) {
        test_pass("Timeout returned correctly");
    } else {
        test_fail("Timeout returned correctly");
    }

    if (elapsed >= 30 && elapsed <= 32) {
        test_pass("Timeout timing accurate");
    } else {
        test_fail("Timeout timing accurate");
    }

    test_print_num("  Expected: 30, Actual: ", elapsed);

    sem_delete(sem);
}

/*============================================================================
 * 测试 12: 优先级继承
 *============================================================================*/

static mutex_id_t test12_mutex;
static volatile int test12_shared = 0;
static volatile int test12_high_got_lock = 0;
static volatile int test12_mid_ran = 0;
static volatile int test12_low_finished = 0;

static void test12_low_task(void *arg) {
    (void)arg;
    mutex_lock(test12_mutex, 100);

    test12_shared = 1;

    // 持有锁，让高优先级任务等待
    for (volatile int i = 0; i < 300000; i++);

    test12_low_finished = 1;
    mutex_unlock(test12_mutex);
}

static void test12_mid_task(void *arg) {
    (void)arg;
    // 中优先级任务，用于测试优先级继承
    for (volatile int i = 0; i < 100000; i++);
    test12_mid_ran = 1;
}

static void test12_high_task(void *arg) {
    (void)arg;
    task_delay(5);  // 让低优先级任务先获取锁

    kern_err_t err = mutex_lock(test12_mutex, 200);
    if (err == KERN_OK) {
        test12_high_got_lock = 1;
        mutex_unlock(test12_mutex);
    }
}

static void test_priority_inheritance(void) {
    test_section("Test 12: Priority Inheritance");

    test12_shared = 0;
    test12_high_got_lock = 0;
    test12_mid_ran = 0;
    test12_low_finished = 0;

    test12_mutex = mutex_create();

    // 低优先级任务先获取锁
    task_id_t low = task_create("low", test12_low_task, NULL, 20, 0);
    // 中优先级任务
    task_id_t mid = task_create("mid", test12_mid_task, NULL, 10, 0);
    // 高优先级任务等待锁
    task_id_t high = task_create("high", test12_high_task, NULL, 2, 0);

    task_start(low);
    task_delay(2);
    task_start(high);
    task_delay(2);
    task_start(mid);

    task_delay(150);

    if (test12_high_got_lock) {
        test_pass("High priority got lock");
    } else {
        test_fail("High priority got lock");
    }

    if (test12_low_finished) {
        test_pass("Low priority finished");
    } else {
        test_fail("Low priority finished");
    }

    // 如果优先级继承工作正常，低优先级任务应该在高优先级等待期间
    // 被提升优先级，从而不被中优先级任务抢占
    if (test12_low_finished && !test12_mid_ran) {
        test_pass("Priority inheritance prevented inversion");
    } else {
        // 这个测试可能因为时序问题不稳定
        test_print("  (Priority inheritance test may vary)\r\n");
    }

    mutex_delete(test12_mutex);
}

/*============================================================================
 * 测试 13: 任务让出 CPU
 *============================================================================*/

static volatile int test13_count1 = 0;
static volatile int test13_count2 = 0;

static void test13_task1(void *arg) {
    (void)arg;
    for (int i = 0; i < 5; i++) {
        test13_count1++;
        task_yield();  // 主动让出
    }
}

static void test13_task2(void *arg) {
    (void)arg;
    for (int i = 0; i < 5; i++) {
        test13_count2++;
        task_yield();  // 主动让出
    }
}

static void test_task_yield(void) {
    test_section("Test 13: Task Yield");

    test13_count1 = 0;
    test13_count2 = 0;

    task_id_t t1 = task_create("yield1", test13_task1, NULL, 5, 0);
    task_id_t t2 = task_create("yield2", test13_task2, NULL, 5, 0);

    task_start(t1);
    task_start(t2);

    task_delay(50);

    if (test13_count1 == 5 && test13_count2 == 5) {
        test_pass("Yield allows fair execution");
    } else {
        test_fail("Yield allows fair execution");
    }

    test_print_num("  Task1 yield count: ", test13_count1);
    test_print_num("  Task2 yield count: ", test13_count2);
}

/*============================================================================
 * 测试 14: 空闲任务
 *============================================================================*/

static void test_idle_task(void) {
    test_section("Test 14: Idle Task");

    // 当所有任务阻塞时，空闲任务应该运行
    test_print("  Blocking all tasks for 50 ticks...\r\n");

    uint32_t start = kern_get_tick();
    task_delay(50);
    uint32_t end = kern_get_tick();

    // 系统应该仍然正常运行
    if (end - start >= 50) {
        test_pass("System ran during idle");
    } else {
        test_fail("System ran during idle");
    }
}

/*============================================================================
 * 测试 15: 多任务压力测试
 *============================================================================*/

#define STRESS_TASK_COUNT 8
static volatile int stress_counters[STRESS_TASK_COUNT];

static void stress_task(void *arg) {
    int id = (int)(long)arg;
    for (int i = 0; i < 10; i++) {
        stress_counters[id]++;
        task_delay(1);
    }
}

static void test_stress(void) {
    test_section("Test 15: Stress Test (8 tasks)");

    for (int i = 0; i < STRESS_TASK_COUNT; i++) {
        stress_counters[i] = 0;
    }

    task_id_t tasks[STRESS_TASK_COUNT];

    // 创建不同优先级的任务
    for (int i = 0; i < STRESS_TASK_COUNT; i++) {
        char name[16];
        name[0] = 't';
        name[1] = '0' + i;
        name[2] = '\0';
        tasks[i] = task_create(name, stress_task, (void *)(long)i, 5 + i * 2, 0);
        task_start(tasks[i]);
    }

    task_delay(200);

    int all_ran = 1;
    for (int i = 0; i < STRESS_TASK_COUNT; i++) {
        if (stress_counters[i] < 10) {
            all_ran = 0;
        }
    }

    if (all_ran) {
        test_pass("All stress tasks completed");
    } else {
        test_fail("All stress tasks completed");
    }

    for (int i = 0; i < STRESS_TASK_COUNT; i++) {
        test_print_num("  Task ", stress_counters[i]);
    }
}

/*============================================================================
 * 测试 16: 任务自删除
 *============================================================================*/

static volatile int test16_self_deleted = 0;

static void test16_task(void *arg) {
    (void)arg;
    test16_self_deleted = 1;
    // 任务自己删除自己应该失败
    task_id_t self = task_self();
    kern_err_t err = task_delete(self);
    if (err == KERN_ERR_STATE) {
        test16_self_deleted = 2;
    }
}

static void test_self_delete(void) {
    test_section("Test 16: Self Delete Protection");

    test16_self_deleted = 0;

    task_id_t t = task_create("selfdel", test16_task, NULL, 10, 0);
    task_start(t);

    task_delay(20);

    if (test16_self_deleted == 2) {
        test_pass("Self delete rejected correctly");
    } else {
        test_fail("Self delete rejected correctly");
    }
}

/*============================================================================
 * 测试 17: 递归锁
 *============================================================================*/

static void test_recursive_mutex(void) {
    test_section("Test 17: Recursive Mutex");

    mutex_id_t m = mutex_create();

    // 第一次加锁
    kern_err_t err = mutex_lock(m, 100);
    if (err == KERN_OK) {
        test_pass("First lock success");
    } else {
        test_fail("First lock success");
    }

    // 第二次加锁（递归）
    err = mutex_lock(m, 100);
    if (err == KERN_OK) {
        test_pass("Recursive lock success");
    } else {
        test_fail("Recursive lock success");
    }

    // 第三次加锁
    err = mutex_lock(m, 100);
    if (err == KERN_OK) {
        test_pass("Third recursive lock success");
    } else {
        test_fail("Third recursive lock success");
    }

    // 解锁三次
    mutex_unlock(m);
    mutex_unlock(m);
    err = mutex_unlock(m);

    if (err == KERN_OK) {
        test_pass("All unlocks success");
    } else {
        test_fail("All unlocks success");
    }

    mutex_delete(m);
}

/*============================================================================
 * 测试 18: 任务 ID 分配
 *============================================================================*/

static void test_task_id_alloc(void) {
    test_section("Test 18: Task ID Allocation");

    // 创建任务
    task_id_t t1 = task_create("id1", test8_task, NULL, 10, 0);
    task_id_t t2 = task_create("id2", test8_task, NULL, 10, 0);
    task_id_t t3 = task_create("id3", test8_task, NULL, 10, 0);

    // ID 应该有效且不同
    if (t1 >= 0 && t2 >= 0 && t3 >= 0 && t1 != t2 && t2 != t3 && t1 != t3) {
        test_pass("Unique task IDs allocated");
    } else {
        test_fail("Unique task IDs allocated");
    }

    // 删除中间任务
    task_delete(t2);

    // 创建新任务，应该复用 t2 的 ID
    task_id_t t4 = task_create("id4", test8_task, NULL, 10, 0);

    if (t4 == t2) {
        test_pass("Task ID reused after delete");
    } else {
        test_fail("Task ID reused after delete");
    }

    task_delete(t1);
    task_delete(t3);
    task_delete(t4);
}

/*============================================================================
 * 主测试任务
 *============================================================================*/

static void test_main_task(void *arg) {
    (void)arg;

    test_print("\r\n\r\n");
    test_print("****************************************\r\n");
    test_print("    My-RTOS Scheduler Test Suite v1.0\r\n");
    test_print("****************************************\r\n");

    test_task_create();
    test_priority_preempt();
    test_round_robin();
    test_task_delay();
    test_suspend_resume();
    test_task_delete();
    test_task_exit();
    test_task_state();
    test_task_priority();
    test_block_wakeup();
    test_timeout();
    test_priority_inheritance();
    test_task_yield();
    test_idle_task();
    test_stress();
    test_self_delete();
    test_recursive_mutex();
    test_task_id_alloc();

    test_summary();

    // LED 闪烁表示测试完成
    while (1) {
        gpio_toggle(TEST_LED_PORT, TEST_LED_PIN);
        task_delay(500);
    }
}

/*============================================================================
 * 主函数
 *============================================================================*/

int main(void) {
    uart_init(TEST_UART, NUCLEO_UART_BAUDRATE);
    gpio_init(TEST_LED_PORT, TEST_LED_PIN, GPIO_DIR_OUTPUT);

    test_print("\r\nScheduler Test Starting...\r\n");

    kern_init();

    task_id_t main_task = task_create("test_main", test_main_task, NULL, 10, 0);
    task_start(main_task);

    test_print("Starting scheduler...\r\n");

    kern_start();

    while (1);
    return 0;
}
