/**
 * @file test_mmio.c
 * @brief Core completion #2 — MMIO mapping tests
 *
 * Validates that kmmio_map_to_task maps a CAP_OBJ_MMIO region into a task's
 * MPU as device memory (ATTR_DEVICE), and kmmio_unmap clears it. This is the
 * mechanism a user-mode driver needs to touch peripheral registers.
 *
 * Tests run in kernel-privileged context and use a kernel (privileged) scratch
 * task: cap_owner_allowed lets privileged tasks resolve any cap without a
 * cspace entry, so kmmio_create_cap (owner=NULL, global cap) + map to the
 * privileged task works. The full user-mode access path (cap in user cspace +
 * the task actually reading the register) is exercised by the GPIO driver
 * server (slice #3).
 */

#include "test_framework.h"
#include "kernel.h"
#include "task.h"
#include "mem.h"
#include "capability.h"
#include "mpu.h"

#if MPU_ENABLE && CAP_ENABLE && TEST_MODULE_MMIO

#include <string.h>

/* A scratch task to map MMIO into (privileged, so cap resolution is allowed). */
static void mmio_scratch_task(void *arg) {
    (void)arg;
    while (1) {
        task_delay(1000);
    }
}

/* A safe, in-range peripheral address for RP2350 (0x40000000 window). IO_BANK0
 * area; we only verify the mapping is programmed, never write. 4 KiB is
 * MPU-compliant (power of two, >= 32 bytes). */
#define TEST_MMIO_BASE  0x40028000UL
#define TEST_MMIO_SIZE  0x1000UL

static int count_enabled_regions(tcb_t *tcb) {
    int n = 0;
    for (int r = 3; r < 8; r++) {
        if (tcb->mpu_regions[r][1] & RASR_ENABLE) n++;
    }
    return n;
}

/*============================================================================
 * Test 1: map an MMIO cap — region programmed, mapped base correct
 *============================================================================*/

static void test_mmio_map_programs_region(void) {
    test_section("Test 1: mmio map programs MPU region");

    task_id_t tid = task_create("mmio_t1", mmio_scratch_task, NULL, 10, 1024);
    TEST_ASSERT(tid >= 0, "scratch task created");
    if (tid < 0) return;
    tcb_t *tcb = task_get_tcb(tid);
    if (tcb == NULL) { task_delete(tid); return; }

    cap_id_t cap = KERN_INVALID_ID;
    kern_err_t e = kmmio_create_cap(TEST_MMIO_BASE, TEST_MMIO_SIZE, 4,
                                    CAP_READ | CAP_WRITE, &cap);
    TEST_ASSERT_EQ((int)KERN_OK, (int)e, "kmmio_create_cap OK");
    TEST_ASSERT(cap >= 0, "cap id valid");
    if (cap < 0) { task_delete(tid); return; }

    TEST_ASSERT_EQ(0, count_enabled_regions(tcb), "no region enabled before map");

    void *mapped = NULL;
    e = kmmio_map_to_task(tcb, cap, CAP_READ | CAP_WRITE, &mapped);
    TEST_ASSERT_EQ((int)KERN_OK, (int)e, "kmmio_map_to_task OK");
    TEST_ASSERT_EQ((int)TEST_MMIO_BASE, (int)(uintptr_t)mapped,
                   "mapped base == MMIO base");
    TEST_ASSERT_EQ(1, count_enabled_regions(tcb),
                   "exactly one region enabled after map");

    (void)kmmio_unmap_from_task(tcb, cap);
    (void)cap_delete(cap);
    (void)task_delete(tid);
}

/*============================================================================
 * Test 2: unmap clears the region
 *============================================================================*/

static void test_mmio_unmap_clears_region(void) {
    test_section("Test 2: mmio unmap clears region");

    task_id_t tid = task_create("mmio_t2", mmio_scratch_task, NULL, 10, 1024);
    TEST_ASSERT(tid >= 0, "scratch task created");
    if (tid < 0) return;
    tcb_t *tcb = task_get_tcb(tid);
    if (tcb == NULL) { task_delete(tid); return; }

    cap_id_t cap = KERN_INVALID_ID;
    (void)kmmio_create_cap(TEST_MMIO_BASE, TEST_MMIO_SIZE, 4,
                           CAP_READ | CAP_WRITE, &cap);
    TEST_ASSERT(cap >= 0, "cap created");
    if (cap < 0) { task_delete(tid); return; }

    void *mapped = NULL;
    (void)kmmio_map_to_task(tcb, cap, CAP_READ | CAP_WRITE, &mapped);
    TEST_ASSERT_EQ(1, count_enabled_regions(tcb), "region enabled after map");

    kern_err_t e = kmmio_unmap_from_task(tcb, cap);
    TEST_ASSERT_EQ((int)KERN_OK, (int)e, "unmap OK");
    TEST_ASSERT_EQ(0, count_enabled_regions(tcb), "no region after unmap");

    (void)cap_delete(cap);
    (void)task_delete(tid);
}

/*============================================================================
 * Test 3: wrong rights / bad cap rejected
 *============================================================================*/

static void test_mmio_rights_and_bad_cap(void) {
    test_section("Test 3: wrong rights / bad cap rejected");

    task_id_t tid = task_create("mmio_t3", mmio_scratch_task, NULL, 10, 1024);
    TEST_ASSERT(tid >= 0, "scratch task created");
    if (tid < 0) return;
    tcb_t *tcb = task_get_tcb(tid);
    if (tcb == NULL) { task_delete(tid); return; }

    /* READ-only cap, request WRITE → rejected. */
    cap_id_t cap = KERN_INVALID_ID;
    (void)kmmio_create_cap(TEST_MMIO_BASE, TEST_MMIO_SIZE, 4, CAP_READ, &cap);
    TEST_ASSERT(cap >= 0, "read-only cap created");
    if (cap < 0) { task_delete(tid); return; }

    void *mapped = NULL;
    kern_err_t e = kmmio_map_to_task(tcb, cap, CAP_READ | CAP_WRITE, &mapped);
    TEST_ASSERT(e != KERN_OK, "write on read-only mmio rejected");

    e = kmmio_map_to_task(tcb, cap, CAP_READ, &mapped);
    TEST_ASSERT_EQ((int)KERN_OK, (int)e, "read-only map OK");
    (void)kmmio_unmap_from_task(tcb, cap);

    /* Invalid rights value rejected. */
    e = kmmio_map_to_task(tcb, cap, 0, &mapped);
    TEST_ASSERT(e != KERN_OK, "rights=0 rejected");
    e = kmmio_map_to_task(tcb, cap, 0xFF, &mapped);
    TEST_ASSERT(e != KERN_OK, "bogus rights rejected");

    /* Bad cap rejected. */
    e = kmmio_map_to_task(tcb, (cap_id_t)-1, CAP_READ, &mapped);
    TEST_ASSERT(e != KERN_OK, "invalid cap rejected");

    (void)cap_delete(cap);
    (void)task_delete(tid);
}

/*============================================================================
 * Module registration
 *============================================================================*/

static void test_mmio_module(void) {
    test_mmio_map_programs_region();
    test_mmio_unmap_clears_region();
    test_mmio_rights_and_bad_cap();
}

TEST_K_MODULE(mmio, test_mmio_module);

#endif /* MPU_ENABLE && CAP_ENABLE && TEST_MODULE_MMIO */
