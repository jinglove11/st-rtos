/**
 * @file main.c
 * @brief 调度器功能测试
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

static void test_summary(void) {
    test_print("\r\n========================================\r\n");
    test_print("         SCHEDULER TEST SUMMARY\r\n");
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
    test_print("\r\n=== Test 1: Task Create & Schedule ===\r\n");

    test1_task1_ran = 0;
    test1_task2_ran = 0;

    task_id_t t1 = task_create("test1", test1_task1, NULL, 5, 0);
    task_id_t t2 = task_create("test2", test1_task2, NULL, 5, 0);

    if (t1 != KERN_INVALID_ID && t2 != KERN_INVALID_ID) {
        test_pass("Task create");
    } else {
        test_fail("Task create");
        return;
    }

    task_start(t1);
    task_start(t2);

    task_delay(10);

    if (test1_task1_ran && test1_task2_ran) {
        test_pass("Both tasks executed");
    } else {
        test_fail("Both tasks executed");
    }
}

/*============================================================================
 * 测试 2: 优先级抢占
 *============================================================================*/

static volatile int test2_high_ran = 0;
static volatile int test2_low_started = 0;
static volatile int test2_low_finished = 0;

static void test2_high_task(void *arg) {
    (void)arg;
    test2_high_ran = 1;
}

static void test2_low_task(void *arg) {
    (void)arg;
    test2_low_started = 1;

    // 高优先级任务应该在这里抢占
    for (volatile int i = 0; i < 100000; i++);

    test2_low_finished = 1;
}

static void test_priority_preempt(void) {
    test_print("\r\n=== Test 2: Priority Preemption ===\r\n");

    test2_high_ran = 0;
    test2_low_started = 0;
    test2_low_finished = 0;

    // 创建低优先级任务 (优先级 10)
    task_id_t low = task_create("low", test2_low_task, NULL, 10, 0);
    // 创建高优先级任务 (优先级 2)
    task_id_t high = task_create("high", test2_high_task, NULL, 2, 0);

    task_start(low);
    task_delay(5);  // 让低优先级任务开始运行
    task_start(high);  // 启动高优先级任务，应该抢占

    task_delay(20);

    if (test2_high_ran) {
        test_pass("High priority task ran");
    } else {
        test_fail("High priority task ran");
    }

    if (test2_low_started && test2_low_finished) {
        test_pass("Low priority task completed");
    } else {
        test_fail("Low priority task completed");
    }
}

/*============================================================================
 * 测试 3: 时间片轮转
 *============================================================================*/

static volatile int test3_count1 = 0;
static volatile int test3_count2 = 0;

static void test3_task1(void *arg) {
    (void)arg;
    for (int i = 0; i < 5; i++) {
        test3_count1++;
        for (volatile int j = 0; j < 50000; j++);
    }
}

static void test3_task2(void *arg) {
    (void)arg;
    for (int i = 0; i < 5; i++) {
        test3_count2++;
        for (volatile int j = 0; j < 50000; j++);
    }
}

static void test_round_robin(void) {
    test_print("\r\n=== Test 3: Round Robin ===\r\n");

    test3_count1 = 0;
    test3_count2 = 0;

    // 同优先级任务应该轮流执行
    task_id_t t1 = task_create("rr1", test3_task1, NULL, 5, 0);
    task_id_t t2 = task_create("rr2", test3_task2, NULL, 5, 0);

    task_start(t1);
    task_start(t2);

    task_delay(100);

    if (test3_count1 > 0 && test3_count2 > 0) {
        test_pass("Both tasks ran (round robin)");
    } else {
        test_fail("Both tasks ran (round robin)");
    }

    test_print_num("  Task1 count: ", test3_count1);
    test_print_num("  Task2 count: ", test3_count2);
}

/*============================================================================
 * 测试 4: 任务延时
 *============================================================================*/

static void test_task_delay(void) {
    test_print("\r\n=== Test 4: Task Delay ===\r\n");

    uint32_t start = kern_get_tick();
    task_delay(50);
    uint32_t end = kern_get_tick();

    uint32_t elapsed = end - start;

    if (elapsed >= 50 && elapsed <= 52) {
        test_pass("Delay accuracy");
    } else {
        test_fail("Delay accuracy");
    }

    test_print_num("  Expected: 50 ticks, Actual: ", elapsed);
}

/*============================================================================
 * 测试 5: 任务挂起与恢复
 *============================================================================*/

static volatile int test5_count = 0;

static void test5_task(void *arg) {
    (void)arg;
    while (1) {
        test5_count++;
        for (volatile int j = 0; j < 50000; j++);
        task_yield();
    }
}

