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
/* Phase F2: vfs.h 移除 (内核 VFS 直调已清除) */

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
 * Test 9: 用户 fault 后释放打开的 fd
 *============================================================================*/

#if MPU_ENABLE && VFS_ENABLE
static void __attribute__((unused)) fault_task_open_fd_then_fault(void *arg) {
    (void)arg;

    (void)open("/tmp/fault_fd_cleanup", O_RDWR);
    sys_call1(SYSCALL_TASK_DELAY, 20);

    volatile uint32_t *p = (volatile uint32_t *)0xBBBBBBBB;
    *p = 0xDEAD;
    task_exit((void *)0xBAD);
}
#endif

static void test_fault_releases_fd_refs(void) {
    /* Phase F2:内核 VFS 移除,vfs_open/vfs_lookup 不可用。
     * fd 死亡清理已由 fs_server 的 kern.fault 订阅 + fs_store_close_client_fds
     * 覆盖,见 test_fs_fd_cleanup.c (端到端验证)。
     * 这个测试测的是旧的内核 fd_table refcount,已无意义。 */
    test_section("Test 9: fault fd cleanup (migrated to fs_server)");
    test_pass("fd cleanup via fs_server kern.fault (see test_fs_fd_cleanup)");
}

/*============================================================================
 * Module registration
 *============================================================================*/

static void test_fault_module(void) {
    test_crash_dump_size();
    test_crash_dump_offsets();
    test_fault_type_distinct();
    test_hardfault_print_symbol();
    test_fault_releases_fd_refs();
}

TEST_K_MODULE(fault, test_fault_module);

#endif /* FAULT_ENABLE && TEST_MODULE_FAULT */
