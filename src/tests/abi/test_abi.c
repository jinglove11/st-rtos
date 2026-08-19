/**
 * @file test_abi.c
 * @brief M3-Step1: ABI 版本 + 错误码稳定性测试
 */

#include "test_framework.h"
#include "kernel.h"
#include "abi.h"
#include "user_api.h"
#include "kernel_types.h"
#include "factory.h"

/*============================================================================
 * Test 1: sys_abi_version 返回正确值
 *============================================================================*/

static void test_abi_version_value(void) {
    test_section("Test 1: ABI version value");

    uint32_t ver = sys_abi_version();
    uint32_t expected = ((uint32_t)KERN_ABI_MAJOR << 16) | KERN_ABI_MINOR;

    TEST_ASSERT_EQ((int)expected, (int)ver,
                   "ABI version matches KERN_ABI_MAJOR/MINOR");
    TEST_ASSERT_EQ(KERN_ABI_MAJOR, (int)((ver >> 16) & 0xFFFF),
                   "ABI major extracted");
    TEST_ASSERT_EQ(KERN_ABI_MINOR, (int)(ver & 0xFFFF),
                   "ABI minor extracted");
}

/*============================================================================
 * Test 2: 错误码值未变 (冻结检查)
 *============================================================================*/

static void test_error_codes_frozen(void) {
    test_section("Test 2: error codes frozen");

    /* 这些值是 ABI 契约的一部分,任何改动都会破坏用户应用。
     * 测试断言当前值,如果有人改了枚举这里会 fail。 */
    TEST_ASSERT_EQ(0,   (int)KERN_OK,           "KERN_OK = 0");
    TEST_ASSERT_EQ(-1,  (int)KERN_ERR,          "KERN_ERR = -1");
    TEST_ASSERT_EQ(-2,  (int)KERN_ERR_PARAM,    "KERN_ERR_PARAM = -2");
    TEST_ASSERT_EQ(-3,  (int)KERN_ERR_TIMEOUT,  "KERN_ERR_TIMEOUT = -3");
    TEST_ASSERT_EQ(-4,  (int)KERN_ERR_RESOURCE, "KERN_ERR_RESOURCE = -4");
    TEST_ASSERT_EQ(-5,  (int)KERN_ERR_STATE,    "KERN_ERR_STATE = -5");
    TEST_ASSERT_EQ(-6,  (int)KERN_ERR_ISR,      "KERN_ERR_ISR = -6");
    TEST_ASSERT_EQ(-7,  (int)KERN_ERR_CAP,      "KERN_ERR_CAP = -7");
    TEST_ASSERT_EQ(-8,  (int)KERN_ERR_BUSY,     "KERN_ERR_BUSY = -8");
    TEST_ASSERT_EQ(-9,  (int)KERN_ERR_NOEXIST,  "KERN_ERR_NOEXIST = -9");
    TEST_ASSERT_EQ(-10, (int)KERN_ERR_OVERFLOW, "KERN_ERR_OVERFLOW = -10");
    TEST_ASSERT_EQ(-11, (int)KERN_ERR_DEADLOCK, "KERN_ERR_DEADLOCK = -11");
    TEST_ASSERT_EQ(-12, (int)KERN_ERR_PERM,     "KERN_ERR_PERM = -12");
    TEST_ASSERT_EQ(-13, (int)KERN_ERR_NOTDIR,   "KERN_ERR_NOTDIR = -13");
    TEST_ASSERT_EQ(-14, (int)KERN_ERR_ISDIR,    "KERN_ERR_ISDIR = -14");
    TEST_ASSERT_EQ(-15, (int)KERN_ERR_FAULT,    "KERN_ERR_FAULT = -15");
    TEST_ASSERT_EQ(-16, (int)KERN_ERR_NOSYS,    "KERN_ERR_NOSYS = -16");
}

/*============================================================================
 * Test 3: abi_header_t 布局
 *============================================================================*/

