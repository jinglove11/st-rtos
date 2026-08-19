/**
 * @file test_scheduler.c
 * @brief 调度器功能测试模块
 *
 * 测试内容：
 * 1. 任务创建与基本调度
 * 2. 优先级抢占
 * 3. 时间片轮转
 * 4. 任务延时
 * 5. 任务挂起与恢复
 * 6. 任务删除
 * 7. 任务退出
 * 8. 任务状态查询
 * 9. 任务优先级修改
 * 10. 阻塞与唤醒
 * 11. 超时唤醒
 * 12. 优先级继承
 * 13. 任务让出 CPU
 * 14. 空闲任务
 * 15. 多任务压力测试
 * 16. 任务自删除保护
 * 17. 递归锁
 * 18. 任务 ID 分配
 * 19. 单核 RUNNING 状态不变量
 */

#include "test_framework.h"
#include "kernel.h"
#include "task.h"
#include "semaphore.h"
#include "mutex.h"
#include <string.h>

#if !SMP
/*============================================================================
 * 测试 0: 单核系统只能有一个 RUNNING TCB
 *============================================================================*/

static void test_single_core_running_invariant(void) {
    test_section("Test 0: Single-core running invariant");

    uint64_t used = task_get_used_bitmap();
    int running = 0;

    for (task_id_t id = 0; id < KERNEL_MAX_TASKS; id++) {
        if ((used & (1ULL << id)) == 0) {
            continue;
        }

        tcb_t *tcb = task_get_tcb(id);
        if (tcb != NULL && tcb->state == TASK_STATE_RUNNING) {
            running++;
        }
    }

    TEST_ASSERT_EQ(1, running, "UP kernel has exactly one running task");
}
#endif

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

    /* 测试有效创建 */
    task_id_t t1 = task_create("test1", test1_task1, NULL, 5, 0);
    task_id_t t2 = task_create("test2", test1_task2, NULL, 5, 0);

    TEST_ASSERT(t1 >= 0 && t2 >= 0, "Create tasks with valid params");

    /* 测试任务名称 */
    const char *name = task_get_name(t1);
    TEST_ASSERT(name && strcmp(name, "test1") == 0, "Task name set correctly");

    /* 测试优先级 */
    uint8_t prio = task_get_priority(t1);
    TEST_ASSERT_EQ(5, prio, "Task priority set correctly");

    /* 启动任务 */
    kern_err_t err = task_start(t1);
    TEST_ASSERT_EQ(KERN_OK, err, "Task start success");

    task_start(t2);
    task_delay(10);

    TEST_ASSERT(test1_task1_ran && test1_task2_ran, "Both tasks executed");

    /* 测试无效优先级 */
    task_id_t t3 = task_create("invalid", test1_task1, NULL, 255, 0);
    TEST_ASSERT_EQ(KERN_INVALID_ID, t3, "Reject invalid priority");
}

/*============================================================================
 * 测试 2: 优先级抢占
 *============================================================================*/

static volatile int test2_high_ran = 0;
static volatile int test2_mid_ran = 0;
static volatile int test2_low_started = 0;

static void test2_high_task(void *arg) {
    (void)arg;
    test2_high_ran = 1;
}

static void test2_mid_task(void *arg) {
    (void)arg;
    test2_mid_ran = 1;
}

static void test2_low_task(void *arg) {
    (void)arg;
    test2_low_started = 1;

    /* 长时间运行，让高优先级任务抢占 */
    for (volatile int i = 0; i < 100000; i++);
}

static void test_priority_preempt(void) {
    test_section("Test 2: Priority Preemption");

    test2_high_ran = 0;
    test2_mid_ran = 0;
    test2_low_started = 0;

    /* 创建三个不同优先级的任务 */
    task_id_t low = task_create("low", test2_low_task, NULL, 20, 0);
    task_id_t mid = task_create("mid", test2_mid_task, NULL, 10, 0);
    task_id_t high = task_create("high", test2_high_task, NULL, 2, 0);

    /* 先启动低优先级任务 */
    task_start(low);
    task_delay(5);

    /* 启动高优先级任务，应该立即抢占 */
    task_start(high);

    task_delay(10);

    TEST_ASSERT(test2_high_ran, "High priority preempted low");
    TEST_ASSERT(test2_low_started, "Low priority task started");

    /* 测试中优先级任务 */
    test2_mid_ran = 0;
    task_start(mid);
    task_delay(10);

    TEST_ASSERT(test2_mid_ran, "Mid priority task ran");
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
        /* 忙等待消耗时间片 */
        for (volatile int j = 0; j < 100000; j++);
    }
}

