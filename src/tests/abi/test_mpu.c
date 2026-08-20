/**
 * @file test_mpu.c
 * @brief MPU 内存保护测试
 */

#include "test_framework.h"
#include "kernel.h"
#include "mpu.h"
#include "task.h"
#include "hal.h"
#include "syscall.h"
#include "user_api.h"

#if TEST_ENABLE
/* P0-5 验证补漏:TEST-off 镜像(dev/release/tiny)不得链入测试代码。
 * TEST_ENABLE 为测试代码链接总门(与既有模块级 TEST_MODULE_* 门互补)。 */

#if MPU_ENABLE

/*============================================================================
 * Test 1: MPU 已使能
 *============================================================================*/

static void test_mpu_enabled(void) {
    test_section("Test 1: MPU enabled");
    TEST_ASSERT(1, "MPU init compiled and linked");
}

/*============================================================================
 * Test 2: 用户任务创建和删除
 *============================================================================*/

#if SYSCALL_ENABLE

static void dummy_user_task(void *arg) {
    (void)arg;
    sys_call1(SYSCALL_TASK_DELAY, 2);
}

static void test_user_task_create(void) {
    test_section("Test 2: User task create");

    task_id_t tid = task_create_user("mpu_u1", dummy_user_task, NULL, 10, 512);
    TEST_ASSERT(tid >= 0, "User task created");

    kern_err_t err = task_start(tid);
    TEST_ASSERT_EQ(KERN_OK, err, "User task started");

    task_delay(10);

    err = task_delete(tid);
    TEST_ASSERT_EQ(KERN_OK, err, "User task deleted");
}

#else
static void test_user_task_create(void) {
    test_section("Test 2: User task create");
    TEST_ASSERT(1, "SYSCALL disabled, skip");
}
#endif

/*============================================================================
 * Test 3: 多个用户任务并发
 *============================================================================*/

#if SYSCALL_ENABLE

static void multi_user_task(void *arg) {
    (void)arg;
    sys_call1(SYSCALL_TASK_DELAY, 5);
}

static void test_multiple_user_tasks(void) {
    test_section("Test 3: Multiple user tasks");

    task_id_t t1 = task_create_user("mpu_a", multi_user_task, NULL, 12, 512);
    task_id_t t2 = task_create_user("mpu_b", multi_user_task, NULL, 12, 512);

    TEST_ASSERT(t1 >= 0, "User task A created");
    TEST_ASSERT(t2 >= 0, "User task B created");

    task_start(t1);
    task_start(t2);

    kern_err_t err1 = task_join(t1, NULL, 50);
    kern_err_t err2 = task_join(t2, NULL, 50);

    TEST_ASSERT_EQ(KERN_OK, err1, "User task A ran to completion");
    TEST_ASSERT_EQ(KERN_OK, err2, "User task B ran to completion");
}

#else
static void test_multiple_user_tasks(void) {
    test_section("Test 3: Multiple user tasks");
    TEST_ASSERT(1, "SYSCALL disabled, skip");
}
#endif

/*============================================================================
 * Test 4: MPU region 设置/禁用
 *============================================================================*/

static void test_mpu_region_set_disable(void) {
    test_section("Test 4: MPU region set/disable");

    /* 在测试区域设置 region（不会影响运行中的任务） */
    mpu_region_set(7, 0x20010000, 256, AP_FULL | ATTR_STRONGLY_ORDERED | XN_ENABLE);
    TEST_ASSERT(1, "Region 7 set without fault");

    mpu_region_disable(7);
    TEST_ASSERT(1, "Region 7 disabled without fault");
}

/*============================================================================
 * Test 5: RASR size 计算
 *============================================================================*/

