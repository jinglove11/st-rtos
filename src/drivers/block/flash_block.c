/**
 * @file flash_block.c
 * @brief Phase 3 §3.3 — block device over onboard QSPI NOR flash
 *
 * See flash_block.h for the architecture rationale (kernel-side erase/program
 * because the SDK primitives require IRQs off; user tasks can't mask IRQs).
 */

#include "flash_block.h"

#if BLOCK_DEVICE

#include "hal.h"
#include "spinlock.h"
#if SMP
#include "smp.h"
#endif
#include "pico2w.h"
#include "hardware/flash.h"
#include <string.h>

/* Absolute flash address of a FS-relative offset. */
#define FLASH_ABS(offs) ((uintptr_t)(BOARD_FLASH_BASE + FLASH_FS_OFFSET + (offs)))

#if SMP
/* Serializes writers before asking the peer CPU to park.  A plain spinlock is
 * intentional: a contender keeps interrupts enabled and can therefore obey
 * the current owner's lockout IPI. */
static spinlock_t flash_op_lock;
#endif

static kern_err_t flash_block_exclusive_begin(uint32_t *primask) {
#if SMP
    spin_lock(&flash_op_lock);
    kern_err_t err = smp_flash_lockout_start();
    if (err != KERN_OK) {
        spin_unlock(&flash_op_lock);
        return err;
    }
#endif
    *primask = hal_irq_save();
    return KERN_OK;
}

static void flash_block_exclusive_end(uint32_t primask) {
    /* flash_range_* has restored XIP before it returns, so local and remote
     * execution may safely resume.  Release the peer before restoring local
     * interrupts: a local ISR may issue a synchronous remote scheduler op,
     * which cannot complete while the peer is still parked. */
#if SMP
    smp_flash_lockout_end();
#endif
    hal_irq_restore(primask);
#if SMP
    spin_unlock(&flash_op_lock);
#endif
}

/* Validate an FS-region offset range. offs/count are relative to FLASH_FS_OFFSET. */
static int flash_block_in_bounds(uint32_t offs, uint32_t count) {
    /* offs + count must not wrap and must stay within the FS region. */
    if (offs > FLASH_FS_SIZE) {
        return 0;
    }
    if (count > (FLASH_FS_SIZE - offs)) {
        return 0;
    }
    return 1;
}

kern_err_t flash_block_read(uint32_t offs, void *buf, uint32_t count) {
    if (buf == NULL) {
        return KERN_ERR_PARAM;
    }
    if (!flash_block_in_bounds(offs, count)) {
        return KERN_ERR_PARAM;
    }
    /* Read is a plain XIP fetch — no IRQ masking needed, no cache concern. */
    memcpy(buf, (const void *)FLASH_ABS(offs), count);
    return KERN_OK;
}

kern_err_t flash_block_erase(uint32_t offs, uint32_t count) {
    if (!flash_block_in_bounds(offs, count)) {
        return KERN_ERR_PARAM;
    }
    /* SDK requires sector-aligned offset and a multiple of FLASH_SECTOR_SIZE. */
    if ((offs & (FLASH_BLOCK_SECTOR_SIZE - 1U)) != 0U) {
        return KERN_ERR_PARAM;
    }
    if ((count & (FLASH_BLOCK_SECTOR_SIZE - 1U)) != 0U) {
        return KERN_ERR_PARAM;
    }
    if (count == 0U) {
        return KERN_OK;
    }

    /* flash_range_erase reconfigures QSPI and runs from bootrom; IRQs must be
     * off so no flash-resident ISR fires mid-operation. The SDK call does not
     * disable them itself (verified in hardware_flash/flash.c). */
    uint32_t primask;
    kern_err_t err = flash_block_exclusive_begin(&primask);
    if (err != KERN_OK) {
        return err;
    }
    flash_range_erase((uint32_t)(FLASH_FS_OFFSET + offs), (size_t)count);
    flash_block_exclusive_end(primask);
    return KERN_OK;
}

kern_err_t flash_block_program(uint32_t offs, const void *data, uint32_t count) {
    if (data == NULL) {
        return KERN_ERR_PARAM;
    }
    if (!flash_block_in_bounds(offs, count)) {
        return KERN_ERR_PARAM;
    }
    /* SDK requires page-aligned offset and a multiple of FLASH_PAGE_SIZE. */
    if ((offs & (FLASH_BLOCK_PAGE_SIZE - 1U)) != 0U) {
        return KERN_ERR_PARAM;
    }
    if ((count & (FLASH_BLOCK_PAGE_SIZE - 1U)) != 0U) {
        return KERN_ERR_PARAM;
    }
    if (count == 0U) {
        return KERN_OK;
    }

    /* Program data must be in SRAM (the SDK DMAs from the buffer); data passed
     * in from a kernel caller is on a stack/buffer in SRAM, fine. IRQs off as
     * with erase. */
    uint32_t primask;
    kern_err_t err = flash_block_exclusive_begin(&primask);
    if (err != KERN_OK) {
        return err;
    }
    flash_range_program((uint32_t)(FLASH_FS_OFFSET + offs),
                        (const uint8_t *)data, (size_t)count);
    flash_block_exclusive_end(primask);
    return KERN_OK;
}

#endif /* BLOCK_DEVICE */
