/**
 * @file test_framework.c
 * @brief 模块化测试框架 — 失败当场打印 + 末尾汇总
 */

#include "test_framework.h"
#include "uart.h"
#include "board_config.h"
#include "kernel.h"
#include "task.h"
#include "mem.h"
#include "capability.h"
#include "gpio.h"
#include "system_init.h"
#include "stats.h"
#include "shell.h"
#include "board.h"
#include <string.h>
#if INIT_PROCESS && CAP_ENABLE
#include "root_bootstrap.h"
#endif

#define TEST_UART BOARD_DEFAULT_UART

extern const test_module_t __test_modules_start;
extern const test_module_t __test_modules_end;

/*============================================================================
 * 状态
 *============================================================================*/

static volatile int test_passed = 0;
static volatile int test_failed = 0;
static const char *current_module  = "";

typedef struct {
    int passed;
    int failed;
    uint16_t cap_free;
    uint32_t mem_outstanding;
    uint32_t faults;
} test_resource_snapshot_t;

/*============================================================================
 * 计数查询
 *============================================================================*/

int test_get_passed(void) { return test_passed; }
int test_get_failed(void) { return test_failed; }

void test_reset_counts(void) {
    test_passed = 0;
    test_failed = 0;
}

void test_skip(const char *name) {
    test_print("  [SKIP] ");
    test_print(name);
    test_print("\r\n");
}

/*============================================================================
 * 基础输出
 *============================================================================*/

void test_print(const char *msg) {
    uart_puts(TEST_UART, msg);
}

void test_print_num(const char *label, int32_t num) {
    uart_puts(TEST_UART, label);
    if (num < 0) { uart_puts(TEST_UART, "-"); num = -num; }
    uart_putdec(TEST_UART, (uint32_t)num);
    uart_puts(TEST_UART, "\r\n");
}

void test_print_hex(const char *label, uint32_t num) {
    uart_puts(TEST_UART, label);
    const char hex[] = "0123456789ABCDEF";
    char buf[9];
    buf[8] = '\0';
    for (int i = 7; i >= 0; i--) { buf[i] = hex[num & 0xF]; num >>= 4; }
    uart_puts(TEST_UART, buf);
    uart_puts(TEST_UART, "\r\n");
}

static test_resource_snapshot_t test_resource_snapshot(void) {
    test_resource_snapshot_t snap;
    snap.passed = test_passed;
    snap.failed = test_failed;
#if CAP_ENABLE
    snap.cap_free = cap_free_count();
#else
    snap.cap_free = 0;
#endif
#if MEM_DYNAMIC
    snap.mem_outstanding = mem_get_outstanding_allocs();
#else
    snap.mem_outstanding = 0;
#endif
#if KERN_TASK_STATS
    snap.faults = stats_get_kern_stats()->fault_count;
#else
    snap.faults = 0;
#endif
    return snap;
}

static void test_print_module_result(const test_module_t *module,
                                     const test_resource_snapshot_t *before,
                                     const test_resource_snapshot_t *after) {
    int pass_delta = after->passed - before->passed;
    int fail_delta = after->failed - before->failed;

    test_print("[MODULE] ");
    test_print(module->name);
    test_print(" pass +");
    uart_putdec(TEST_UART, (uint32_t)pass_delta);
    test_print(" fail +");
    uart_putdec(TEST_UART, (uint32_t)fail_delta);

#if CAP_ENABLE
    test_print(" cap ");
    uart_putdec(TEST_UART, (uint32_t)before->cap_free);
    test_print("->");
    uart_putdec(TEST_UART, (uint32_t)after->cap_free);
#endif

#if MEM_DYNAMIC
    test_print(" mem ");
    uart_putdec(TEST_UART, before->mem_outstanding);
    test_print("->");
    uart_putdec(TEST_UART, after->mem_outstanding);
#endif

#if KERN_TASK_STATS
    test_print(" faults ");
    uart_putdec(TEST_UART, before->faults);
    test_print("->");
    uart_putdec(TEST_UART, after->faults);
#endif

    test_print("\r\n");

#if CAP_ENABLE
    if (after->cap_free < before->cap_free) {
        test_print("[RESOURCE] ");
        test_print(module->name);
        test_print(": cap free decreased by ");
        uart_putdec(TEST_UART, (uint32_t)(before->cap_free - after->cap_free));
        test_print("\r\n");
    }
    if (after->cap_free < 8U) {
        test_print("[RESOURCE] ");
        test_print(module->name);
        test_print(": cap pool low, free=");
        uart_putdec(TEST_UART, (uint32_t)after->cap_free);
        test_print("\r\n");
    }
#endif

#if MEM_DYNAMIC
    if (after->mem_outstanding > before->mem_outstanding) {
        test_print("[RESOURCE] ");
        test_print(module->name);
        test_print(": mem outstanding increased by ");
        uart_putdec(TEST_UART, after->mem_outstanding - before->mem_outstanding);
        test_print("\r\n");
    }
#endif
}

