/**
 * @file test_fault_user.c
 * @brief ABI 层 — 用户任务 fault 契约
 *
 * 验证用户态可观察的故障行为:用户任务 fault 后被终止、内核存活、
 * 资源(cap)被回收。这些是用户任务存在才有意义的契约,归 ABI 层。
 */

#include "test_framework.h"
#include "kernel.h"
#include "task.h"
#include "scheduler.h"
#include "hal.h"
#include "user_api.h"
#if CAP_ENABLE
#include "capability.h"
#endif

#if FAULT_ENABLE && TEST_MODULE_FAULT

/*============================================================================
 * 用户 fault 注入 helper
 *============================================================================*/

#if MPU_ENABLE
static void fault_task_null_write(void *arg) {
    (void)arg;
    /* 0xBBBBBBBB 未被任何 MPU region 覆盖，保证触发 MemManage fault
     * (0x00000000 在 STM32F767 上被 boot aliasing 映射到 Flash，
     *  写入会触发 BusFault 而非 MemManage) */
    volatile uint32_t *p = (volatile uint32_t *)0xBBBBBBBB;
    *p = 0xDEAD;  /* MemManage fault */
    /* 不应到达这里 */
    task_exit((void *)0xBAD);
}
#endif

/*============================================================================
 * Test: 用户任务 MemManage — 写 NULL 区域
 *============================================================================*/

static void test_user_memmanage_null(void) {
    test_section("user MemManage — NULL write");

#if MPU_ENABLE
    task_id_t tid = task_create_user("f_null", fault_task_null_write,
                                     NULL, 10, 512);
    TEST_ASSERT(tid >= 0, "fault task created");
    if (tid < 0) return;

    task_start(tid);

    void *retval = NULL;
    kern_err_t err = task_join(tid, &retval, 2000);
    TEST_ASSERT_EQ((int)KERN_ERR_FAULT, (int)err, "task_join returns FAULT");
    /* retval 不是 0xBAD — 说明不是正常退出 */
    TEST_ASSERT(retval != (void *)0xBAD, "task did not exit normally");
#else
    test_skip("MPU not enabled");
#endif
}

/*============================================================================
 * Test: 用户任务 UsageFault — 除零
 *============================================================================*/

#if MPU_ENABLE
static void fault_task_divzero(void *arg) {
    (void)arg;
    volatile int a = 1;
    volatile int b = 0;
    volatile int c = a / b;  /* UsageFault (if DIVBYZERO enabled) */
    (void)c;
    task_exit((void *)0xBAD);
}
#endif

static void test_user_usagefault_divzero(void) {
    test_section("user UsageFault — divide by zero");

#if MPU_ENABLE
    /* 使能 DIVBYZERO trap (CCR bit 4) */
    volatile uint32_t *ccr = (volatile uint32_t *)0xE000ED14;
    uint32_t saved_ccr = *ccr;
    *ccr = saved_ccr | (1U << 4);
    __asm volatile("dsb; isb");

    task_id_t tid = task_create_user("f_dz", fault_task_divzero,
                                     NULL, 10, 512);
    TEST_ASSERT(tid >= 0, "divzero task created");
    if (tid < 0) {
        *ccr = saved_ccr;
        return;
    }

    task_start(tid);

    void *retval = NULL;
    kern_err_t err = task_join(tid, &retval, 2000);
    TEST_ASSERT_EQ((int)KERN_ERR_FAULT, (int)err, "task_join returns FAULT");
    TEST_ASSERT(retval != (void *)0xBAD, "task did not exit normally");

    /* 恢复 CCR */
    *ccr = saved_ccr;
    __asm volatile("dsb; isb");
#else
    test_skip("MPU not enabled");
#endif
}

/*============================================================================
 * Test: 用户 fault 后内核存活
 *============================================================================*/

#if MPU_ENABLE
static volatile int post_fault_ok = 0;
static void post_fault_task(void *arg) {
    (void)arg;
    post_fault_ok = 1;
    task_exit(NULL);
}
#endif

