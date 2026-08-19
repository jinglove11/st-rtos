/**
 * @file test_svc_runtime.c
 * @brief Service runtime supervisor boot tests
 */

#include "test_framework.h"
#include "shell.h"

#if TEST_ENABLE && SHELL_ENABLE

static void test_runtime_selftest(void) {
    test_section("Test 1: svc runtime selftest");

    int err = shell_svc_runtime_selftest();
    TEST_ASSERT_EQ(0, err, "svc runtime tick, health, and auto modes");
}

void test_svc_runtime_module(void) {
    test_runtime_selftest();
}

TEST_K_MODULE(svc_runtime, test_svc_runtime_module);

#endif /* TEST_ENABLE && SHELL_ENABLE */
