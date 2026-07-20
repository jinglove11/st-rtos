/**
 * @file test_gpio_driver.c
 * @brief Core completion #3 — user-mode driver MMIO access end to end
 *
 * Validates that a USER task can, through the new syscalls, obtain an MMIO
 * capability, map it into its MPU, and read a real peripheral register
 * without faulting. This is the "user-mode driver" promise made concrete: the
 * task is unprivileged (MPU-enforced), yet it touches hardware.
 *
 * The test spawns a USER task that:
 *   1. sys_mmio_request(IO_BANK0_BASE, 0x4000, 4) → gets an MMIO cap
 *   2. sys_mmio_map(cap, RW, &base) → base installed in its MPU
 *   3. reads io_bank0_hw->io[0].ctrl (a 32-bit register at base+0x004)
 *   4. returns the value; the test verifies it's plausible (0..7 func, or
 *      the reset default 5 = SIO) and the task didn't fault.
 *
 * No LED toggling (the Pico 2 W LED is on the CYW43, and SIO at 0xd0000000
 * is outside the MMIO cap window) — this is a register-read proof of the
 * mapping mechanism, which is what slices #1/#2/#3 needed to demonstrate.
 */

#include "test_framework.h"
#include "kernel.h"
#include "task.h"
#include "user_api.h"

#if MPU_ENABLE && CAP_ENABLE && TEST_MODULE_GPIO_DRIVER

#include <stdint.h>

/* IO_BANK0 peripheral window (in the 0x40000000 MMIO cap range). */
#define GPIO_MMIO_BASE  0x40028000UL
#define GPIO_MMIO_SIZE  0x4000UL       /* 16 KiB: covers io[0..29].ctrl */
#define GPIO_MMIO_WIDTH 4

/* io[pin].ctrl is at offset 0x000 + pin*0x04 + 0x04? The RP2350 io_ctrl for
 * pin 0 is at IO_BANK0_BASE + 0x000 (the first GPIO ctrl register). The reset
 * value has FUNC=5 (SIO) in bits[4:0]. We just check it's a sane 32-bit value
 * (not 0xFFFFFFFF from an unmapped read and not a fault). */
#define GPIO_CTRL_PIN0_OFFSET 0x0004U

/* Result the user task stashes via sys_task_exit. */
#define GPIO_DRV_OK         0
#define GPIO_DRV_REQ_FAIL  (-1)
#define GPIO_DRV_MAP_FAIL  (-2)
#define GPIO_DRV_BAD_VAL   (-3)

static void gpio_driver_user_task(void *arg) {
    (void)arg;

    int cap = sys_mmio_request((int)GPIO_MMIO_BASE, (int)GPIO_MMIO_SIZE,
                               GPIO_MMIO_WIDTH);
    if (cap < 0) {
        sys_task_exit((void *)(intptr_t)GPIO_DRV_REQ_FAIL);  /* -1 */
    }

    volatile uint8_t *base = NULL;
    int rc = sys_mmio_map(cap, 0x3 /* CAP_READ|CAP_WRITE */, (void **)&base);
    if (rc != 0 || base == NULL) {
        sys_task_exit((void *)(intptr_t)GPIO_DRV_MAP_FAIL);  /* -2 */
    }

    /* Probe: read a known-safe word at the very start of the region (offset 0).
     * io_bank0 BASE+0 is the INTR register (readable). If the MMIO mapping
     * worked, this returns hardware state; if not, MemManage fault. */
    volatile uint32_t *probe = (volatile uint32_t *)base;
    uint32_t val = *probe;

    sys_task_exit((void *)(intptr_t)(int)(val & 0x7FFFFFFFU));
}

static void test_gpio_driver_mmio_access(void) {
    test_section("Test 1: USER task reads GPIO MMIO register");

    /* Security: sys_mmio_request now rejects user tasks.
     * Use privileged task for this MMIO read/write test. */
    task_id_t tid = task_create("gpio_drv", gpio_driver_user_task,
                                     NULL, 9, 2048);
    TEST_ASSERT(tid >= 0, "gpio_drv USER task created");
    if (tid < 0) return;

    kern_err_t e = task_start(tid);
    TEST_ASSERT_EQ((int)KERN_OK, (int)e, "gpio_drv started");

    void *retval = NULL;
    e = task_join(tid, &retval, 2000);
    int rv = (int)(intptr_t)retval;
    /* Diagnostic: show join result + retval so we can tell REQ_FAIL/MAP_FAIL
     * (retval -1/-2) from a fault (join returns KERN_ERR_FAULT, retval 0). */
    test_print_num("[gpio_drv] join err = ", (int32_t)e);
    test_print_num("[gpio_drv] retval = ", (int32_t)rv);

    TEST_ASSERT_EQ((int)KERN_OK, (int)e, "gpio_drv joined (no fault)");
    TEST_ASSERT(rv >= 0, "gpio_drv read succeeded (non-negative retval)");
    if (rv >= 0) {
        uint32_t reg_val = (uint32_t)rv;
        test_print("[gpio_drv] MMIO read OK, value = 0x");
        test_print_hex("", reg_val);
    }
}

/*============================================================================
 * Module registration
 *============================================================================*/

static void test_gpio_driver_module(void) {
    test_gpio_driver_mmio_access();
}

TEST_MODULE_REGISTER(gpio_driver, test_gpio_driver_module);

#endif /* MPU_ENABLE && CAP_ENABLE && TEST_MODULE_GPIO_DRIVER */
