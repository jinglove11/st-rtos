/**
 * @file main.c
 * @brief RTOS 完整功能测试
 */

#include "board_config.h"
#include "kernel.h"
#include "task.h"
#include "semaphore.h"
#include "mutex.h"
#include "mqueue.h"
#include "event.h"
#include "mem.h"
#include "mempool.h"
#include "uart.h"
#include "gpio.h"
#include <string.h>

#define TEST_UART       NUCLEO_DEFAULT_UART
#define TEST_LED_PORT   NUCLEO_LED_PORT
#define TEST_LED_PIN    NUCLEO_LED_PIN

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

/*============================================================================
 * 测试 1: 任务创建与切换
 *============================================================================*/

static volatile int task1_count = 0;
static volatile int task2_count = 0;

static void test_task1(void *arg) {
    (void)arg;
    for (int i = 0; i < 5; i++) {
        task1_count++;
        for (volatile int j = 0; j < 50000; j++);
    }
}

static void test_task2(void *arg) {
    (void)arg;
    for (int i = 0; i < 5; i++) {
        task2_count++;
        for (volatile int j = 0; j < 50000; j++);
    }
}

static void test_task_switch(void) {
    test_print("\r\n=== Test 1: Task Switch ===\r\n");

    task1_count = 0;
    task2_count = 0;

    task_id_t t1 = task_create("test1", test_task1, NULL, 2, 0);
    task_id_t t2 = task_create("test2", test_task2, NULL, 2, 0);

    if (t1 != KERN_INVALID_ID && t2 != KERN_INVALID_ID) {
        test_pass("Task create");
    } else {
        test_fail("Task create");
        return;
    }

    task_start(t1);
    task_start(t2);

    task_delay(100);

    if (task1_count > 0 && task2_count > 0) {
        test_pass("Both tasks running");
    } else {
        test_fail("Both tasks running");
    }

    task_delete(t1);
    task_delete(t2);
}

/*============================================================================
 * 测试 2: 优先级调度
 *============================================================================*/

static volatile int high_prio_ran = 0;
static volatile int low_prio_ran = 0;

static void high_prio_task(void *arg) {
    (void)arg;
    high_prio_ran = 1;
}

static void low_prio_task(void *arg) {
    (void)arg;
    low_prio_ran = 1;
}

static void test_priority(void) {
    test_print("\r\n=== Test 2: Priority Scheduling ===\r\n");

    high_prio_ran = 0;
    low_prio_ran = 0;

    task_id_t low = task_create("low", low_prio_task, NULL, 5, 0);
    task_id_t high = task_create("high", high_prio_task, NULL, 1, 0);

    task_start(low);
    task_start(high);

    task_delay(50);

    if (high_prio_ran && low_prio_ran) {
        test_pass("Priority scheduling");
    } else {
        test_fail("Priority scheduling");
    }

    task_delete(low);
    task_delete(high);
}

/*============================================================================
 * 测试 3: 任务延迟
 *============================================================================*/

static volatile uint32_t delay_start_tick = 0;
static volatile uint32_t delay_end_tick = 0;

static void delay_test_task(void *arg) {
    (void)arg;
    delay_start_tick = kern_get_tick();
    task_delay(50);
    delay_end_tick = kern_get_tick();
}

static void test_task_delay(void) {
    test_print("\r\n=== Test 3: Task Delay ===\r\n");

    delay_start_tick = 0;
    delay_end_tick = 0;

    task_id_t t = task_create("delay", delay_test_task, NULL, 2, 0);
    task_start(t);

    task_delay(100);

    uint32_t elapsed = delay_end_tick - delay_start_tick;
    if (elapsed >= 50 && elapsed <= 55) {
        test_pass("Task delay accuracy");
    } else {
        test_fail("Task delay accuracy");
    }
    test_print_num("  Delay elapsed: ", elapsed);

    task_delete(t);
}

/*============================================================================
 * 测试 4: 任务挂起与恢复
 *============================================================================*/

static volatile int suspend_count = 0;
static volatile int suspend_task_running = 0;

static void suspend_test_task(void *arg) {
    (void)arg;
    while (1) {
        suspend_task_running = 1;
        suspend_count++;
        for (volatile int j = 0; j < 10000; j++);
        task_yield();
    }
}