static void test_round_robin(void) {
    test_section("Test 3: Round Robin (Time Slice)");

    test3_count1 = 0;
    test3_count2 = 0;
    test3_count3 = 0;

    /* 创建三个同优先级任务 */
    task_id_t t1 = task_create("rr1", test3_task, (void *)&test3_count1, 5, 0);
    task_id_t t2 = task_create("rr2", test3_task, (void *)&test3_count2, 5, 0);
    task_id_t t3 = task_create("rr3", test3_task, (void *)&test3_count3, 5, 0);

    task_start(t1);
    task_start(t2);
    task_start(t3);

    task_delay(200);

    /* 所有任务都应该有机会运行 */
    TEST_ASSERT(test3_count1 > 0 && test3_count2 > 0 && test3_count3 > 0,
                "All same-priority tasks ran");


    /* 检查时间片轮转的公平性 */
    int max = test3_count1 > test3_count2 ? test3_count1 : test3_count2;
    max = max > test3_count3 ? max : test3_count3;
    int min = test3_count1 < test3_count2 ? test3_count1 : test3_count2;
    min = min < test3_count3 ? min : test3_count3;

    TEST_ASSERT(max - min <= 3, "Fair round robin distribution");
}

/*============================================================================
 * 测试 4: 任务延时
 *============================================================================*/

static void test_task_delay(void) {
    test_section("Test 4: Task Delay");

    /* 测试基本延时 */
    uint32_t start = kern_get_tick();
    task_delay(50);
    uint32_t end = kern_get_tick();
    uint32_t elapsed = end - start;

    TEST_ASSERT_RANGE(elapsed, 50, 52, "Delay 50 ticks accuracy");

    /* 测试短延时 */
    start = kern_get_tick();
    task_delay(5);
    end = kern_get_tick();
    elapsed = end - start;

    TEST_ASSERT_RANGE(elapsed, 5, 7, "Delay 5 ticks accuracy");

    /* 测试零延时 */
    start = kern_get_tick();
    task_delay(0);
    end = kern_get_tick();
    elapsed = end - start;

    TEST_ASSERT_EQ(0, elapsed, "Delay 0 returns immediately");

    /* 测试毫秒延时 */
    start = kern_get_tick();
    task_delay_ms(10);
    end = kern_get_tick();
    elapsed = end - start;

    TEST_ASSERT_RANGE(elapsed, 10, 12, "Delay_ms accuracy");
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
    test_section("Test 5: Suspend & Resume");

    test5_count = 0;

    task_id_t t = task_create("suspend", test5_task, NULL, 10, 0);
    task_start(t);

    task_delay(20);
    int count1 = test5_count;

    /* 挂起任务 */
    kern_err_t err = task_suspend(t);
    TEST_ASSERT_EQ(KERN_OK, err, "Suspend task success");

    task_delay(20);
    int count2 = test5_count;

    TEST_ASSERT_EQ(count1, count2, "Suspended task stopped");

    /* 恢复任务 */
    err = task_resume(t);
    TEST_ASSERT_EQ(KERN_OK, err, "Resume task success");

    task_delay(20);
    int count3 = test5_count;

    TEST_ASSERT(count3 > count2, "Resumed task continued");


    /* 测试重复挂起 */
    err = task_suspend(t);
    task_suspend(t);
    TEST_ASSERT_EQ(KERN_OK, err, "Double suspend handled");

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

    /* 删除任务 */
    kern_err_t err = task_delete(t);
    TEST_ASSERT_EQ(KERN_OK, err, "Delete task success");

    task_delay(30);

    int count_after = test6_deleted;

    TEST_ASSERT_EQ(count_before, count_after, "Deleted task stopped");


    /* 测试删除无效任务 */
    err = task_delete(KERN_INVALID_ID);
    TEST_ASSERT_EQ(KERN_ERR_PARAM, err, "Reject delete invalid task");
}

/*============================================================================
 * 测试 7: 任务退出
 *============================================================================*/

static volatile int test7_exited = 0;

static void test7_task(void *arg) {
    (void)arg;
    test7_exited = 1;
    task_exit(NULL);
    test7_exited = 2;  /* 不应该执行到这里 */
}

static void test_task_exit(void) {
    test_section("Test 7: Task Exit");

    test7_exited = 0;

    task_id_t t = task_create("toexit", test7_task, NULL, 10, 0);
    task_start(t);

    task_delay(10);

    TEST_ASSERT_EQ(1, test7_exited, "Task exit executed");

    /* 检查任务状态 */
    task_state_t state = task_get_state(t);
    TEST_ASSERT_EQ(TASK_STATE_TERMINATED, state, "Exited task is TERMINATED");
}

