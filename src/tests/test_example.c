/**
 * @file test_example.c
 * @brief 示例测试模块
 *
 * 演示如何创建新的测试模块。
 * 复制此文件作为模板，修改测试内容。
 */

#include "test_framework.h"

/*============================================================================
 * 示例测试用例
 *============================================================================*/

/**
 * @brief 示例测试 1
 *
 * 测试描述：演示基本断言使用
 */
static void test_example_basic(void) {
    test_section("Example: Basic Assertions");

    /* 使用断言宏 */
    TEST_ASSERT(1 == 1, "Basic assertion works");
    TEST_ASSERT_EQ(5, 5, "Equality assertion works");
    TEST_ASSERT_NE(3, 4, "Inequality assertion works");
    TEST_ASSERT_RANGE(5, 1, 10, "Range assertion works");

    /* 打印调试信息 */
    test_print_num("  Example value: ", 42);
    test_print_hex("  Example hex: ", 0xDEADBEEF);
}

/**
 * @brief 示例测试 2
 *
 * 测试描述：演示条件测试
 */
static void test_example_conditions(void) {
    test_section("Example: Condition Tests");

    int value = 100;

    TEST_ASSERT(value > 0, "Value is positive");
    TEST_ASSERT(value < 1000, "Value is less than 1000");
    TEST_ASSERT_EQ(100, value, "Value equals 100");

    /* 测试指针 */
    int *ptr = &value;
    TEST_ASSERT_NOT_NULL(ptr, "Pointer is not null");
    TEST_ASSERT_NULL(NULL, "NULL pointer check");
}

/*============================================================================
 * 示例测试模块入口
 *============================================================================*/

/**
 * @brief 示例测试模块主函数
 *
 * 执行所有示例测试。
 */
static void test_example_module(void) {
    test_example_basic();
    test_example_conditions();
}

/*============================================================================
 * 模块注册
 *============================================================================*/

/* 注册示例测试模块（取消注释以启用） */
/* TEST_MODULE_REGISTER(example, test_example_module); */
