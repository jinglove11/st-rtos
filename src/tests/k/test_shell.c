/**
 * @file test_shell.c
 * @brief Shell 交互终端测试
 */

#include "test_framework.h"
#include "shell.h"
#include <string.h>

#if SHELL_ENABLE && TEST_ENABLE

#define SHELL_LINE_MAX  128
#define SHELL_ARGV_MAX  8

/*============================================================================
 * Test 1: shell_split 空输入
 *============================================================================*/

static void test_split_empty(void) {
    test_section("Test 1: shell_split empty input");

    char buf[SHELL_LINE_MAX];
    char *argv[SHELL_ARGV_MAX];

    /* 空字符串 */
    strcpy(buf, "");
    int argc = shell_split(buf, argv, SHELL_ARGV_MAX);
    TEST_ASSERT_EQ(0, argc, "empty → argc=0");

    /* 纯空格 */
    strcpy(buf, "   ");
    argc = shell_split(buf, argv, SHELL_ARGV_MAX);
    TEST_ASSERT_EQ(0, argc, "spaces → argc=0");
}

/*============================================================================
 * Test 2: shell_split 单词
 *============================================================================*/

static void test_split_single(void) {
    test_section("Test 2: shell_split single word");

    char buf[SHELL_LINE_MAX];
    char *argv[SHELL_ARGV_MAX];

    strcpy(buf, "help");
    int argc = shell_split(buf, argv, SHELL_ARGV_MAX);
    TEST_ASSERT_EQ(1, argc, "single word → argc=1");
    TEST_ASSERT_EQ(0, strcmp(argv[0], "help"), "argv[0] = 'help'");
}

/*============================================================================
 * Test 3: shell_split 多参数
 *============================================================================*/

static void test_split_multi(void) {
    test_section("Test 3: shell_split multiple args");

    char buf[SHELL_LINE_MAX];
    char *argv[SHELL_ARGV_MAX];

    strcpy(buf, "ls /tmp");
    int argc = shell_split(buf, argv, SHELL_ARGV_MAX);
    TEST_ASSERT_EQ(2, argc, "ls /tmp → argc=2");
    TEST_ASSERT_EQ(0, strcmp(argv[0], "ls"), "argv[0] = 'ls'");
    TEST_ASSERT_EQ(0, strcmp(argv[1], "/tmp"), "argv[1] = '/tmp'");
}

/*============================================================================
 * Test 4: shell_split 多余空格
 *============================================================================*/

static void test_split_whitespace(void) {
    test_section("Test 4: shell_split extra whitespace");

    char buf[SHELL_LINE_MAX];
    char *argv[SHELL_ARGV_MAX];

    strcpy(buf, "  echo   hello   world  ");
    int argc = shell_split(buf, argv, SHELL_ARGV_MAX);
    TEST_ASSERT_EQ(3, argc, "3 args after trimming");
    TEST_ASSERT_EQ(0, strcmp(argv[0], "echo"), "argv[0] = 'echo'");
    TEST_ASSERT_EQ(0, strcmp(argv[1], "hello"), "argv[1] = 'hello'");
    TEST_ASSERT_EQ(0, strcmp(argv[2], "world"), "argv[2] = 'world'");
}

/*============================================================================
 * Test 5: shell_split 参数上限
 *============================================================================*/

static void test_split_max_args(void) {
    test_section("Test 5: shell_split arg limit");

    char buf[SHELL_LINE_MAX];
    char *argv[4];  /* max=4 */

    strcpy(buf, "a b c d e f");
    int argc = shell_split(buf, argv, 4);
    TEST_ASSERT_EQ(4, argc, "capped at max=4");
    TEST_ASSERT_EQ(0, strcmp(argv[0], "a"), "argv[0] = 'a'");
    TEST_ASSERT_EQ(0, strcmp(argv[3], "d"), "argv[3] = 'd'");
}

/*============================================================================
 * Test 6: 命令表查找 — help 存在
 *============================================================================*/

/* 外部引用 shell 的命令表 */
typedef struct {
    const char *name;
    const char *help;
    void (*func)(int argc, char **argv);
} shell_cmd_t;

extern const shell_cmd_t cmd_table[];
extern const int cmd_count;

static void test_cmd_help_exists(void) {
    test_section("Test 6: command table — help exists");

    int found = 0;
    for (int i = 0; i < cmd_count; i++) {
        if (strcmp(cmd_table[i].name, "help") == 0) {
            found = 1;
            TEST_ASSERT_NOT_NULL(cmd_table[i].help, "help has description");
            TEST_ASSERT_NOT_NULL((void *)(uintptr_t)cmd_table[i].func, "help has handler");
            break;
        }
    }
    TEST_ASSERT(found, "'help' command found in table");
}

/*============================================================================
 * Test 7: 命令表查找 — 所有命令都有 name 和 handler
 *============================================================================*/

static void test_cmd_table_complete(void) {
    test_section("Test 7: command table — all entries valid");

    TEST_ASSERT(cmd_count > 0, "command table not empty");

    for (int i = 0; i < cmd_count; i++) {
        TEST_ASSERT_NOT_NULL(cmd_table[i].name, "name not NULL");
        TEST_ASSERT(strlen(cmd_table[i].name) > 0, "name not empty");
        TEST_ASSERT_NOT_NULL(cmd_table[i].help, "help not NULL");
        TEST_ASSERT_NOT_NULL((void *)(uintptr_t)cmd_table[i].func, "func not NULL");
    }
}

/*============================================================================
 * Test 8: 命令表查找 — 预期命令存在
 *============================================================================*/

static void test_expected_commands(void) {
    test_section("Test 8: expected commands present");

    const char *expected[] = {"help", "ls", "cat", "echo", "clear", "ps",
                              "free", "hexdump", "dev", "driver", "fs"};
    int exp_count = (int)(sizeof(expected) / sizeof(expected[0]));

    for (int e = 0; e < exp_count; e++) {
        int found = 0;
        for (int i = 0; i < cmd_count; i++) {
            if (strcmp(cmd_table[i].name, expected[e]) == 0) {
                found = 1;
                break;
            }
        }
        TEST_ASSERT(found, expected[e]);
    }
}

/*============================================================================
 * Module registration
 *============================================================================*/

static void test_shell_module(void) {
    test_split_empty();
    test_split_single();
    test_split_multi();
    test_split_whitespace();
    test_split_max_args();
    test_cmd_help_exists();
    test_cmd_table_complete();
    test_expected_commands();
}

TEST_K_MODULE(shell, test_shell_module);

#endif /* SHELL_ENABLE && TEST_ENABLE */