static void test_kernel_survives_user_fault(void) {
    test_section("kernel survives user fault");

#if MPU_ENABLE
    /* 先触发一个 fault */
    task_id_t tid = task_create_user("f_surv", fault_task_null_write,
                                     NULL, 10, 512);
    TEST_ASSERT(tid >= 0, "fault task created");
    if (tid >= 0) {
        task_start(tid);
        task_join(tid, NULL, 2000);
    }

    /* 内核应仍然存活 — 创建新任务验证 */
    post_fault_ok = 0;
    task_id_t tid2 = task_create("f_post", post_fault_task, NULL, 10, 0);
    TEST_ASSERT(tid2 >= 0, "post-fault task created");
    if (tid2 >= 0) {
        task_start(tid2);
        task_delay(50);
        TEST_ASSERT_EQ(1, post_fault_ok, "post-fault task ran successfully");
    }
#else
    test_skip("MPU not enabled");
#endif
}

/*============================================================================
 * Test: 用户 fault 后 crash_dump 字段验证
 *============================================================================*/

static void test_crash_dump_after_fault(void) {
    test_section("crash_dump after user fault");

#if MPU_ENABLE
    extern crash_dump_t crash_dump;

    /* 清零 dump */
    crash_dump.fault_type = 0xFF;
    crash_dump.task_id = -1;
    crash_dump.pc = 0;

    /* 触发 MemManage */
    task_id_t tid = task_create_user("f_dump", fault_task_null_write,
                                     NULL, 10, 512);
    TEST_ASSERT(tid >= 0, "fault task created");
    if (tid < 0) return;

    task_start(tid);
    task_join(tid, NULL, 2000);


    /* 验证 crash_dump 字段 */
    TEST_ASSERT_EQ((int)FAULT_TYPE_MEMMANAGE, (int)crash_dump.fault_type,
                   "crash_dump.fault_type == MEMMANAGE");
    TEST_ASSERT(crash_dump.pc != 0, "crash_dump.pc is non-zero");
    TEST_ASSERT(crash_dump.psp != 0, "crash_dump.psp is non-zero");
#else
    test_skip("MPU not enabled");
#endif
}

/*============================================================================
 * Test: 用户 fault 后撤销该任务拥有的 cap
 *============================================================================*/

static void test_fault_releases_caps(void) {
    test_section("user fault releases caps");

#if MPU_ENABLE && CAP_ENABLE
    static int fault_cap_object;

    uint16_t base_refs = cap_object_refcount(&fault_cap_object, CAP_OBJ_SEMAPHORE);
    task_id_t tid = task_create_user("f_cap", fault_task_null_write,
                                     NULL, 10, 512);
    TEST_ASSERT(tid >= 0, "fault cap task created");
    if (tid < 0) return;

    cap_id_t cap = cap_create(&fault_cap_object, CAP_OBJ_SEMAPHORE,
                              CAP_FULL, (uint8_t)tid);
    TEST_ASSERT(cap != ((cap_id_t)-1), "owned cap created for fault task");
    if (cap == ((cap_id_t)-1)) {
        (void)task_delete(tid);
        return;
    }

    TEST_ASSERT_EQ((int)(base_refs + 1),
                   (int)cap_object_refcount(&fault_cap_object, CAP_OBJ_SEMAPHORE),
                   "fault task owns test cap");

    task_start(tid);

    void *retval = NULL;
    kern_err_t err = task_join(tid, &retval, 2000);
    TEST_ASSERT_EQ((int)KERN_ERR_FAULT, (int)err, "fault cap task joined as fault");
    TEST_ASSERT_EQ((int)base_refs,
                   (int)cap_object_refcount(&fault_cap_object, CAP_OBJ_SEMAPHORE),
                   "fault cleanup revoked owned cap");
#else
    test_skip("MPU or CAP not enabled");
#endif
}

/*============================================================================
 * Module registration
 *============================================================================*/

static void test_fault_user_module(void) {
    test_user_memmanage_null();
    test_user_usagefault_divzero();
    test_kernel_survives_user_fault();
    test_crash_dump_after_fault();
    test_fault_releases_caps();
}

TEST_ABI_MODULE(fault_user, test_fault_user_module);

#endif /* FAULT_ENABLE && TEST_MODULE_FAULT */
