/**
 * @file test_deadlock.c
 * @brief 死锁检测测试模块
 *
 * 测试内容：
 * 1. 两任务环死锁 (AB-BA)
 * 2. 三任务链死锁
 * 3. 单等待者无假阳性
 * 4. 超时打破死锁
 * 5. 递归锁不自死锁
 * 6. mutex_deadlock_check() 诊断 API
 */

#include "test_framework.h"
#include "mutex.h"
#include "task.h"
#include "kernel_types.h"

/*============================================================================
 * 测试 1: 两任务环死锁 (AB-BA)
 *
 * A 持有 m1 等待 m2, B 持有 m2 等待 m1 -> 死锁检测应阻止 A
 *============================================================================*/

static mutex_id_t test1_m1, test1_m2;
static volatile int test1_barrier = 0;
static volatile int test1_a_deadlock = 0;
static volatile int test1_b_done = 0;

static void test1_task_a(void *arg) {
    (void)arg;
    mutex_lock(test1_m1, 200);
    test1_barrier = 1;
    task_delay(20);

    kern_err_t err = mutex_lock(test1_m2, 200);
    if (err == KERN_ERR_DEADLOCK) {
        test1_a_deadlock = 1;
    }
    mutex_unlock(test1_m1);
}

static void test1_task_b(void *arg) {
    (void)arg;
    while (!test1_barrier) {
        task_delay(1);
    }
    mutex_lock(test1_m2, 200);
    kern_err_t err = mutex_lock(test1_m1, 300);
    if (err == KERN_OK) {
        mutex_unlock(test1_m1);
    }
    mutex_unlock(test1_m2);
    test1_b_done = 1;
}

static void test_two_task_deadlock(void) {
    test_section("Test 1: Two-Task Circular Deadlock (AB-BA)");

    test1_barrier    = 0;
    test1_a_deadlock = 0;
    test1_b_done     = 0;

    test1_m1 = mutex_create();
    test1_m2 = mutex_create();

    task_id_t ta = task_create("dl_a", test1_task_a, NULL, 10, 0);
    task_id_t tb = task_create("dl_b", test1_task_b, NULL, 5, 0);

    task_start(ta);
    task_start(tb);

    task_delay(150);

    TEST_ASSERT(test1_a_deadlock, "Task A detected AB-BA deadlock");
    TEST_ASSERT(test1_b_done, "Task B completed after cycle broken");


    mutex_delete(test1_m1);
    mutex_delete(test1_m2);
}

/*============================================================================
 * 测试 2: 三任务链死锁
 *
 * A(持m1,等m2) -> B(持m2,等m3) -> C(持m3,等m1) -> A
 *============================================================================*/

static mutex_id_t test2_m1, test2_m2, test2_m3;
static volatile int test2_barrier = 0;
static volatile int test2_deadlock = 0;
static volatile int test2_done_flag = 0;

static void test2_task_a(void *arg) {
    (void)arg;
    mutex_lock(test2_m1, 200);
    test2_barrier++;
    while (test2_barrier < 3) {
        task_delay(1);
    }

    kern_err_t err = mutex_lock(test2_m2, 200);
    if (err == KERN_ERR_DEADLOCK) {
        test2_deadlock = 1;
    } else if (err == KERN_OK) {
        mutex_unlock(test2_m2);
    }
    mutex_unlock(test2_m1);
    test2_done_flag++;
}

static void test2_task_b(void *arg) {
    (void)arg;
    mutex_lock(test2_m2, 200);
    test2_barrier++;
    while (test2_barrier < 3) {
        task_delay(1);
    }

    kern_err_t err = mutex_lock(test2_m3, 300);
    if (err == KERN_ERR_DEADLOCK) {
        test2_deadlock = 1;
    } else if (err == KERN_OK) {
        mutex_unlock(test2_m3);
    }
    mutex_unlock(test2_m2);
    test2_done_flag++;
}

static void test2_task_c(void *arg) {
    (void)arg;
    mutex_lock(test2_m3, 200);
    test2_barrier++;
    while (test2_barrier < 3) {
        task_delay(1);
    }

    kern_err_t err = mutex_lock(test2_m1, 300);
    if (err == KERN_ERR_DEADLOCK) {
        test2_deadlock = 1;
    } else if (err == KERN_OK) {
        mutex_unlock(test2_m1);
    }
    mutex_unlock(test2_m3);
    test2_done_flag++;
}

static void test_three_task_deadlock(void) {
    test_section("Test 2: Three-Task Chain Deadlock");

    test2_barrier   = 0;
    test2_deadlock  = 0;
    test2_done_flag = 0;

    test2_m1 = mutex_create();
    test2_m2 = mutex_create();
    test2_m3 = mutex_create();

    task_id_t ta = task_create("d3_a", test2_task_a, NULL, 10, 0);
    task_id_t tb = task_create("d3_b", test2_task_b, NULL, 10, 0);
    task_id_t tc = task_create("d3_c", test2_task_c, NULL, 10, 0);

    task_start(ta);
    task_start(tb);
    task_start(tc);

    task_delay(200);

    TEST_ASSERT(test2_deadlock, "Three-task chain deadlock detected");
    TEST_ASSERT_EQ(3, test2_done_flag, "All three tasks completed");


    mutex_delete(test2_m1);
    mutex_delete(test2_m2);
    mutex_delete(test2_m3);
}

/*============================================================================
 * 测试 3: 单等待者（无环，不误报）
 *============================================================================*/

static mutex_id_t test3_m;
static volatile int test3_helper_result = -999;

static void test3_helper(void *arg) {
    (void)arg;
    kern_err_t err = mutex_lock(test3_m, 200);
    test3_helper_result = (int)err;
    if (err == KERN_OK) {
        mutex_unlock(test3_m);
    }
}

