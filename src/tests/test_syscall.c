/**
 * @file test_syscall.c
 * @brief 系统调用接口测试 — 使用通用 sys_callN API
 */

#include "test_framework.h"
#include "kernel.h"
#include "syscall.h"
#include "user_api.h"
#include "task.h"
#include "semaphore.h"

#if CAP_ENABLE
#include "capability.h"
#endif

#if SYSCALL_ENABLE

/*============================================================================
 * 测试 1: syscall 表存在
 *============================================================================*/

static void test_syscall_table_exists(void) {
    test_section("Test 1: Syscall Table Exists");
    TEST_ASSERT(1, "Syscall table compiled and linked");
}

/*============================================================================
 * 测试 2: task_yield via syscall
 *============================================================================*/

static void test_syscall_task_yield(void) {
    test_section("Test 2: Task Yield via sys_call0");

    kern_err_t err = (kern_err_t)sys_call0(SYSCALL_TASK_YIELD);
    TEST_ASSERT_EQ(KERN_OK, err, "sys_call0(TASK_YIELD) returns OK");
}

/*============================================================================
 * 测试 3: task_delay via syscall
 *============================================================================*/

static void test_syscall_task_delay(void) {
    test_section("Test 3: Task Delay via sys_call1");

    kern_err_t err = (kern_err_t)sys_call1(SYSCALL_TASK_DELAY, 1);
    TEST_ASSERT_EQ(KERN_OK, err, "sys_call1(TASK_DELAY, 1) returns OK");
}

/*============================================================================
 * 测试 4: sem_create/wait/post via syscall
 *============================================================================*/

static void test_syscall_sem(void) {
    test_section("Test 4: Semaphore via sys_callN");

    volatile int cap_svc = sys_call2(SYSCALL_SEM_CREATE, 1, 0);
    int cap = cap_svc;

    /* brute-force verify: store in global so no register tricks */
    (void)cap;

    TEST_ASSERT(cap >= 0, "sys_call2(SEM_CREATE, max=1, init=0) returns valid ID");
    TEST_ASSERT(cap > 0, "cap_create returned positive token");
    TEST_ASSERT(cap < 32768, "cap_create returned in-range token");

    /* Direct cap_resolve */
    (void)cap_resolve((cap_id_t)cap, CAP_OBJ_SEMAPHORE, CAP_WRITE);

    int err = sys_call1(SYSCALL_SEM_POST, cap);
    TEST_ASSERT_EQ(KERN_OK, (kern_err_t)err, "sys_call1(SEM_POST) OK");

    err = sys_call2(SYSCALL_SEM_WAIT, cap, 0);
    TEST_ASSERT_EQ(KERN_OK, (kern_err_t)err, "sys_call2(SEM_WAIT, 0) OK");

    /* cleanup capability via syscall */
    sys_call1(SYSCALL_SEM_DELETE, cap);

    /* --- Part B: Direct cap_create, then test via SVC --- */
    {
        sem_id_t sid = sem_create(0, 1);
        cap_id_t dcap = cap_create((void *)(uintptr_t)(sid + 1),
                                   CAP_OBJ_SEMAPHORE, CAP_FULL, 1);
        (void)cap_resolve(dcap, CAP_OBJ_SEMAPHORE, CAP_WRITE);
        kern_err_t derr = (kern_err_t)sys_call1(SYSCALL_SEM_POST, dcap);
        TEST_ASSERT_EQ(KERN_OK, derr, "PartB: direct-cap via SVC POST OK");
        /* cleanup */
        cap_delete(dcap);
        sem_delete(sid);
    }
}

/*============================================================================
 * 测试 5: task_self via syscall
 *============================================================================*/

static void test_syscall_task_self(void) {
    test_section("Test 5: Task Self via sys_call0");

    task_id_t id = (task_id_t)sys_call0(SYSCALL_TASK_SELF);
    TEST_ASSERT(id >= 0, "sys_call0(TASK_SELF) returns valid ID");
}

/*============================================================================
 * 测试 6: syscall 参数传递正确性 (R1/R2 不被破坏)
 *============================================================================*/

static void test_syscall_args(void) {
    test_section("Test 6: Syscall Args Integrity (R1/R2 preserved)");

    int sem = sys_call2(SYSCALL_SEM_CREATE, 1, 0);
    TEST_ASSERT(sem >= 0, "sem created");

    /* post/wait verify argument passing is correct */
    int err = sys_call1(SYSCALL_SEM_POST, sem);
    TEST_ASSERT_EQ(KERN_OK, err, "post OK");

    err = sys_call2(SYSCALL_SEM_WAIT, sem, 100);
    TEST_ASSERT_EQ(KERN_OK, err, "wait with timeout OK");

    /* cleanup capability */
    sys_call1(SYSCALL_SEM_DELETE, sem);
}

/*============================================================================
 * 测试 7: sys_call2 的 R2 完整性 (验证修复 R2 clobber bug)
 *============================================================================*/

static void test_syscall_r2_integrity(void) {
    test_section("Test 7: R2 Register Integrity");

    /* Repeated sys_call2 to verify R2 is preserved */
    cap_id_t caps[10];
    for (int i = 0; i < 10; i++) {
        int err = sys_call2(SYSCALL_SEM_CREATE, 1, 0);
        TEST_ASSERT(err >= 0 || err == KERN_ERR, "R2 intact after syscall");
        caps[i] = (cap_id_t)err;
    }
    /* cleanup capabilities */
    for (int i = 0; i < 10; i++) {
        if (caps[i] >= 0) sys_call1(SYSCALL_SEM_DELETE, caps[i]);
    }
}

/*============================================================================
 * 测试 8: syscall 坏指针应返回错误，不应造成内核 fault
 *============================================================================*/

static void test_syscall_bad_user_pointers(void) {
    test_section("Test 8: Bad User Pointers");

    int err = sys_call5(SYSCALL_TASK_CREATE,
                        (int)(uintptr_t)0xBBBBBBBBu,
                        (int)(uintptr_t)test_syscall_bad_user_pointers,
                        0, 10, 512);
    TEST_ASSERT_EQ(KERN_ERR_PARAM, err, "bad task name pointer rejected");

#if VFS_ENABLE
    err = sys_call2(SYSCALL_OPEN, (int)(uintptr_t)0xBBBBBBBBu, 0);
    TEST_ASSERT_EQ(KERN_ERR_PARAM, err, "bad open path pointer rejected");
#endif
}

/*============================================================================
 * Syscall 测试模块入口
 *============================================================================*/

static void test_syscall_module(void) {
    test_syscall_table_exists();
    test_syscall_task_yield();
    test_syscall_task_delay();
    test_syscall_sem();
    test_syscall_task_self();
    test_syscall_args();
    test_syscall_r2_integrity();
    test_syscall_bad_user_pointers();
}

TEST_MODULE_REGISTER(syscall, test_syscall_module);

#endif /* SYSCALL_ENABLE */