/*============================================================================
 * 断言记录
 *============================================================================*/

void test_section(const char *name) {
    (void)name;
}

void test_pass(const char *name) {
    (void)name;
    test_passed++;
}

void test_fail(const char *name) {
    test_failed++;
    test_print("[FAIL] ");
    test_print(current_module);
    test_print(": ");
    test_print(name);
    test_print("\r\n");
}

static void test_print_i32_inline(int32_t value) {
    if (value < 0) {
        test_print("-");
        uart_putdec(TEST_UART, (uint32_t)(-(int64_t)value));
    } else {
        uart_putdec(TEST_UART, (uint32_t)value);
    }
}

void test_fail_eq(const char *name, int32_t expected, int32_t actual) {
    test_failed++;
    test_print("[FAIL] ");
    test_print(current_module);
    test_print(": ");
    test_print(name);
    test_print(" (expected ");
    test_print_i32_inline(expected);
    test_print(", actual ");
    test_print_i32_inline(actual);
    test_print(")\r\n");
}

/*============================================================================
 * 汇总
 *============================================================================*/

void test_summary(void) {
    int total = test_passed + test_failed;

    test_print("\r\n========================================\r\n");
    test_print("         TEST SUMMARY\r\n");
    test_print("========================================\r\n");
    test_print_num("Passed: ", test_passed);
    test_print_num("Failed: ", test_failed);
    test_print_num("Total:  ", total);
    test_print("========================================\r\n");

    if (test_failed == 0) {
        test_print("All tests PASSED!\r\n");
    } else {
        test_print_num("FAILED: ", test_failed);
    }
}

/*============================================================================
 * 模块管理
 *============================================================================*/

int test_get_module_count(void) {
    size_t size = (size_t)&__test_modules_end - (size_t)&__test_modules_start;
    return (int)(size / sizeof(test_module_t));
}

void test_run_all_modules(void) {
    const test_module_t *module;
    int module_count = 0;

    test_print("\r\nMy-RTOS Test Suite v2.0\r\n");

    for (module = &__test_modules_start;
         module < &__test_modules_end;
         module++) {

        if (module->name == NULL || module->func == NULL) continue;

        module_count++;
        current_module = module->name;
        test_resource_snapshot_t before = test_resource_snapshot();
        module->func();
        test_resource_snapshot_t after = test_resource_snapshot();
        test_print_module_result(module, &before, &after);
    }

    test_print_num("\r\nModules: ", module_count);
    test_summary();
}

/*============================================================================
 * 测试运行器
 *============================================================================*/

static void test_runner_task(void *arg) {
    (void)arg;
    test_run_all_modules();

#if INIT_PROCESS && CAP_ENABLE
    /* Phase 2 §2.3: after the suite passes (so init/supervisor never disturb
     * tests), launch the user-mode init process via the root bootstrap path.
     * init registers restart recipes, spawns the supervisor, and exits. The
     * shell still starts separately below — it is a privileged kernel task
     * for now and cannot be a child of a user-mode init. */
    {
        extern void init_main(void *arg);
        task_id_t init_tid = KERN_INVALID_ID;
        kern_err_t err = root_bootstrap_create("init", init_main, NULL,
                                               3, 1024, &init_tid);
        if (err == KERN_OK && init_tid >= 0) {
            (void)root_bootstrap_start();
        }
    }
#endif

#if SHELL_ENABLE
    shell_start();
#endif

    while (1) {
        board_status_led_toggle();
        task_delay(500);
    }
}

void test_runner_start(void) {
    system_init(NULL);
    test_print("\r\nTest Framework Starting...\r\n");

    task_id_t runner = task_create("test_runner", test_runner_task, NULL, 10, 0);
    task_start(runner);

    test_print("Starting scheduler...\r\n");
    kern_start();
    while (1);
}