static void test_task_suspend(void) {
    test_print("\r\n=== Test 4: Task Suspend/Resume ===\r\n");

    suspend_count = 0;
    suspend_task_running = 0;

    task_id_t t = task_create("suspend", suspend_test_task, NULL, 14, 0);
    task_start(t);

    task_delay(20);

    int count_before = suspend_count;

    kern_err_t err = task_suspend(t);
    if (err == KERN_OK) {
        test_pass("Task suspend");
    } else {
        test_fail("Task suspend");
    }

    suspend_task_running = 0;
    task_delay(20);

    int count_during = suspend_count;

    if (count_during == count_before && suspend_task_running == 0) {
        test_pass("Task stopped");
    } else {
        test_fail("Task stopped");
    }

    err = task_resume(t);
    if (err == KERN_OK) {
        test_pass("Task resume");
    } else {
        test_fail("Task resume");
    }

    task_delay(20);

    int count_after = suspend_count;

    if (count_after > count_during) {
        test_pass("Task resumed running");
    } else {
        test_fail("Task resumed running");
    }

    task_delete(t);
}

/*============================================================================
 * 测试 5: 信号量
 *============================================================================*/

static sem_id_t g_test_sem;
static volatile int sem_test_count = 0;

static void sem_wait_task(void *arg) {
    (void)arg;
    kern_err_t err = sem_wait(g_test_sem, 100);
    if (err == KERN_OK) {
        sem_test_count++;
    }
}

static void test_semaphore(void) {
    test_print("\r\n=== Test 5: Semaphore ===\r\n");

    g_test_sem = sem_create(2, 5);
    if (g_test_sem != KERN_INVALID_ID) {
        test_pass("Semaphore create");
    } else {
        test_fail("Semaphore create");
        return;
    }

    int32_t count = sem_get_count(g_test_sem);
    if (count == 2) {
        test_pass("Initial count");
    } else {
        test_fail("Initial count");
    }

    kern_err_t err = sem_wait(g_test_sem, 100);
    if (err == KERN_OK) {
        test_pass("Semaphore wait");
    } else {
        test_fail("Semaphore wait");
    }

    count = sem_get_count(g_test_sem);
    if (count == 1) {
        test_pass("Count after wait");
    } else {
        test_fail("Count after wait");
    }

    err = sem_post(g_test_sem);
    if (err == KERN_OK) {
        test_pass("Semaphore post");
    } else {
        test_fail("Semaphore post");
    }

    count = sem_get_count(g_test_sem);
    if (count == 2) {
        test_pass("Count after post");
    } else {
        test_fail("Count after post");
    }

    sem_test_count = 0;
    sem_wait(g_test_sem, 100);
    sem_wait(g_test_sem, 100);

    task_id_t t = task_create("sem_wait", sem_wait_task, NULL, 2, 0);
    task_start(t);

    task_delay(10);

    if (sem_test_count == 0) {
        test_pass("Task blocked on sem");
    } else {
        test_fail("Task blocked on sem");
    }

    sem_post(g_test_sem);

    task_delay(10);

    if (sem_test_count == 1) {
        test_pass("Task wakeup by sem");
    } else {
        test_fail("Task wakeup by sem");
    }

    task_delete(t);

    err = sem_wait(g_test_sem, 10);
    if (err == KERN_ERR_TIMEOUT) {
        test_pass("Semaphore timeout");
    } else {
        test_fail("Semaphore timeout");
    }

    sem_delete(g_test_sem);
}

/*============================================================================
 * 测试 6: 互斥锁
 *============================================================================*/

static mutex_id_t g_test_mutex;
static volatile int shared_resource = 0;

static void mutex_task(void *arg) {
    (void)arg;
    kern_err_t err = mutex_lock(g_test_mutex, 100);
    if (err == KERN_OK) {
        shared_resource++;
        for (volatile int j = 0; j < 10000; j++);
        mutex_unlock(g_test_mutex);
    }
}

static void test_mutex(void) {
    test_print("\r\n=== Test 6: Mutex ===\r\n");

    g_test_mutex = mutex_create();
    if (g_test_mutex != KERN_INVALID_ID) {
        test_pass("Mutex create");
    } else {
        test_fail("Mutex create");
        return;
    }

    kern_err_t err = mutex_lock(g_test_mutex, 100);
    if (err == KERN_OK) {
        test_pass("Mutex lock");
    } else {
        test_fail("Mutex lock");
    }

    err = mutex_lock(g_test_mutex, 100);
    if (err == KERN_OK) {
        test_pass("Recursive lock");
    } else {
        test_fail("Recursive lock");
    }

    mutex_unlock(g_test_mutex);
    mutex_unlock(g_test_mutex);

    shared_resource = 0;

    task_id_t t1 = task_create("mtx1", mutex_task, NULL, 2, 0);
    task_id_t t2 = task_create("mtx2", mutex_task, NULL, 2, 0);
    task_id_t t3 = task_create("mtx3", mutex_task, NULL, 2, 0);

    task_start(t1);
    task_start(t2);
    task_start(t3);

    task_delay(100);

    if (shared_resource == 3) {
        test_pass("Mutex mutual exclusion");
    } else {
        test_fail("Mutex mutual exclusion");
    }

    task_delete(t1);
    task_delete(t2);
    task_delete(t3);

    err = mutex_lock(g_test_mutex, 100);
    mutex_unlock(g_test_mutex);

    err = mutex_lock(g_test_mutex, 10);
    if (err == KERN_ERR_TIMEOUT) {
        test_pass("Mutex timeout");
    } else {
        test_fail("Mutex timeout");
    }

    mutex_delete(g_test_mutex);
}

