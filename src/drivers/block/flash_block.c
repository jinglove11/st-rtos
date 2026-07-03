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
#include "pico2w.h"
#include "hardware/flash.h"
#include <string.h>

/* Absolute flash address of a FS-relative offset. */
#define FLASH_ABS(offs) ((uintptr_t)(BOARD_FLASH_BASE + FLASH_FS_OFFSET + (offs)))

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
    uint32_t primask = hal_irq_save();
    flash_range_erase((uint32_t)(FLASH_FS_OFFSET + offs), (size_t)count);
    hal_irq_restore(primask);
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
    uint32_t primask = hal_irq_save();
    flash_range_program((uint32_t)(FLASH_FS_OFFSET + offs),
                        (const uint8_t *)data, (size_t)count);
    hal_irq_restore(primask);
    return KERN_OK;
}

#endif /* BLOCK_DEVICE */
