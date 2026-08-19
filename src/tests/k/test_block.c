/**
 * @file test_block.c
 * @brief Phase 3 §3.3 — block device (onboard QSPI NOR flash) tests
 *
 * Exercises flash_block_read/erase/program directly (kernel-privileged test
 * context — no syscall). All offsets are RELATIVE to the FS region
 * (FLASH_FS_OFFSET..+FLASH_FS_SIZE), so firmware is never touched.
 *
 * The 100-sector erase/program/readback loop is the §3.3 exit condition.
 *
 * NOTE: erase/program disable IRQs for tens of ms per sector; the 100-sector
 * test makes the system unresponsive for a few seconds — that is expected.
 */

#include "test_framework.h"
#include "kernel.h"
#include "flash_block.h"

#if BLOCK_DEVICE && TEST_MODULE_BLOCK

#include <string.h>

/* Work buffers. A full sector is 4 KiB; keep two statically so the test never
 * overflows the test_runner stack. */
static uint8_t g_buf_a[FLASH_BLOCK_SECTOR_SIZE];
static uint8_t g_buf_b[FLASH_BLOCK_SECTOR_SIZE];

/*============================================================================
 * Test 1: geometry + initial erase state of the first FS sector
 *============================================================================*/

static void test_block_geometry_and_blank(void) {
    test_section("Test 1: block geometry + blank sector");

    TEST_ASSERT_EQ((int)FLASH_BLOCK_SECTOR_SIZE, 4096, "sector size 4KiB");
    TEST_ASSERT_EQ((int)FLASH_BLOCK_PAGE_SIZE, 256, "page size 256B");
    TEST_ASSERT(FLASH_FS_SIZE >= (3UL * 1024UL * 1024UL), "FS region >= 3MiB");
    TEST_ASSERT_EQ((int)FLASH_FS_SECTORS, (int)(3UL * 1024UL * 1024UL / 4096UL),
                   "768 sectors");

    /* The first sector may or may not be erased depending on prior runs, so
     * erase it first to get a known state, then read it back all-0xFF. */
    kern_err_t e = flash_block_erase(0, FLASH_BLOCK_SECTOR_SIZE);
    TEST_ASSERT_EQ((int)KERN_OK, (int)e, "erase sector 0 OK");
    e = flash_block_read(0, g_buf_a, FLASH_BLOCK_SECTOR_SIZE);
    TEST_ASSERT_EQ((int)KERN_OK, (int)e, "read sector 0 OK");
    int all_ff = 1;
    for (uint32_t i = 0; i < FLASH_BLOCK_SECTOR_SIZE; i++) {
        if (g_buf_a[i] != 0xFFU) { all_ff = 0; break; }
    }
    TEST_ASSERT(all_ff, "erased sector reads all 0xFF");
}

/*============================================================================
 * Test 2: program one page (256B) and read it back
 *============================================================================*/

static void test_block_program_page(void) {
    test_section("Test 2: program + readback one page");

    kern_err_t e = flash_block_erase(0, FLASH_BLOCK_SECTOR_SIZE);
    TEST_ASSERT_EQ((int)KERN_OK, (int)e, "erase before program");

    /* Fill a page with a recognizable pattern. */
    for (uint32_t i = 0; i < FLASH_BLOCK_PAGE_SIZE; i++) {
        g_buf_a[i] = (uint8_t)(i ^ 0xA5U);
    }
    e = flash_block_program(0, g_buf_a, FLASH_BLOCK_PAGE_SIZE);
    TEST_ASSERT_EQ((int)KERN_OK, (int)e, "program page 0");

    memset(g_buf_b, 0, sizeof(g_buf_b));
    e = flash_block_read(0, g_buf_b, FLASH_BLOCK_PAGE_SIZE);
    TEST_ASSERT_EQ((int)KERN_OK, (int)e, "read page 0");
    TEST_ASSERT(memcmp(g_buf_a, g_buf_b, FLASH_BLOCK_PAGE_SIZE) == 0,
                "programmed page reads back identical");
}

/*============================================================================
 * Test 3: program spanning multiple pages within a sector
 *============================================================================*/

