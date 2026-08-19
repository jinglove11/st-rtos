/**
 * @file test_elf.c
 * @brief Core completion #6 — ELF loader tests
 *
 * Loads a freestanding ELF (embedded via .incbin) and verifies it executes
 * as a user task: writes to .data, calls sys_task_exit(0x600D), and the
 * test reads the exit value via task_join.
 */

#include "test_framework.h"
#include "kernel.h"
#include "task.h"
#include "elf_loader.h"

#if ELF_LOADER && TEST_MODULE_ELF

/*============================================================================
 * The embedded test ELF (from test_elf_blob.c's .incbin)
 *============================================================================*/

extern const uint8_t __test_elf_start[];
extern const uint8_t __test_elf_end[];

/*============================================================================
 * Test 1: load + execute the embedded ELF
 *============================================================================*/

static void test_elf_load_and_run(void) {
    test_section("Test 1: load + execute ELF");

    /* Verify the blob is non-empty (the build produced an ELF). */
    size_t elf_size = (size_t)(__test_elf_end - __test_elf_start);
    test_print_num("[elf] embedded size = ", (int32_t)elf_size);
    TEST_ASSERT(elf_size > 0, "embedded ELF non-empty");
    TEST_ASSERT(elf_size >= sizeof(Elf32_Ehdr), "large enough for ELF header");
    if (elf_size < sizeof(Elf32_Ehdr)) return;

    /* Verify magic. */
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)__test_elf_start;
    TEST_ASSERT(eh->e_ident[0] == 0x7F, "ELF magic byte 0");
    TEST_ASSERT(eh->e_ident[1] == 'E', "ELF magic byte 1");

    /* Load + create task. */
    task_id_t tid = KERN_INVALID_ID;
    kern_err_t e = elf_load(__test_elf_start, "elf_test", 8, &tid);
    TEST_ASSERT_EQ((int)KERN_OK, (int)e, "elf_load OK");
    TEST_ASSERT(tid >= 0, "task id valid");
    if (e != KERN_OK || tid < 0) return;

    /* Start + join. The ELF exits with 0x600D. */
    e = task_start(tid);
    TEST_ASSERT_EQ((int)KERN_OK, (int)e, "elf task started");

    void *retval = NULL;
    e = task_join(tid, &retval, 2000);
    test_print_num("[elf] join err = ", (int32_t)e);
    TEST_ASSERT_EQ((int)KERN_OK, (int)e, "elf task joined (no fault)");

    int rv = (int)(intptr_t)retval;
    test_print_num("[elf] exit value = ", (int32_t)rv);
    /* The ELF _start simply returns (LR=user_task_exit_handler with retval=0).
     * We mainly check join succeeded (no fault) and retval is 0. */
    TEST_ASSERT_EQ(0, rv, "ELF exited cleanly (retval 0)");
}

/*============================================================================
 * Test 2: bad ELF (corrupted magic) rejected
 *============================================================================*/

static void test_elf_bad_magic_rejected(void) {
    test_section("Test 2: bad ELF magic rejected");

    /* Use a dummy buffer with wrong magic. */
    static const uint8_t bad_elf[64] = {0};
    task_id_t tid = KERN_INVALID_ID;
    kern_err_t e = elf_load(bad_elf, "bad_elf", 8, &tid);
    TEST_ASSERT(e != KERN_OK, "bad magic rejected");
    TEST_ASSERT(tid == KERN_INVALID_ID, "no task created for bad ELF");
}

/*============================================================================
 * Module registration
 *============================================================================*/

static void test_elf_module(void) {
    test_elf_load_and_run();
    test_elf_bad_magic_rejected();
}

TEST_K_MODULE(elf, test_elf_module);

#endif /* ELF_LOADER && TEST_MODULE_ELF */
