/**
 * @file test_task.c
 * @brief task_join/exit + sys_task_create 测试
 */

#include "test_framework.h"
#include "kernel.h"
#include "task.h"
#include "kernel_config.h"
#include "semaphore.h"

#if TEST_ENABLE

/*============================================================================
 * Test 1: task_exit 存储 retval
 *============================================================================*/

#define JOIN_TEST_VAL   ((void *)0x42)

static void join_helper_task(void *arg) {
    (void)arg;
    task_exit(JOIN_TEST_VAL);
}

static void test_join_retval(void) {
    test_section("Test 1: task_join receives retval");

    task_id_t tid = task_create("join_h1", join_helper_task, NULL, 10, 0);
    TEST_ASSERT(tid >= 0, "helper task created");

    task_start(tid);

    void *retval = NULL;
    kern_err_t err = task_join(tid, &retval, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "task_join returns OK");
    TEST_ASSERT(retval == JOIN_TEST_VAL, "retval matches 0x42");
}

/*============================================================================
 * Test 2: task_join 已终止的任务
 *============================================================================*/

static void test_join_already_exited(void) {
    test_section("Test 2: task_join already terminated task");

    task_id_t tid = task_create("join_h2", join_helper_task, NULL, 10, 0);
    TEST_ASSERT(tid >= 0, "helper task created");

    task_start(tid);
    task_delay(50);  /* 让 helper 执行并退出 */

    void *retval = NULL;
    kern_err_t err = task_join(tid, &retval, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "join already-terminated OK");
    TEST_ASSERT(retval == JOIN_TEST_VAL, "retval still available");
}

/*============================================================================
 * Test 3: task_join 不能 join 自己
 *============================================================================*/

static void test_join_self(void) {
    test_section("Test 3: task_join self rejected");

    task_id_t self = task_self();
    kern_err_t err = task_join(self, NULL, 0);
    TEST_ASSERT(err != KERN_OK, "join self returns error");
}

/*============================================================================
 * Test 4: task_join 无效 task_id
 *============================================================================*/

static void test_join_invalid(void) {
    test_section("Test 4: task_join invalid id");

    kern_err_t err = task_join(-1, NULL, 0);
    TEST_ASSERT(err != KERN_OK, "join id=-1 returns error");

    err = task_join(999, NULL, 0);
    TEST_ASSERT(err != KERN_OK, "join id=999 returns error");
}

/*============================================================================
 * Test 5: task_join 多个 joiner
 *============================================================================*/

static sem_id_t multi_sem;

static void multi_joiner_task(void *arg) {
    task_id_t target = (task_id_t)(uintptr_t)arg;
    task_join(target, NULL, 1000);
    sem_post(multi_sem);
}

static void multi_target_task(void *arg) {
    (void)arg;
    task_delay(20);
    task_exit((void *)0xAA);
}

static void test_join_multiple_joiners(void) {
    test_section("Test 5: multiple joiners on same task");

    multi_sem = sem_create(0, 2);
    TEST_ASSERT(multi_sem >= 0, "sem created");

    task_id_t target = task_create("mj_target", multi_target_task, NULL, 10, 0);
    TEST_ASSERT(target >= 0, "target created");

    /* 创建 2 个 joiner */
    task_id_t j1 = task_create("mj_j1", multi_joiner_task,
                               (void *)(uintptr_t)target, 10, 0);
    task_id_t j2 = task_create("mj_j2", multi_joiner_task,
                               (void *)(uintptr_t)target, 10, 0);
    TEST_ASSERT(j1 >= 0 && j2 >= 0, "joiners created");

    task_start(j1);
    task_start(j2);
    task_start(target);

    /* 等待两个 joiner 都完成 */
    kern_err_t r1 = sem_wait(multi_sem, 2000);
    kern_err_t r2 = sem_wait(multi_sem, 2000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)r1, "joiner 1 completed");
    TEST_ASSERT_EQ((int)KERN_OK, (int)r2, "joiner 2 completed");

    sem_delete(multi_sem);
}

/*============================================================================
 * Test 6: sys_task_create 基本功能
 *============================================================================*/

static void syscall_test_task(void *arg) {
    (void)arg;
}

static void test_sys_task_create(void) {
    test_section("Test 6: sys_task_create basic");

    /* 从内核任务调用 task_create_user (如果 MPU 使能) */
    task_id_t tid;
#if MPU_ENABLE
    tid = task_create_user("sys_tc", syscall_test_task, NULL, 10, 512);
#else
    tid = task_create("sys_tc", syscall_test_task, NULL, 10, 0);
#endif
    TEST_ASSERT(tid >= 0, "task_create_user returns valid id");

    task_start(tid);

    kern_err_t err = task_join(tid, NULL, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "user task executed");
}

/*============================================================================
 * Test 7: task_exit 不再 double-free (回归测试)
 *============================================================================*/

static void exit_no_crash_task(void *arg) {
    (void)arg;
    task_exit((void *)99);
}

static void test_exit_no_double_free(void) {
    test_section("Test 7: task_exit no double-free");

    task_id_t tid = task_create("ex_ndf", exit_no_crash_task, NULL, 10, 0);
    TEST_ASSERT(tid >= 0, "task created");

    task_start(tid);
    task_delay(50);

    /* 如果 double-free 存在，池会损坏 — 尝试分配新任务验证 */
    task_id_t tid2 = task_create("ex_chk", exit_no_crash_task, NULL, 10, 0);
    TEST_ASSERT(tid2 >= 0, "pool not corrupted after exit");
}

