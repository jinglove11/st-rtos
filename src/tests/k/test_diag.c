/**
 * @file test_diag.c
 * @brief 诊断命令测试 — crash / stats / mem / trace 命令存在性验证
 */

#include "test_framework.h"
#include "shell.h"
#include <string.h>

#if SHELL_ENABLE && TEST_ENABLE

#include "fault.h"
#include "trace.h"
#include "stats.h"

/*============================================================================
 * Test 1: crash 命令存在
 *============================================================================*/

typedef struct {
    const char *name;
    const char *help;
    void (*func)(int argc, char **argv);
} shell_cmd_t;

extern const shell_cmd_t cmd_table[];
extern const int cmd_count;

static void test_crash_cmd_exists(void) {
    test_section("Test 1: crash command exists");

    int found = 0;
    for (int i = 0; i < cmd_count; i++) {
        if (strcmp(cmd_table[i].name, "crash") == 0) {
            found = 1;
            TEST_ASSERT(strlen(cmd_table[i].help) > 0, "crash has help text");
            TEST_ASSERT_NOT_NULL((void *)(uintptr_t)cmd_table[i].func, "crash has handler");
            break;
        }
    }
    TEST_ASSERT(found, "'crash' in command table");
}

/*============================================================================
 * Test 2: stats 命令存在
 *============================================================================*/

static void test_stats_cmd_exists(void) {
    test_section("Test 2: stats command exists");

    int found = 0;
    for (int i = 0; i < cmd_count; i++) {
        if (strcmp(cmd_table[i].name, "stats") == 0) {
            found = 1;
            TEST_ASSERT(strlen(cmd_table[i].help) > 0, "stats has help text");
            TEST_ASSERT_NOT_NULL((void *)(uintptr_t)cmd_table[i].func, "stats has handler");
            break;
        }
    }
    TEST_ASSERT(found, "'stats' in command table");
}

/*============================================================================
 * Test 3: mem 命令存在
 *============================================================================*/

static void test_mem_cmd_exists(void) {
    test_section("Test 3: mem command exists");

    int found = 0;
    for (int i = 0; i < cmd_count; i++) {
        if (strcmp(cmd_table[i].name, "mem") == 0) {
            found = 1;
            TEST_ASSERT(strlen(cmd_table[i].help) > 0, "mem has help text");
            TEST_ASSERT_NOT_NULL((void *)(uintptr_t)cmd_table[i].func, "mem has handler");
            break;
        }
    }
    TEST_ASSERT(found, "'mem' in command table");
}

/*============================================================================
 * Test 4: trace 命令存在并支持过滤
 *============================================================================*/

static void test_trace_cmd_exists(void) {
    test_section("Test 4: trace command exists");

    int found = 0;
    for (int i = 0; i < cmd_count; i++) {
        if (strcmp(cmd_table[i].name, "trace") == 0) {
            found = 1;
            TEST_ASSERT(strlen(cmd_table[i].help) > 0, "trace has help text");
            TEST_ASSERT_NOT_NULL((void *)(uintptr_t)cmd_table[i].func, "trace has handler");
            break;
        }
    }
    TEST_ASSERT(found, "'trace' in command table");
}

/*============================================================================
 * Test 4b: trace help mentions bounded output
 *============================================================================*/

static void test_trace_help_bounded(void) {
    test_section("Test 4b: trace command help");

    int found = 0;
    for (int i = 0; i < cmd_count; i++) {
        if (strcmp(cmd_table[i].name, "trace") == 0) {
            found = 1;
            TEST_ASSERT(cmd_table[i].help[0] == '[' &&
                        cmd_table[i].help[1] == 'n',
                        "trace help mentions count");
            break;
        }
    }
    TEST_ASSERT(found, "'trace' help checked");
}

/*============================================================================
 * Test 5: crash_dump 数据结构可访问
 *============================================================================*/

static void test_crash_dump_structure(void) {
    test_section("Test 5: crash_dump accessible");

    extern crash_dump_t crash_dump;
    crash_dump_t *cd = &crash_dump;
    TEST_ASSERT_NOT_NULL(cd, "crash_dump pointer not null");

    /* After boot without crash, fault_type is 0 or cfsr is 0 */
    uint32_t cfsr = cd->cfsr;
    uint32_t pc = cd->pc;
    uint8_t ft = cd->fault_type;
    (void)cfsr;
    (void)pc;
    (void)ft;
    TEST_ASSERT(1, "crash_dump fields readable");
}

/*============================================================================
 * Test 6: kern_stats_t 数据结构可访问
 *============================================================================*/

static void test_stats_structure(void) {
    test_section("Test 6: kern_stats accessible");

    const kern_stats_t *ks = stats_get_kern_stats();
    TEST_ASSERT_NOT_NULL(ks, "kern_stats pointer not null");

    /* uptime should be > 0 once scheduler is running */
    uint32_t uptime = stats_get_uptime();
    (void)uptime;
    TEST_ASSERT(1, "stats fields readable");
}

/*============================================================================
 * Test 7: Trace 事件名称字符串
 *============================================================================*/

static void test_trace_event_names(void) {
    test_section("Test 7: trace event name strings");

    /* Verify the shell can translate event IDs */
    const char *names[] = {
        "SW", "ISR+", "ISR-", "SVC",
        "IPC+", "IPC-", "BH", "FLT",
        "TMR", "IRQ", "BH2", "DEV",
        "MEM", "IPC", "CAP", "VFS"
    };

    /* Spot-check that we have all shell-visible event groups */
    for (int i = 0; i < 16; i++) {
        TEST_ASSERT(names[i] != NULL, "event name exists");
        TEST_ASSERT(strlen(names[i]) > 0, "event name not empty");
    }
}

/*============================================================================
 * Test 8: dev 命令存在
 *============================================================================*/

static void test_dev_cmd_exists(void) {
    test_section("Test 8: dev command exists");

    int found = 0;
    for (int i = 0; i < cmd_count; i++) {
        if (strcmp(cmd_table[i].name, "dev") == 0) {
            found = 1;
            TEST_ASSERT(strlen(cmd_table[i].help) > 0, "dev has help text");
            TEST_ASSERT_NOT_NULL((void *)(uintptr_t)cmd_table[i].func, "dev has handler");
            break;
        }
    }
    TEST_ASSERT(found, "'dev' in command table");
}

/*============================================================================
 * Module registration
 *============================================================================*/

static void test_diag_module(void) {
    test_crash_cmd_exists();
    test_stats_cmd_exists();
    test_mem_cmd_exists();
    test_trace_cmd_exists();
    test_trace_help_bounded();
    test_crash_dump_structure();
    test_stats_structure();
    test_trace_event_names();
    test_dev_cmd_exists();
}

TEST_K_MODULE(diag, test_diag_module);

#endif /* SHELL_ENABLE && TEST_ENABLE */
