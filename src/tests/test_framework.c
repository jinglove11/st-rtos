/**
 * @file test_framework.c
 * @brief 模块化测试框架 — 失败当场打印 + 末尾汇总
 */

#include "test_framework.h"
#include "uart.h"
#include "board_config.h"
#include "kernel.h"
#include "task.h"
#include "gpio.h"
#include "system_init.h"
#include "shell.h"
#include <string.h>

#define TEST_UART NUCLEO_DEFAULT_UART
#define TEST_LED_PORT NUCLEO_LED_PORT
#define TEST_LED_PIN NUCLEO_LED_PIN

extern const test_module_t __test_modules_start;
extern const test_module_t __test_modules_end;

/*============================================================================
 * 状态
 *============================================================================*/

static volatile int test_passed = 0;
static volatile int test_failed = 0;
static const char *current_module  = "";

/*============================================================================
 * 计数查询
 *============================================================================*/

int test_get_passed(void) { return test_passed; }
int test_get_failed(void) { return test_failed; }

void test_reset_counts(void) {
    test_passed = 0;
    test_failed = 0;
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
        module->func();
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

#if SHELL_ENABLE
    shell_start();
#endif

    while (1) {
        gpio_toggle(TEST_LED_PORT, TEST_LED_PIN);
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
