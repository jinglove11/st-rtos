/**
 * @file test_framework.h
 * @brief 模块化测试框架接口 — 三层契约分层
 *
 * 测试按契约稳定性分三层:
 *   K   — 内核不变量(白盒):验证内核内部实现,允许随实现演化。
 *         改内核后 K 层红 = 预期重构成本。
 *   ABI — syscall 契约(黑盒):从用户任务发 SVC 验证对外行为,冻结契约。
 *         ABI 层红 = 真回归,必须查。
 *   SYS — 启动健康:init/supervisor 接线断言,在 init 启动后运行。
 *         SYS 层红 = 内核与用户态各自都对、接缝断了。
 *
 * 层间执行顺序固定为 K → ABI → SYS,与构建系统链接顺序无关。
 *
 * 使用方法：
 * 1. 在各模块文件中定义测试函数
 * 2. 用 TEST_K_MODULE / TEST_ABI_MODULE / TEST_SYS_MODULE 注册
 *    (TEST_MODULE_REGISTER 为兼容别名,等同 TEST_K_MODULE)
 * 3. main.c 中调用 test_runner_start() 启动测试
 */

#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdint.h>
#include "kobject.h"

/*============================================================================
 * cap 假对象(P0-1/B3)
 *
 * gen-0 豁免移除后,凡被 cap 系统引用的对象必须嵌入 kobject_header_t。
 * 测试用假对象统一用本类型;val 供标记值/身份断言;&fake 与 &fake.hdr
 * 同址,旧调用点传 &fake 不需要改。
 *============================================================================*/

typedef struct {
    kobject_header_t hdr;
    uint32_t val;
} test_cap_obj_t;

#define TEST_CAP_OBJ_INIT(p, type) kobj_header_init(&(p)->hdr, (type))

/*============================================================================
 * 测试层
 *============================================================================*/

#define TEST_TIER_K     0
#define TEST_TIER_ABI   1
#define TEST_TIER_SYS   2
#define TEST_TIER_COUNT 3

const char *test_tier_name(uint8_t tier);

typedef void (*test_module_func_t)(void);

typedef struct test_module {
    const char          *name;
    test_module_func_t  func;
    uint8_t             tier;
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
        if (_test_expected == _test_actual) { test_pass(name); } \
        else { test_fail_eq(name, _test_expected, _test_actual); } \
    } while (0)

#define TEST_ASSERT_NE(not_expected, actual, name) \
    TEST_ASSERT((not_expected) != (actual), name)

#define TEST_ASSERT_RANGE(value, min, max, name) \
    TEST_ASSERT((value) >= (min) && (value) <= (max), name)

#define TEST_ASSERT_NOT_NULL(ptr, name) \
    TEST_ASSERT((ptr) != NULL, name)

#define TEST_ASSERT_NULL(ptr, name) \
    TEST_ASSERT((ptr) == NULL, name)

/*============================================================================
 * ABI 层用户任务用例助手
 *
 * 模式(样板源自 test_syscall.c 用户用例):
 *   1. 用户子任务入口内调用 sys_* 系列,聚合结果
 *   2. 用 sys_task_exit((void *)(intptr_t)result) 回传
 *   3. 内核侧本助手 create + join + 断言
 *
 * 用户任务写不了内核计数器(内核 SRAM 不可达),所以断言必须回到内核
 * 上下文做——单结果用 test_user_case,多断言自行 task_join 后逐条
 * TEST_ASSERT_EQ。
 *============================================================================*/

void test_user_case(const char *name, void (*user_entry)(void *),
                    int expected_retval, uint32_t timeout_ms);

/*============================================================================
 * 模块注册
 *============================================================================*/

#define TEST_K_MODULE(_name, _func) \
    __attribute__((used, section(".test_modules"))) \
    static const test_module_t __test_module_##_name = { \
        .name = #_name, .func = _func, .tier = TEST_TIER_K \
    }

#define TEST_ABI_MODULE(_name, _func) \
    __attribute__((used, section(".test_modules"))) \
    static const test_module_t __test_module_##_name = { \
        .name = #_name, .func = _func, .tier = TEST_TIER_ABI \
    }

#define TEST_SYS_MODULE(_name, _func) \
    __attribute__((used, section(".test_modules"))) \
    static const test_module_t __test_module_##_name = { \
        .name = #_name, .func = _func, .tier = TEST_TIER_SYS \
    }

/* 兼容别名(迁移期):未分层的模块按 K 处理 */
#define TEST_MODULE_REGISTER(_name, _func) TEST_K_MODULE(_name, _func)

/* 按层执行:跑指定 tier 的全部模块,返回该层新增 fail 数 */
int  test_run_tier(uint8_t tier);

/* 按名重跑(shell test 命令用):找到返回该模块 fail 数,未找到返回 -1 */
int  test_run_module_by_name(const char *name);

/* K + ABI 全量(SYS 层由 runner 在 init 启动后单独触发) */
void test_run_all_modules(void);
void test_runner_start(void);
int  test_get_module_count(void);

#endif /* TEST_FRAMEWORK_H */