/*============================================================================
 * 测试 7: 消息队列
 *============================================================================*/

static queue_id_t g_test_queue;

static void queue_recv_task(void *arg) {
    (void)arg;
    uint32_t msg;
    kern_err_t err = mqueue_recv(g_test_queue, &msg, 100);
    if (err == KERN_OK && msg == 0xBEEF5678) {
        test_pass("Queue blocking recv");
    }
}

static void test_mqueue(void) {
    test_print("\r\n=== Test 7: Message Queue ===\r\n");

    g_test_queue = mqueue_create(4, 8);
    if (g_test_queue != KERN_INVALID_ID) {
        test_pass("Queue create");
    } else {
        test_fail("Queue create");
        return;
    }

    uint32_t send_msg = 0xDEADBEEF;
    kern_err_t err = mqueue_trysend(g_test_queue, &send_msg);
    if (err == KERN_OK) {
        test_pass("Queue send");
    } else {
        test_fail("Queue send");
    }

    uint32_t recv_msg = 0;
    err = mqueue_tryrecv(g_test_queue, &recv_msg);
    if (err == KERN_OK && recv_msg == send_msg) {
        test_pass("Queue recv");
    } else {
        test_fail("Queue recv");
    }

    for (int i = 0; i < 8; i++) {
        mqueue_trysend(g_test_queue, &send_msg);
    }
    err = mqueue_trysend(g_test_queue, &send_msg);
    if (err == KERN_ERR_BUSY) {
        test_pass("Queue full");
    } else {
        test_fail("Queue full");
    }

    uint32_t dummy;
    while (mqueue_tryrecv(g_test_queue, &dummy) == KERN_OK);

    int32_t count = mqueue_get_count(g_test_queue);
    if (count == 0) {
        test_pass("Queue empty");
    } else {
        test_fail("Queue empty");
    }

    task_id_t t1 = task_create("qrecv", queue_recv_task, NULL, 2, 0);
    task_start(t1);

    task_delay(10);

    uint32_t msg = 0xBEEF5678;
    mqueue_send(g_test_queue, &msg, 100);

    task_delay(10);
    task_delete(t1);

    mqueue_delete(g_test_queue);
}

/*============================================================================
 * 测试 8: 事件标志
 *============================================================================*/

static event_id_t g_test_event;

static void event_wait_or_task(void *arg) {
    (void)arg;
    uint32_t received;
    kern_err_t err = event_wait(g_test_event, 0x03, EVENT_OPT_OR, 100, &received);
    if (err == KERN_OK && (received & 0x03)) {
        test_pass("Event OR wait");
    }
}

static void event_wait_and_task(void *arg) {
    (void)arg;
    uint32_t received;
    kern_err_t err = event_wait(g_test_event, 0x07, EVENT_OPT_AND, 100, &received);
    if (err == KERN_OK && (received & 0x07) == 0x07) {
        test_pass("Event AND wait");
    }
}

static void test_event_flags(void) {
    test_print("\r\n=== Test 8: Event Flags ===\r\n");

    g_test_event = event_create(0);
    if (g_test_event != KERN_INVALID_ID) {
        test_pass("Event create");
    } else {
        test_fail("Event create");
        return;
    }

    event_set(g_test_event, 0x01);
    uint32_t flags = event_get(g_test_event);
    if (flags == 0x01) {
        test_pass("Event set");
    } else {
        test_fail("Event set");
    }

    event_set(g_test_event, 0x02);
    flags = event_get(g_test_event);
    if (flags == 0x03) {
        test_pass("Event set multiple");
    } else {
        test_fail("Event set multiple");
    }

    event_clear(g_test_event, 0x01);
    flags = event_get(g_test_event);
    if (flags == 0x02) {
        test_pass("Event clear");
    } else {
        test_fail("Event clear");
    }

    event_clear(g_test_event, 0x02);

    task_id_t t1 = task_create("evt_or", event_wait_or_task, NULL, 2, 0);
    task_start(t1);

    task_delay(10);

    event_set(g_test_event, 0x01);

    task_delay(10);
    task_delete(t1);

    event_clear(g_test_event, 0xFF);

    task_id_t t2 = task_create("evt_and", event_wait_and_task, NULL, 2, 0);
    task_start(t2);

    task_delay(10);

    event_set(g_test_event, 0x01);
    event_set(g_test_event, 0x02);
    event_set(g_test_event, 0x04);

    task_delay(10);
    task_delete(t2);

    event_delete(g_test_event);
}