static void test_block_program_multi_page(void) {
    test_section("Test 3: program multi-page (1024B)");

    kern_err_t e = flash_block_erase(0, FLASH_BLOCK_SECTOR_SIZE);
    TEST_ASSERT_EQ((int)KERN_OK, (int)e, "erase before multi-page");

    uint32_t n = FLASH_BLOCK_PAGE_SIZE * 4U;  /* 1024B, 4 pages */
    for (uint32_t i = 0; i < n; i++) {
        g_buf_a[i] = (uint8_t)((i * 7U) ^ 0x5AU);
    }
    e = flash_block_program(0, g_buf_a, n);
    TEST_ASSERT_EQ((int)KERN_OK, (int)e, "program 4 pages");

    memset(g_buf_b, 0, n);
    e = flash_block_read(0, g_buf_b, n);
    TEST_ASSERT_EQ((int)KERN_OK, (int)e, "read 4 pages");
    TEST_ASSERT(memcmp(g_buf_a, g_buf_b, n) == 0,
                "multi-page readback identical");
}

/*============================================================================
 * Test 4: erase/program/read 100 sectors (§3.3 exit condition)
 *============================================================================*/

static void test_block_100_sectors(void) {
    test_section("Test 4: erase/program/read 100 sectors");

    int failures = 0;
    for (uint32_t s = 0; s < 100U; s++) {
        uint32_t offs = s * FLASH_BLOCK_SECTOR_SIZE;
        if (offs + FLASH_BLOCK_SECTOR_SIZE > FLASH_FS_SIZE) {
            break;  /* FS region smaller than 100 sectors — stop early */
        }

        kern_err_t e = flash_block_erase(offs, FLASH_BLOCK_SECTOR_SIZE);
        if (e != KERN_OK) { failures++; continue; }

        /* Distinct pattern per sector: byte i = (s + i) so we detect cross-
         * sector corruption or stale data. */
        for (uint32_t i = 0; i < FLASH_BLOCK_SECTOR_SIZE; i++) {
            g_buf_a[i] = (uint8_t)(s + i);
        }
        e = flash_block_program(offs, g_buf_a, FLASH_BLOCK_SECTOR_SIZE);
        if (e != KERN_OK) { failures++; continue; }

        memset(g_buf_b, 0, sizeof(g_buf_b));
        e = flash_block_read(offs, g_buf_b, FLASH_BLOCK_SECTOR_SIZE);
        if (e != KERN_OK) { failures++; continue; }

        if (memcmp(g_buf_a, g_buf_b, FLASH_BLOCK_SECTOR_SIZE) != 0) {
            failures++;
        }
    }
    TEST_ASSERT_EQ(0, failures, "100 sectors erase/program/read all match");
}

/*============================================================================
 * Test 5: bounds checking rejects out-of-region offsets
 *============================================================================*/

static void test_block_bounds(void) {
    test_section("Test 5: bounds checking");

    kern_err_t e = flash_block_read(FLASH_FS_SIZE, g_buf_a, 1);
    TEST_ASSERT(e != KERN_OK, "read at FS_SIZE rejected");

    e = flash_block_read(FLASH_FS_SIZE - 1, g_buf_a, 2);
    TEST_ASSERT(e != KERN_OK, "read spanning FS end rejected");

    e = flash_block_erase(FLASH_FS_SIZE, FLASH_BLOCK_SECTOR_SIZE);
    TEST_ASSERT(e != KERN_OK, "erase past FS end rejected");

    /* Unaligned erase is rejected. */
    e = flash_block_erase(1, FLASH_BLOCK_SECTOR_SIZE);
    TEST_ASSERT(e != KERN_OK, "unaligned erase offset rejected");

    /* Unaligned program is rejected. */
    e = flash_block_program(1, g_buf_a, FLASH_BLOCK_PAGE_SIZE);
    TEST_ASSERT(e != KERN_OK, "unaligned program offset rejected");
}

/*============================================================================
 * Module registration
 *============================================================================*/

static void test_block_module(void) {
    test_block_geometry_and_blank();
    test_block_program_page();
    test_block_program_multi_page();
    test_block_100_sectors();
    test_block_bounds();
}

TEST_K_MODULE(block, test_block_module);

#endif /* BLOCK_DEVICE && TEST_MODULE_BLOCK */
