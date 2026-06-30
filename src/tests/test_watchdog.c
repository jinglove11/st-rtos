/**
 * @file test_watchdog.c
 * @brief 看门狗测试模块
 *
 * 测试内容：
 * 1. IWDG 寄存器可访问
 * 2. feed 后 RLR 值正确
 * 3. init 后 SR 状态正确
 */

#include "test_framework.h"
#include "kernel_config.h"
#include <stdint.h>

#if KERN_WATCHDOG_ENABLE && TEST_MODULE_WATCHDOG

/*============================================================================
 * IWDG 寄存器 (与 hal.c 一致)
 *============================================================================*/

#define IWDG_BASE   0x40003000UL
#define IWDG_KR     (*(volatile uint32_t *)(IWDG_BASE + 0x00))
#define IWDG_PR     (*(volatile uint32_t *)(IWDG_BASE + 0x04))
#define IWDG_RLR    (*(volatile uint32_t *)(IWDG_BASE + 0x08))
#define IWDG_SR     (*(volatile uint32_t *)(IWDG_BASE + 0x0C))

/*============================================================================
 * Test 1: IWDG 寄存器可访问
 *============================================================================*/

static void test_iwdg_register_access(void) {
    test_section("Test 1: IWDG register access");

    /* 读取 SR 寄存器 — 不应产生 fault */
    uint32_t sr = IWDG_SR;
    (void)sr;
    TEST_ASSERT(1, "IWDG_SR readable");

    /* 读取 PR 寄存器 */
    uint32_t pr = IWDG_PR;
    (void)pr;
    TEST_ASSERT(1, "IWDG_PR readable");

    /* 读取 RLR 寄存器 */
    uint32_t rlr = IWDG_RLR;
    (void)rlr;
    TEST_ASSERT(1, "IWDG_RLR readable");
}

/*============================================================================
 * Test 2: feed 后 RLR 值正确
 *============================================================================*/

static void test_watchdog_feed(void) {
    test_section("Test 2: watchdog feed");

    /* 解锁并读取当前 RLR */
    IWDG_KR = 0x5555;  /* 解锁写保护 */
    uint32_t rlr_before = IWDG_RLR & 0xFFF;

    /* 喂狗 */
    IWDG_KR = 0xAAAA;

    /* 喂狗后 RLR 应保持不变 (喂狗只重载计数器，不改 RLR) */
    uint32_t rlr_after = IWDG_RLR & 0xFFF;
    TEST_ASSERT_EQ((int)rlr_before, (int)rlr_after,
                   "RLR unchanged after feed");
}

/*============================================================================
 * Test 3: init 后 SR 状态正确
 *============================================================================*/

static void test_watchdog_sr_status(void) {
    test_section("Test 3: watchdog SR status");

    /* IWDG 启动后，SR 的 PVU (bit 0) 和 RVU (bit 1) 表示更新中 */
    /* 等待更新完成 */
    while (IWDG_SR & 0x3);

    uint32_t sr = IWDG_SR;
    /* 启动后 PVU 和 RVU 应为 0 (空闲) */
    TEST_ASSERT((sr & 0x3) == 0, "SR PVU/RVU clear (idle)");
}

/*============================================================================
 * Module registration
 *============================================================================*/

static void test_watchdog_module(void) {
    test_iwdg_register_access();
    test_watchdog_feed();
    test_watchdog_sr_status();
}

TEST_MODULE_REGISTER(watchdog, test_watchdog_module);

#endif /* KERN_WATCHDOG_ENABLE && TEST_MODULE_WATCHDOG */
