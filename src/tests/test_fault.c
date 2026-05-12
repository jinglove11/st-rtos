/**
 * @file test_fault.c
 * @brief Fault Handler 测试 — 结构验证 + fault 注入
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
#if VFS_ENABLE
#include "vfs.h"
#endif

#if FAULT_ENABLE && TEST_MODULE_FAULT

/*============================================================================
 * Test 1: crash_dump_t size within 128 bytes
 *============================================================================*/

static void test_crash_dump_size(void) {
    test_section("Test 1: crash_dump_t size");

    TEST_ASSERT(sizeof(crash_dump_t) <= 128,
                "crash_dump_t fits in 128 bytes");
    TEST_ASSERT(sizeof(crash_dump_t) >= 64,
                "crash_dump_t has minimum expected size");
}

/*============================================================================
 * Test 2: crash_dump_t offset alignment
 *============================================================================*/

static void test_crash_dump_offsets(void) {
    test_section("Test 2: crash_dump_t offsets");

    crash_dump_t dummy;
    uintptr_t base = (uintptr_t)&dummy;

    TEST_ASSERT(((uintptr_t)&dummy.r4  - base) % 4 == 0, "r4 aligned");
    TEST_ASSERT(((uintptr_t)&dummy.msp - base) % 4 == 0, "msp aligned");
    TEST_ASSERT(((uintptr_t)&dummy.fault_type - base) % 4 == 0,
                "fault_type aligned");
}

/*============================================================================
 * Test 3: fault type constants are distinct
 *============================================================================*/

static void test_fault_type_distinct(void) {
    test_section("Test 3: fault type constants");

    TEST_ASSERT(FAULT_TYPE_HARD != FAULT_TYPE_MEMMANAGE, "HARD != MEMMANAGE");
    TEST_ASSERT(FAULT_TYPE_HARD != FAULT_TYPE_BUS, "HARD != BUS");
    TEST_ASSERT(FAULT_TYPE_HARD != FAULT_TYPE_USAGE, "HARD != USAGE");
    TEST_ASSERT(FAULT_TYPE_MEMMANAGE != FAULT_TYPE_BUS, "MEMMANAGE != BUS");
}

/*============================================================================
 * Test 4: hardfault_print exists and callable
 *============================================================================*/

extern void hardfault_print(uint32_t psp, uint32_t msp,
                            uint32_t cfsr, uint32_t hfsr);

static void test_hardfault_print_symbol(void) {
    test_section("Test 4: hardfault_print symbol");

    TEST_ASSERT((uintptr_t)hardfault_print != 0,
                "hardfault_print is linked");
}

/*============================================================================
 * Test 5: 用户任务 MemManage — 写 NULL 区域
 *
 * 用户任务尝试写 0xBBBBBBBB (无 MPU region 覆盖)。
 * 应触发 MemManage fault，fault handler 终止任务，内核继续运行。
 *============================================================================*/

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

static void test_user_memmanage_null(void) {
    test_section("Test 5: user MemManage — NULL write");

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
 * Test 6: 用户任务 UsageFault — 除零
 *
 * 需要 CCR.DIVBYZEROENA = 1 才能触发 UsageFault。
 * 用户任务做 1/0 触发异常。
 *============================================================================*/

static void fault_task_divzero(void *arg) {
    (void)arg;
    volatile int a = 1;
    volatile int b = 0;
    volatile int c = a / b;  /* UsageFault (if DIVBYZERO enabled) */
    (void)c;
    task_exit((void *)0xBAD);
}

static void test_user_usagefault_divzero(void) {
    test_section("Test 6: user UsageFault — divide by zero");

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
 * Test 7: 用户 fault 后内核存活
 *
 * 触发 fault 后，验证内核仍能正常创建和调度新任务。
 *============================================================================*/

static volatile int post_fault_ok = 0;

static void post_fault_task(void *arg) {
    (void)arg;
    post_fault_ok = 1;
    task_exit(NULL);
}

static void test_kernel_survives_user_fault(void) {
    test_section("Test 7: kernel survives user fault");

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
 * Test 8: crash_dump 字段验证
 *
 * 用户 fault 后，检查 crash_dump 中的关键字段。
 *============================================================================*/

static void test_crash_dump_after_fault(void) {
    test_section("Test 8: crash_dump after fault");

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
 * Test 9: 用户 fault 后释放打开的 fd
 *============================================================================*/

#if MPU_ENABLE && VFS_ENABLE
static void fault_task_open_fd_then_fault(void *arg) {
    (void)arg;

    (void)open("/tmp/fault_fd_cleanup", O_RDWR);
    sys_call1(SYSCALL_TASK_DELAY, 20);

    volatile uint32_t *p = (volatile uint32_t *)0xBBBBBBBB;
    *p = 0xDEAD;
    task_exit((void *)0xBAD);
}
#endif

static void test_fault_releases_fd_refs(void) {
    test_section("Test 9: fault releases fd refs");

#if MPU_ENABLE && VFS_ENABLE
    int setup = vfs_open("/tmp/fault_fd_cleanup", O_RDWR | O_CREAT);
    TEST_ASSERT(setup >= 0, "create fault fd cleanup file");
    if (setup >= 0) {
        vfs_close(setup);
    }

    inode_t *ino = vfs_lookup("/tmp/fault_fd_cleanup");
    TEST_ASSERT_NOT_NULL(ino, "lookup fault fd cleanup inode");
    if (!ino) return;

    uint32_t base_ref = ino->refcount;

    task_id_t tid = task_create_user("f_fd", fault_task_open_fd_then_fault,
                                     NULL, 10, 512);
    TEST_ASSERT(tid >= 0, "fault fd task created");
    if (tid < 0) {
        inode_put(ino);
        return;
    }

    task_start(tid);
    task_delay(5);

    TEST_ASSERT_EQ((int)(base_ref + 1), (int)ino->refcount,
                   "fault task fd holds inode ref");

    void *retval = NULL;
    kern_err_t err = task_join(tid, &retval, 2000);
    TEST_ASSERT_EQ((int)KERN_ERR_FAULT, (int)err, "fault fd task joined as fault");
    TEST_ASSERT_EQ((int)base_ref, (int)ino->refcount,
                   "fault cleanup released fd inode ref");

    inode_put(ino);
#else
    test_skip("MPU or VFS not enabled");
#endif
}

/*============================================================================
 * Test 10: 用户 fault 后撤销该任务拥有的 cap
 *============================================================================*/

static void test_fault_releases_caps(void) {
    test_section("Test 10: fault releases caps");

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

static void test_fault_module(void) {
    test_crash_dump_size();
    test_crash_dump_offsets();
    test_fault_type_distinct();
    test_hardfault_print_symbol();
    test_user_memmanage_null();
    test_user_usagefault_divzero();
    test_kernel_survives_user_fault();
    test_crash_dump_after_fault();
    test_fault_releases_fd_refs();
    test_fault_releases_caps();
}

TEST_MODULE_REGISTER(fault, test_fault_module);

#endif /* FAULT_ENABLE && TEST_MODULE_FAULT */