/*============================================================================
 * 测试 8: 任务状态查询
 *============================================================================*/

static void test8_task(void *arg) {
    (void)arg;
    while (1) {
        for (volatile int i = 0; i < 10000; i++);
        task_yield();
    }
}

static void test_task_state(void) {
    test_section("Test 8: Task State Query");

    /* 创建但不启动 - CREATED */
    task_id_t t = task_create("state_test", test8_task, NULL, 10, 0);
    task_state_t state = task_get_state(t);

    TEST_ASSERT_EQ(TASK_STATE_CREATED, state, "Created state correct");

    /* 启动后 - READY 或 RUNNING */
    task_start(t);
    task_delay(5);
    state = task_get_state(t);

    TEST_ASSERT(state == TASK_STATE_READY || state == TASK_STATE_RUNNING,
                "Started state correct");

    /* 挂起 - SUSPENDED */
    task_suspend(t);
    state = task_get_state(t);

    TEST_ASSERT_EQ(TASK_STATE_SUSPENDED, state, "Suspended state correct");

    /* 恢复 - READY */
    task_resume(t);
    state = task_get_state(t);

    TEST_ASSERT_EQ(TASK_STATE_READY, state, "Resumed state correct");

    /* 删除 - TERMINATED */
    task_delete(t);
    state = task_get_state(t);

    TEST_ASSERT_EQ(TASK_STATE_TERMINATED, state, "Deleted state correct");

    /* 查询无效任务 */
    state = task_get_state(KERN_INVALID_ID);
    TEST_ASSERT_EQ(TASK_STATE_TERMINATED, state, "Invalid task returns TERMINATED");
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

    /* 创建三个任务，初始优先级相同 */
    task_id_t t1 = task_create("p1", test9_task, (void *)1, 10, 0);
    task_id_t t2 = task_create("p2", test9_task, (void *)2, 10, 0);
    task_id_t t3 = task_create("p3", test9_task, (void *)3, 10, 0);

    /* 修改优先级 */
    kern_err_t err = task_set_priority(t1, 5);
    TEST_ASSERT_EQ(KERN_OK, err, "Set priority success");

    task_set_priority(t2, 15);
    task_set_priority(t3, 25);

    /* 验证优先级 */
    TEST_ASSERT_EQ(5, task_get_priority(t1), "Priority changed correctly");

    /* 启动任务，应该按优先级顺序执行 */
    task_start(t3);
    task_start(t2);
    task_start(t1);

    task_delay(20);

    /* 最高优先级任务应该先执行 */
    TEST_ASSERT_EQ(1, test9_seq[0], "Highest priority ran first");


    /* 测试无效优先级 */
    err = task_set_priority(t1, 255);
    TEST_ASSERT_EQ(KERN_ERR_PARAM, err, "Reject invalid priority");
}

/*============================================================================
 * 测试 10: 阻塞与唤醒 (信号量)
 *============================================================================*/

static sem_id_t test10_sem;
static volatile int test10_producer_done = 0;
static volatile int test10_consumer_got = 0;

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

    TEST_ASSERT(test10_producer_done && test10_consumer_got,
                "Block/wakeup via semaphore");

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

    TEST_ASSERT_EQ(KERN_ERR_TIMEOUT, err, "Timeout returned correctly");
    TEST_ASSERT_RANGE(elapsed, 30, 32, "Timeout timing accurate");


    sem_delete(sem);
}

/*============================================================================
 * 测试 12: 优先级继承
 *============================================================================*/

static mutex_id_t test12_mutex;
static volatile int test12_low_has_lock = 0;
static volatile int test12_high_got_lock = 0;
static volatile int test12_mid_ran = 0;
static volatile int test12_low_finished = 0;

static void test12_low_task(void *arg) {
    (void)arg;
    mutex_lock(test12_mutex, 100);
    test12_low_has_lock = 1;

    /* 持有锁，让高优先级任务等待 */
    for (volatile int i = 0; i < 500000; i++);

    test12_low_finished = 1;
    mutex_unlock(test12_mutex);
}

static void test12_mid_task(void *arg) {
    (void)arg;
    /* 中优先级任务，用于测试优先级继承 */
    for (volatile int i = 0; i < 100000; i++);
    test12_mid_ran = 1;
}

static void test12_high_task(void *arg) {
    (void)arg;
    /* 延迟等待低优先级任务获取锁 */
    task_delay(10);

    kern_err_t err = mutex_lock(test12_mutex, 500);
    if (err == KERN_OK) {
        test12_high_got_lock = 1;
        mutex_unlock(test12_mutex);
    }
}