static void test_suspend_resume(void) {
    test_print("\r\n=== Test 5: Suspend & Resume ===\r\n");

    test5_count = 0;

    task_id_t t = task_create("suspend", test5_task, NULL, 10, 0);
    task_start(t);

    task_delay(20);
    int count1 = test5_count;

    task_suspend(t);
    task_delay(20);
    int count2 = test5_count;

    task_resume(t);
    task_delay(20);
    int count3 = test5_count;

    if (count1 > 0 && count2 == count1 && count3 > count2) {
        test_pass("Suspend/Resume works");
    } else {
        test_fail("Suspend/Resume works");
    }

    test_print_num("  Before suspend: ", count1);
    test_print_num("  During suspend: ", count2);
    test_print_num("  After resume: ", count3);

    task_delete(t);
}

/*============================================================================
 * 测试 6: 阻塞与唤醒 (信号量)
 *============================================================================*/

static sem_id_t test6_sem;
static volatile int test6_producer_done = 0;
static volatile int test6_consumer_got = 0;

static void test6_producer(void *arg) {
    (void)arg;
    task_delay(20);
    sem_post(test6_sem);
    test6_producer_done = 1;
}

static void test6_consumer(void *arg) {
    (void)arg;
    kern_err_t err = sem_wait(test6_sem, 100);
    if (err == KERN_OK) {
        test6_consumer_got = 1;
    }
}

static void test_block_wakeup(void) {
    test_print("\r\n=== Test 6: Block & Wakeup (Semaphore) ===\r\n");

    test6_producer_done = 0;
    test6_consumer_got = 0;

    test6_sem = sem_create(0, 1);

    task_id_t consumer = task_create("consumer", test6_consumer, NULL, 5, 0);
    task_id_t producer = task_create("producer", test6_producer, NULL, 5, 0);

    task_start(consumer);
    task_start(producer);

    task_delay(50);

    if (test6_producer_done && test6_consumer_got) {
        test_pass("Block/Wakeup via semaphore");
    } else {
        test_fail("Block/Wakeup via semaphore");
    }

    sem_delete(test6_sem);
}

/*============================================================================
 * 测试 7: 超时唤醒
 *============================================================================*/

static void test_timeout(void) {
    test_print("\r\n=== Test 7: Timeout Wakeup ===\r\n");

    sem_id_t sem = sem_create(0, 1);

    uint32_t start = kern_get_tick();
    kern_err_t err = sem_wait(sem, 30);  // 等待30 ticks，应该超时
    uint32_t end = kern_get_tick();
    uint32_t elapsed = end - start;

    if (err == KERN_ERR_TIMEOUT) {
        test_pass("Timeout returned");
    } else {
        test_fail("Timeout returned");
    }

    if (elapsed >= 30 && elapsed <= 32) {
        test_pass("Timeout accuracy");
    } else {
        test_fail("Timeout accuracy");
    }

    test_print_num("  Expected: 30 ticks, Actual: ", elapsed);

    sem_delete(sem);
}

/*============================================================================
 * 测试 8: 优先级继承
 *============================================================================*/

static mutex_id_t test8_mutex;
static volatile int test8_shared = 0;
static volatile int test8_high_got_lock = 0;

static void test8_low_task(void *arg) {
    (void)arg;
    mutex_lock(test8_mutex, 100);

    test8_shared = 1;

    // 持有锁一段时间
    for (volatile int i = 0; i < 200000; i++);

    mutex_unlock(test8_mutex);
}

static void test8_high_task(void *arg) {
    (void)arg;
    task_delay(5);  // 等待低优先级任务先获取锁

    kern_err_t err = mutex_lock(test8_mutex, 100);
    if (err == KERN_OK) {
        test8_high_got_lock = 1;
        mutex_unlock(test8_mutex);
    }
}

static void test_priority_inheritance(void) {
    test_print("\r\n=== Test 8: Priority Inheritance ===\r\n");

    test8_shared = 0;
    test8_high_got_lock = 0;

    test8_mutex = mutex_create();

    // 低优先级任务先获取锁
    task_id_t low = task_create("low", test8_low_task, NULL, 10, 0);
    // 高优先级任务等待锁
    task_id_t high = task_create("high", test8_high_task, NULL, 2, 0);

    task_start(low);
    task_start(high);

    task_delay(100);

    if (test8_high_got_lock) {
        test_pass("Priority inheritance works");
    } else {
        test_fail("Priority inheritance works");
    }

    mutex_delete(test8_mutex);
}

/*============================================================================
 * 主测试任务
 *============================================================================*/

static void test_main_task(void *arg) {
    (void)arg;

    test_print("\r\n\r\n");
    test_print("========================================\r\n");
    test_print("    My-RTOS Scheduler Test Suite\r\n");
    test_print("========================================\r\n");

    test_task_create();
    test_priority_preempt();
    test_round_robin();
    test_task_delay();
    test_suspend_resume();
    test_block_wakeup();
    test_timeout();
    test_priority_inheritance();

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
