/**
 * @file test_usercopy.c
 * @brief Usercopy boundary helper tests
 */

#include "test_framework.h"
#include "usercopy.h"
#include "kernel_config.h"
#include <string.h>

#if SYSCALL_ENABLE && TEST_ENABLE

static void test_user_access_kernel_context(void) {
    test_section("Test 1: user_access_ok kernel context");

    char buf[8] = {0};
    const char *text = "ucopy";

    TEST_ASSERT(user_access_ok(text, 5, USER_ACCESS_READ),
                "flash/string readable in kernel context");
    TEST_ASSERT(user_access_ok(buf, sizeof(buf), USER_ACCESS_READ),
                "sram readable in kernel context");
    TEST_ASSERT(user_access_ok(buf, sizeof(buf), USER_ACCESS_WRITE),
                "sram writable in kernel context");
    TEST_ASSERT(!user_access_ok((void *)(uintptr_t)0xBBBBBBBBu, 4,
                                USER_ACCESS_READ),
                "bad address rejected");
    TEST_ASSERT(!user_access_ok(NULL, 1, USER_ACCESS_READ),
                "NULL nonzero rejected");
    TEST_ASSERT(user_access_ok(NULL, 0, USER_ACCESS_READ),
                "NULL zero length accepted");
}

static void test_copy_helpers(void) {
    test_section("Test 2: copy helpers");

    char src[8] = "abc";
    char dst[8] = {0};

    kern_err_t err = copy_from_user(dst, src, 4);
    TEST_ASSERT_EQ(KERN_OK, err, "copy_from_user OK");
    TEST_ASSERT_EQ(0, strcmp(dst, "abc"), "copy_from_user content");

    char out[8] = {0};
    err = copy_to_user(out, dst, 4);
    TEST_ASSERT_EQ(KERN_OK, err, "copy_to_user OK");
    TEST_ASSERT_EQ(0, strcmp(out, "abc"), "copy_to_user content");

    err = copy_from_user(dst, (void *)(uintptr_t)0xBBBBBBBBu, 4);
    TEST_ASSERT_EQ(KERN_ERR_PARAM, err, "copy_from_user bad pointer");
}

static void test_strncpy_from_user(void) {
    test_section("Test 3: strncpy_from_user");

    char dst[8];
    kern_err_t err = strncpy_from_user(dst, "hello", sizeof(dst));
    TEST_ASSERT_EQ(KERN_OK, err, "strncpy_from_user OK");
    TEST_ASSERT_EQ(0, strcmp(dst, "hello"), "string copied");

    err = strncpy_from_user(dst, "toolong-string", 5);
    TEST_ASSERT_EQ(KERN_ERR_PARAM, err, "unterminated bounded string rejected");
    TEST_ASSERT_EQ('\0', dst[4], "bounded string terminated");

    err = strncpy_from_user(dst, (const char *)(uintptr_t)0xBBBBBBBBu,
                            sizeof(dst));
    TEST_ASSERT_EQ(KERN_ERR_PARAM, err, "bad string pointer rejected");
}

static void test_usercopy_module(void) {
    test_user_access_kernel_context();
    test_copy_helpers();
    test_strncpy_from_user();
}

TEST_ABI_MODULE(usercopy, test_usercopy_module);

#endif /* SYSCALL_ENABLE && TEST_ENABLE */