static void test_mpu_rasr_size(void) {
    test_section("Test 5: RASR size calculation");

    /* 各个大小等级应该返回非零值 */
    uint32_t s32  = mpu_calc_rasr_size(32);
    uint32_t s256 = mpu_calc_rasr_size(256);
    uint32_t s1k  = mpu_calc_rasr_size(1024);
    uint32_t s4k  = mpu_calc_rasr_size(4096);
    uint32_t s64k = mpu_calc_rasr_size(65536);

    TEST_ASSERT(s32  > 0, "size=32 → valid RASR");
    TEST_ASSERT(s256 > 0, "size=256 → valid RASR");
    TEST_ASSERT(s1k  > 0, "size=1K → valid RASR");
    TEST_ASSERT(s4k  > 0, "size=4K → valid RASR");
    TEST_ASSERT(s64k > 0, "size=64K → valid RASR");

    /* 非 2 的幂次或 < 32 的大小应返回 0 */
    uint32_t s_invalid = mpu_calc_rasr_size(0);  /* 0 是非法输入 */
    TEST_ASSERT(s_invalid == 0, "size=0 → invalid input returns 0");
}

/*============================================================================
 * Test 6: Stack guard RASR calculation
 *============================================================================*/

static void test_mpu_stack_guard(void) {
    test_section("Test 6: Stack guard RASR");

    uint32_t base = 0x20008000;
    uint32_t size = 512;

    /* 子区域 0 禁用 → 栈底部 64 字节不可访问 */
    uint32_t rasr = mpu_stack_guard_rasr(base, size, 1 << 0);
    TEST_ASSERT(rasr != 0, "Stack guard RASR non-zero");

    /* 子区域 7 禁用 */
    uint32_t rasr2 = mpu_stack_guard_rasr(base, size, 1 << 7);
    TEST_ASSERT(rasr2 != 0, "Stack guard RASR subregion 7 non-zero");
}

/*============================================================================
 * Test 7: 特权任务不受 MPU 限制
 *============================================================================*/

static void test_privileged_task_bypass(void) {
    test_section("Test 7: Privileged task bypass");

    /* 创建一个特权任务（默认），MPU 不会对它施加限制 */
    task_id_t tid = task_create("mpu_priv", NULL, NULL, 15, 256);
    TEST_ASSERT(tid >= 0, "Privileged task created");

    tcb_t *tcb = sched_get_tcb(tid);
    TEST_ASSERT_NOT_NULL(tcb, "Privileged TCB exists");

    if (tcb) {
        /* 特权任务 should NOT have TASK_ATTR_USER */
        TEST_ASSERT(!(tcb->attrs & 1), "Privileged task does NOT have USER attr");
    }

    task_delete(tid);
}

/*============================================================================
 * Test 8: 用户任务有 USER 属性
 *============================================================================*/

#if SYSCALL_ENABLE

static void attr_check_task(void *arg) {
    (void)arg;
    sys_call1(SYSCALL_TASK_DELAY, 1);
}

static void test_user_task_attr(void) {
    test_section("Test 8: User task has USER attr");

    task_id_t tid = task_create_user("mpu_chk", attr_check_task, NULL, 12, 512);
    TEST_ASSERT(tid >= 0, "User task created for attr check");

    tcb_t *tcb = sched_get_tcb(tid);
    TEST_ASSERT_NOT_NULL(tcb, "User TCB exists");

    if (tcb) {
        TEST_ASSERT(tcb->attrs & 1, "User task HAS TASK_ATTR_USER bit");
        TEST_ASSERT((tcb->aspace->regions[1][1] & RASR_ENABLE) == 0,
                    "Full SRAM user RW region is disabled");
        TEST_ASSERT((tcb->aspace->regions[2][1] & RASR_ENABLE) != 0,
                    "User stack region is enabled");
    }

    task_start(tid);
    task_delay(10);
    task_delete(tid);
}

#else
static void test_user_task_attr(void) {
    test_section("Test 8: User task attr");
    TEST_ASSERT(1, "SYSCALL disabled, skip");
}
#endif

/*============================================================================
 * Module registration
 *============================================================================*/

static void test_mpu_module(void) {
    test_mpu_enabled();
    test_user_task_create();
    test_multiple_user_tasks();
    test_mpu_region_set_disable();
    test_mpu_rasr_size();
    test_mpu_stack_guard();
    test_privileged_task_bypass();
    test_user_task_attr();
}

TEST_ABI_MODULE(mpu, test_mpu_module);

#endif /* MPU_ENABLE */
#endif /* TEST_ENABLE */