/*============================================================================
 * 测试 9: 时间片轮转
 *============================================================================*/

static volatile int rr_count1 = 0;
static volatile int rr_count2 = 0;

static void rr_task1(void *arg) {
    (void)arg;
    for (int i = 0; i < 10; i++) {
        rr_count1++;
        for (volatile int j = 0; j < 100000; j++);
    }
}

static void rr_task2(void *arg) {
    (void)arg;
    for (int i = 0; i < 10; i++) {
        rr_count2++;
        for (volatile int j = 0; j < 100000; j++);
    }
}

static void test_round_robin(void) {
    test_print("\r\n=== Test 9: Round Robin ===\r\n");

    rr_count1 = 0;
    rr_count2 = 0;

    task_id_t t1 = task_create("rr1", rr_task1, NULL, 3, 0);
    task_id_t t2 = task_create("rr2", rr_task2, NULL, 3, 0);

    task_start(t1);
    task_start(t2);

    task_delay(200);

    if (rr_count1 > 0 && rr_count2 > 0) {
        test_pass("Round robin");
    } else {
        test_fail("Round robin");
    }

    test_print_num("  Task1 count: ", rr_count1);
    test_print_num("  Task2 count: ", rr_count2);

    task_delete(t1);
    task_delete(t2);
}

/*============================================================================
 * 测试 10: 内存管理
 *============================================================================*/

static void test_memory(void) {
    test_print("\r\n=== Test 10: Memory Management ===\r\n");

    mem_stats_t stats = mem_get_stats();
    test_print_num("  Total heap: ", stats.total_size);
    test_print_num("  Free heap: ", stats.free_size);

    void *p1 = kmalloc(64);
    void *p2 = kmalloc(128);
    void *p3 = kmalloc(256);

    if (p1 && p2 && p3) {
        test_pass("kmalloc");
    } else {
        test_fail("kmalloc");
    }

    memset(p1, 0xAA, 64);
    memset(p2, 0xBB, 128);
    memset(p3, 0xCC, 256);

    kfree(p2);

    void *p4 = kmalloc(64);
    if (p4) {
        test_pass("kmalloc after free");
    } else {
        test_fail("kmalloc after free");
    }

    kfree(p1);
    kfree(p3);
    kfree(p4);

    stats = mem_get_stats();
    if (stats.used_size == 0) {
        test_pass("kfree all");
    } else {
        test_fail("kfree all");
    }

    void *aligned = kmalloc_aligned(64, 32);
    if (aligned && ((uintptr_t)aligned & 31) == 0) {
        test_pass("kmalloc_aligned");
    } else {
        test_fail("kmalloc_aligned");
    }
    kfree_aligned(aligned);

    pool_id_t pool = mempool_create(32, 8);
    if (pool != POOL_INVALID_ID) {
        test_pass("mempool_create");
    } else {
        test_fail("mempool_create");
        return;
    }

    void *blocks[8];
    for (int i = 0; i < 8; i++) {
        blocks[i] = mempool_alloc(pool);
    }

    if (mempool_get_free_count(pool) == 0) {
        test_pass("mempool full");
    } else {
        test_fail("mempool full");
    }

    void *fail_block = mempool_alloc(pool);
    if (fail_block == NULL) {
        test_pass("mempool exhaust");
    } else {
        test_fail("mempool exhaust");
    }

    for (int i = 0; i < 8; i++) {
        mempool_free(pool, blocks[i]);
    }

    if (mempool_get_free_count(pool) == 8) {
        test_pass("mempool free all");
    } else {
        test_fail("mempool free all");
    }

    mempool_delete(pool);
}

/*============================================================================
 * 测试总结
 *============================================================================*/

static void print_summary(void) {
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
 * 主测试任务
 *============================================================================*/

static void test_main_task(void *arg) {
    (void)arg;

    test_print("\r\n\r\n");
    test_print("========================================\r\n");
    test_print("    My-RTOS v0.1 Test Suite\r\n");
    test_print("========================================\r\n");

    test_task_switch();
    test_priority();
    test_task_delay();
    test_task_suspend();
    test_semaphore();
    test_mutex();
    test_mqueue();
    test_event_flags();
    test_round_robin();
    test_memory();

    print_summary();

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

    test_print("\r\nMy-RTOS Test Suite Starting...\r\n");

    kern_init();

    task_id_t main_task = task_create("test_main", test_main_task, NULL, 15, 0);
    task_start(main_task);

    uart_puts(TEST_UART, "Starting test scheduler...\r\n");

    kern_start();

    while (1);
    return 0;
}