/*============================================================================
 * Test 8: 删除阻塞在 IPC wait queue 上的任务
 *============================================================================*/

static sem_id_t delete_blocked_sem;
static volatile int delete_blocked_ran = 0;

static void delete_blocked_task(void *arg) {
    (void)arg;
    delete_blocked_ran = 1;
    (void)sem_wait(delete_blocked_sem, 1000);
    delete_blocked_ran = 2;
    task_exit(NULL);
}

static void test_delete_blocked_task(void) {
    test_section("Test 8: delete blocked task");

    delete_blocked_ran = 0;
    delete_blocked_sem = sem_create(0, 1);
    TEST_ASSERT(delete_blocked_sem >= 0, "sem created");
    if (delete_blocked_sem < 0) return;

    task_id_t tid = task_create("del_blk", delete_blocked_task, NULL, 10, 0);
    TEST_ASSERT(tid >= 0, "blocked task created");
    if (tid >= 0) {
        task_start(tid);
        task_delay(10);
        TEST_ASSERT_EQ(1, delete_blocked_ran, "task reached sem_wait");

        kern_err_t err = task_delete(tid);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err, "delete blocked task OK");

        err = sem_post(delete_blocked_sem);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err, "sem still usable after task delete");
    }

    sem_delete(delete_blocked_sem);
}

/*============================================================================
 * Test 9: task_terminate_with_result propagates join reason
 *============================================================================*/

static void terminate_reason_task(void *arg) {
    (void)arg;
    while (1) {
        task_delay(10);
    }
}

static void test_terminate_join_result(void) {
    test_section("Test 9: terminate result reaches join");

    task_id_t tid = task_create("term_res", terminate_reason_task, NULL, 10, 0);
    TEST_ASSERT(tid >= 0, "terminate target created");
    if (tid < 0) return;

    task_start(tid);
    task_delay(10);

    tcb_t *tcb = task_get_tcb(tid);
    TEST_ASSERT_NOT_NULL(tcb, "terminate target tcb exists");
    if (!tcb) return;

    kern_err_t err = task_terminate_with_result(tcb, KERN_ERR_FAULT);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "terminate with result OK");

    void *retval = (void *)0x1234;
    err = task_join(tid, &retval, 0);
    TEST_ASSERT_EQ((int)KERN_ERR_FAULT, (int)err, "join sees fault result");
    TEST_ASSERT(retval == NULL, "terminate result has no retval");

    (void)task_delete(tid);
}

/*============================================================================
 * Test 10: task bitmap covers IDs above 31
 *============================================================================*/

static void test_task_bitmap_high_half(void) {
    test_section("Test 10: 64-bit task allocation bitmap");

#if KERNEL_MAX_TASKS > 32
    task_id_t ids[KERNEL_MAX_TASKS];
    int created = 0;
    task_id_t high_id = KERN_INVALID_ID;

    while (created < KERNEL_MAX_TASKS) {
        task_id_t id = task_create("bm_hi", syscall_test_task,
                                   NULL, 10, 512);
        if (id < 0) {
            break;
        }
        ids[created++] = id;
        if (id >= 32) {
            high_id = id;
            break;
        }
    }

    TEST_ASSERT(high_id >= 32, "task allocator reaches ID 32 or above");
    if (high_id >= 32) {
        uint64_t bitmap = task_get_used_bitmap();
        TEST_ASSERT((bitmap & (1ULL << high_id)) != 0,
                    "high task ID is represented in allocation bitmap");
    }

    for (int i = 0; i < created; i++) {
        (void)task_delete(ids[i]);
    }
#else
    test_pass("32-bit task configuration needs no high-half bitmap");
#endif
}

static void test_initial_arg_update(void) {
    test_section("Test 11: update initial task argument");

    task_id_t tid = task_create_user("arg_update", syscall_test_task,
                                     NULL, 10, 512);
    TEST_ASSERT(tid >= 0, "initial-argument task created");
    if (tid < 0) {
        return;
    }

    void *expected = (void *)(uintptr_t)0x1234U;
    kern_err_t err = task_set_initial_arg(tid, expected);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "created task initial argument updated");

    tcb_t *tcb = task_get_tcb(tid);
    uint32_t *stacked_r0 = (uint32_t *)((uint8_t *)tcb->stack_base +
                                        tcb->stack_size - 32U);
    TEST_ASSERT_EQ((uintptr_t)expected, (uintptr_t)*stacked_r0,
                   "updated argument stored in hardware R0 frame");

    (void)task_delete(tid);
}

/*============================================================================
 * Module registration
 *============================================================================*/

static void test_task_module(void) {
    test_join_retval();
    test_join_already_exited();
    test_join_self();
    test_join_invalid();
    test_join_multiple_joiners();
    test_sys_task_create();
    test_exit_no_double_free();
    test_delete_blocked_task();
    test_terminate_join_result();
    test_task_bitmap_high_half();
    test_initial_arg_update();
}

TEST_MODULE_REGISTER(task, test_task_module);

#endif /* TEST_ENABLE */