static void test_single_waiter(void) {
    test_section("Test 3: Single Waiter (No False Positive)");

    test3_helper_result = -999;
    test3_m = mutex_create();

    mutex_lock(test3_m, 100);

    task_id_t helper = task_create("waiter", test3_helper, NULL, 10, 0);
    task_start(helper);

    task_delay(30);

    mutex_unlock(test3_m);

    task_delay(30);

    TEST_ASSERT_EQ(KERN_OK, (kern_err_t)test3_helper_result,
                   "Single waiter got mutex (no false deadlock)");


    mutex_delete(test3_m);
}

/*============================================================================
 * 测试 4: 超时避免死锁（不误报）
 *============================================================================*/

static mutex_id_t test4_m1, test4_m2;
static volatile int test4_barrier = 0;
static volatile int test4_a_got_m2 = 0;
static volatile int test4_b_timeout = 0;

static void test4_task_a(void *arg) {
    (void)arg;
    mutex_lock(test4_m1, 200);
    test4_barrier = 1;
    task_delay(60);

    kern_err_t err = mutex_lock(test4_m2, 200);
    test4_a_got_m2 = (err == KERN_OK) ? 1 : 0;
    if (err == KERN_OK) {
        mutex_unlock(test4_m2);
    }
    mutex_unlock(test4_m1);
}

static void test4_task_b(void *arg) {
    (void)arg;
    while (!test4_barrier) {
        task_delay(1);
    }
    mutex_lock(test4_m2, 200);
    kern_err_t err = mutex_lock(test4_m1, 30);
    test4_b_timeout = (err == KERN_ERR_TIMEOUT) ? 1 : 0;
    if (err == KERN_OK) {
        mutex_unlock(test4_m1);
    }
    mutex_unlock(test4_m2);
}

static void test_timeout_break(void) {
    test_section("Test 4: Deadlock Avoided by Timeout");

    test4_barrier   = 0;
    test4_a_got_m2  = 0;
    test4_b_timeout = 0;

    test4_m1 = mutex_create();
    test4_m2 = mutex_create();

    task_id_t ta = task_create("t4_a", test4_task_a, NULL, 10, 0);
    task_id_t tb = task_create("t4_b", test4_task_b, NULL, 5, 0);

    task_start(ta);
    task_start(tb);

    task_delay(180);

    TEST_ASSERT(test4_b_timeout, "Task B timed out (broke potential cycle)");
    TEST_ASSERT(test4_a_got_m2, "Task A got m2 normally (no deadlock)");


    mutex_delete(test4_m1);
    mutex_delete(test4_m2);
}

/*============================================================================
 * 测试 5: 递归锁不误报死锁
 *============================================================================*/

static void test_recursive_no_deadlock(void) {
    test_section("Test 5: Recursive Lock (No Self-Deadlock)");

    mutex_id_t m = mutex_create();

    kern_err_t err1 = mutex_lock(m, 100);
    TEST_ASSERT_EQ(KERN_OK, err1, "First lock OK");

    kern_err_t err2 = mutex_lock(m, 100);
    TEST_ASSERT_EQ(KERN_OK, err2, "Recursive lock OK (not deadlock)");

    kern_err_t err3 = mutex_lock(m, 100);
    TEST_ASSERT_EQ(KERN_OK, err3, "Third recursive lock OK");

    mutex_unlock(m);
    mutex_unlock(m);
    mutex_unlock(m);

    mutex_delete(m);
    test_pass("Recursive lock");
}

/*============================================================================
 * 测试 6: 诊断 API mutex_deadlock_check()
 *============================================================================*/

static void test_diagnostic_api(void) {
    test_section("Test 6: Diagnostic API mutex_deadlock_check()");

    /* 子测试 6a: 干净系统无死锁 */
    int count = mutex_deadlock_check();
    TEST_ASSERT_EQ(0, count, "No deadlock in clean system");

    /* 子测试 6b: 单等待者无死锁 */
    test3_helper_result = -999;
    test3_m = mutex_create();
    mutex_lock(test3_m, 100);

    task_id_t helper = task_create("diag_h", test3_helper, NULL, 10, 0);
    task_start(helper);
    task_delay(10);

    count = mutex_deadlock_check();
    TEST_ASSERT_EQ(0, count, "Single waiter: no deadlock cycle");

    mutex_unlock(test3_m);
    task_delay(20);
    mutex_delete(test3_m);

    /* 子测试 6c: 两任务死锁场景后诊断确认无残留 */
    test1_barrier    = 0;
    test1_a_deadlock = 0;
    test1_b_done     = 0;

    test1_m1 = mutex_create();
    test1_m2 = mutex_create();

    task_id_t ta = task_create("dg_a", test1_task_a, NULL, 10, 0);
    task_id_t tb = task_create("dg_b", test1_task_b, NULL, 5, 0);

    task_start(ta);
    task_start(tb);

    task_delay(100);

    TEST_ASSERT(test1_a_deadlock, "Deadlock was detected and prevented");
    count = mutex_deadlock_check();
    TEST_ASSERT_EQ(0, count, "Diagnostic confirms no deadlock persists");


    mutex_delete(test1_m1);
    mutex_delete(test1_m2);
}

/*============================================================================
 * 死锁检测测试模块入口
 *============================================================================*/

static void test_deadlock_module(void) {
    test_two_task_deadlock();
    test_three_task_deadlock();
    test_single_waiter();
    test_timeout_break();
    test_recursive_no_deadlock();
    test_diagnostic_api();
}

/*============================================================================
 * 模块注册
 *============================================================================*/

TEST_MODULE_REGISTER(deadlock, test_deadlock_module);
