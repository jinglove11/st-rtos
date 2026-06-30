/**
 * @file test_framework.h
 * @brief 模块化测试框架接口
 *
 * 使用方法：
 * 1. 在各模块文件中定义测试函数
 * 2. 使用 TEST_MODULE_REGISTER() 注册测试模块
 * 3. main.c 中调用 test_runner_start() 启动测试
 */

#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdint.h>

/* 最大注册模块数量 */
#define TEST_MAX_MODULES        20

typedef void (*test_module_func_t)(void);

typedef struct test_module {
    const char          *name;
    test_module_func_t  func;
} test_module_t;

/* 测试结果统计 */
int  test_get_passed(void);
int  test_get_failed(void);
void test_reset_counts(void);

/* 测试输出 */
void test_print(const char *msg);
void test_print_num(const char *label, int32_t num);
void test_print_hex(const char *label, uint32_t num);
void test_pass(const char *name);
void test_fail(const char *name);
void test_fail_eq(const char *name, int32_t expected, int32_t actual);
void test_skip(const char *name);
void test_section(const char *name);
void test_summary(void);

/* 断言宏 */
#define TEST_ASSERT(condition, name) \
    do { \
        if (condition) { test_pass(name); } \
        else { test_fail(name); } \
    } while (0)

#define TEST_ASSERT_EQ(expected, actual, name) \
    do { \
        int32_t _test_expected = (int32_t)(expected); \
        int32_t _test_actual = (int32_t)(actual); \
        if (_test_expected == _test_actual) { \
            test_pass(name); \
        } else { \
            test_fail_eq(name, _test_expected, _test_actual); \
        } \
    } while (0)

#define TEST_ASSERT_NE(not_expected, actual, name) \
    TEST_ASSERT((not_expected) != (actual), name)

#define TEST_ASSERT_RANGE(value, min, max, name) \
    TEST_ASSERT((value) >= (min) && (value) <= (max), name)

#define TEST_ASSERT_NOT_NULL(ptr, name) \
    TEST_ASSERT((ptr) != NULL, name)

#define TEST_ASSERT_NULL(ptr, name) \
    TEST_ASSERT((ptr) == NULL, name)

/* 模块注册 */
#define TEST_MODULE_REGISTER(_name, _func) \
    __attribute__((used, section(".test_modules"))) \
    static const test_module_t __test_module_##_name = { \
        .name = #_name, \
        .func = _func \
    }

void test_run_all_modules(void);
void test_runner_start(void);
int  test_get_module_count(void);

#endif /* TEST_FRAMEWORK_H */