static void test_abi_header_layout(void) {
    test_section("Test 3: abi_header_t layout");

    TEST_ASSERT_EQ(4, (int)sizeof(abi_header_t),
                   "abi_header_t is 4 bytes");

    abi_header_t hdr = { .version = 0, .size = 4 };
    TEST_ASSERT(ABI_CHECK(&hdr, 4), "ABI_CHECK passes for correct size");
    TEST_ASSERT(!ABI_CHECK(&hdr, 8), "ABI_CHECK rejects wrong size");
    TEST_ASSERT(!ABI_CHECK(NULL, 4), "ABI_CHECK rejects NULL");
}

/*============================================================================
 * Test 4: ABI 兼容性 — 版本不匹配拒绝 (验收 D)
 *
 * 模拟"前一版本用户应用"场景:
 * - 旧 MAJOR 版本 → 拒绝 (不兼容)
 * - 未来 MAJOR 版本 → 拒绝
 * - 旧 MINOR 版本 → 接受 (向后兼容)
 * - 未来 MINOR 版本 → 拒绝 (内核不认识)
 * - size 不匹配 → 拒绝
 *============================================================================*/

static void test_abi_compatibility(void) {
    test_section("Test 4: ABI compatibility accept/reject (D)");

    /* 当前内核版本 */
    uint32_t kern_ver = sys_abi_version();
    TEST_ASSERT_EQ(KERN_ABI_MAJOR, (int)((kern_ver >> 16) & 0xFFFF),
                   "kernel ABI major is current");

    /* 模拟旧版本结构 (version=0, size 正确) → 接受 (向后兼容) */
    abi_header_t old_hdr = { .version = 0, .size = sizeof(abi_header_t) };
    TEST_ASSERT(ABI_CHECK(&old_hdr, sizeof(abi_header_t)),
                "ABI_CHECK accepts older minor version (backward compat)");

    /* 模拟未来版本结构 (version 超过当前 MINOR) → 拒绝 */
    abi_header_t future_hdr = { .version = KERN_ABI_MINOR + 1,
                                .size = sizeof(abi_header_t) };
    TEST_ASSERT(!ABI_CHECK(&future_hdr, sizeof(abi_header_t)),
                "ABI_CHECK rejects future minor version");

    /* size 不匹配 → 拒绝 (模拟结构体大小变化) */
    abi_header_t wrong_size = { .version = 0, .size = 999 };
    TEST_ASSERT(!ABI_CHECK(&wrong_size, sizeof(abi_header_t)),
                "ABI_CHECK rejects mismatched size");

    /* version=0 + size=0 → 拒绝 (空结构) */
    abi_header_t empty_hdr = { .version = 0, .size = 0 };
    TEST_ASSERT(!ABI_CHECK(&empty_hdr, sizeof(abi_header_t)),
                "ABI_CHECK rejects zero-size structure");

    /* 模拟 factory_create_request_t 的 ABI 校验 */
    abi_header_t factory_hdr = {
        .version = KERN_ABI_MINOR,
        .size = sizeof(factory_create_request_t)
    };
    TEST_ASSERT(ABI_CHECK(&factory_hdr, sizeof(factory_create_request_t)),
                "factory_create_request_t ABI_CHECK passes");

    /* 旧版 factory (size 不对) → 拒绝 */
    abi_header_t old_factory = {
        .version = 0,
        .size = sizeof(factory_create_request_t) - 4  /* 少了 hdr */
    };
    TEST_ASSERT(!ABI_CHECK(&old_factory, sizeof(factory_create_request_t)),
                "old factory struct (wrong size) rejected");
}

/*============================================================================
 * Module registration
 *============================================================================*/

static void test_abi_module(void) {
    test_abi_version_value();
    test_error_codes_frozen();
    test_abi_header_layout();
    test_abi_compatibility();
}

TEST_ABI_MODULE(abi, test_abi_module);