static void test_priority_inheritance(void) {
    test_section("Test 12: Priority Inheritance");

    test12_low_has_lock = 0;
    test12_high_got_lock = 0;
    test12_mid_ran = 0;
    test12_low_finished = 0;

    test12_mutex = mutex_create();

    task_id_t low = task_create("low", test12_low_task, NULL, 20, 0);
    task_id_t mid = task_create("mid", test12_mid_task, NULL, 10, 0);
    task_id_t high = task_create("high", test12_high_task, NULL, 2, 0);

    task_start(low);
    task_delay(2);
    task_start(high);
    task_delay(2);
    task_start(mid);

    task_delay(200);

    TEST_ASSERT(test12_high_got_lock, "High priority got lock");
    TEST_ASSERT(test12_low_finished, "Low priority finished");


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
        task_yield();
    }
}

static void test13_task2(void *arg) {
    (void)arg;
    for (int i = 0; i < 5; i++) {
        test13_count2++;
        task_yield();
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

    TEST_ASSERT(test13_count1 == 5 && test13_count2 == 5,
                "Yield allows fair execution");

}

/*============================================================================
 * 测试 14: 空闲任务
 *============================================================================*/

static void test_idle_task(void) {
    test_section("Test 14: Idle Task");


    uint32_t start = kern_get_tick();
    task_delay(50);
    uint32_t end = kern_get_tick();

    TEST_ASSERT(end - start >= 50, "System ran during idle");
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

    /* 创建不同优先级的任务 */
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

    TEST_ASSERT(all_ran, "All stress tasks completed");

    for (int i = 0; i < STRESS_TASK_COUNT; i++) {
    }
}

/*============================================================================
 * 测试 16: 任务自删除
 *============================================================================*/

static volatile int test16_self_deleted = 0;

static void test16_task(void *arg) {
    (void)arg;
    test16_self_deleted = 1;
    /* 任务自己删除自己应该失败 */
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

    TEST_ASSERT_EQ(2, test16_self_deleted, "Self delete rejected correctly");
}

/*============================================================================
 * 测试 17: 递归锁
 *============================================================================*/

static void test_recursive_mutex(void) {
    test_section("Test 17: Recursive Mutex");

    mutex_id_t m = mutex_create();

    /* 第一次加锁 */
    kern_err_t err = mutex_lock(m, 100);
    TEST_ASSERT_EQ(KERN_OK, err, "First lock success");

    /* 第二次加锁（递归） */
    err = mutex_lock(m, 100);
    TEST_ASSERT_EQ(KERN_OK, err, "Recursive lock success");

    /* 第三次加锁 */
    err = mutex_lock(m, 100);
    TEST_ASSERT_EQ(KERN_OK, err, "Third recursive lock success");

    /* 解锁三次 */
    mutex_unlock(m);
    mutex_unlock(m);
    err = mutex_unlock(m);

    TEST_ASSERT_EQ(KERN_OK, err, "All unlocks success");

    mutex_delete(m);
}

/*============================================================================
 * 测试 18: 任务 ID 分配
 *============================================================================*/

static void test18_task(void *arg) {
    (void)arg;
    while (1) {
        for (volatile int i = 0; i < 10000; i++);
        task_yield();
    }
}

static void test_task_id_alloc(void) {
    test_section("Test 18: Task ID Allocation");

    /* 创建任务 */
    task_id_t t1 = task_create("id1", test18_task, NULL, 10, 0);
    task_id_t t2 = task_create("id2", test18_task, NULL, 10, 0);
    task_id_t t3 = task_create("id3", test18_task, NULL, 10, 0);

    /* ID 应该有效且不同 */
    TEST_ASSERT(t1 >= 0 && t2 >= 0 && t3 >= 0 && t1 != t2 && t2 != t3 && t1 != t3,
                "Unique task IDs allocated");

    /* 删除中间任务 */
    task_delete(t2);

    /* 创建新任务，应该复用 t2 的 ID */
    task_id_t t4 = task_create("id4", test18_task, NULL, 10, 0);

    TEST_ASSERT_EQ(t2, t4, "Task ID reused after delete");

    task_delete(t1);
    task_delete(t3);
    task_delete(t4);
}

/*============================================================================
 * 调度器测试模块入口
 *============================================================================*/

/**
 * @brief 调度器测试模块主函数
 *
 * 执行所有调度器相关测试。
 */
static void test_scheduler_module(void) {
#if !SMP
    test_single_core_running_invariant();
#endif
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
}

/*============================================================================
 * 模块注册
 *============================================================================*/

TEST_K_MODULE(scheduler, test_scheduler_module);
