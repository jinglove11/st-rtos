/**
 * @file flash_block.h
 * @brief Phase 3 §3.3 — block device over onboard QSPI NOR flash (W25Q32JV)
 *
 * A minimal block abstraction over the RP2350's XIP-mapped QSPI flash, used
 * to back the persistent filesystem (Phase 4 littlefs). Three operations:
 * read (direct XIP memcpy), erase (SDK flash_range_erase), program
 * (SDK flash_range_program).
 *
 * Why this lives in the kernel (not a user-mode server): the SDK
 * flash_range_erase/program primitives MUST run with interrupts disabled
 * (they reconfigure the QSPI controller and fetch from bootrom; an IRQ taken
 * mid-operation whose handler lives in flash would fault). A user-mode task
 * cannot mask IRQs, so erase/program are exposed via SYSCALL_FLASH_OP and
 * executed in handler context with hal_irq_save/restore around them. Read is
 * a plain XIP memcpy but goes through the same syscall for a uniform, bounds-
 * checked interface.
 *
 * Partition: the firmware occupies 0x10000000..__flash_binary_end (~390 KB).
 * The filesystem region is carved out ABOVE that by offset convention:
 *   FLASH_FS_OFFSET .. FLASH_FS_OFFSET + FLASH_FS_SIZE
 * Both are 4 KiB (sector) aligned. Nothing in the build enforces the boundary;
 * callers (tests, FS layer) must stay within FLASH_FS_OFFSET..+FLASH_FS_SIZE.
 *
 * Kconfig: BLOCK_DEVICE (depends on DRIVER_ENABLE).
 */

#ifndef FLASH_BLOCK_H
#define FLASH_BLOCK_H

#include "kernel_types.h"
#include "kernel_config.h"

#if BLOCK_DEVICE

#include <stdint.h>

/*============================================================================
 * Geometry — keep in sync with the SDK flash.h and the FS partition choice.
 *============================================================================*/

/* The W25Q erase/program granularities (SDK hardware/flash.h). */
#define FLASH_BLOCK_SECTOR_SIZE   4096U   /* erase granularity (flash_range_erase) */
#define FLASH_BLOCK_PAGE_SIZE     256U    /* program granularity (flash_range_program) */

/*
 * Filesystem partition within the 4 MiB flash. The firmware ends around
 * 0x62000 (build-dependent, via __flash_binary_end); we reserve a comfortable
 * margin and start the FS region at 1 MiB. Size 3 MiB = 768 sectors.
 *
 * NOTE: erasing/programming below FLASH_FS_OFFSET will corrupt the firmware
 * and brick the board. flash_block_* bounds-check against [0, FLASH_FS_SIZE)
 * relative to FLASH_FS_OFFSET, i.e. callers pass an offset WITHIN the FS
 * region (0..FLASH_FS_SIZE), never an absolute flash offset.
 */
#define FLASH_FS_OFFSET   0x100000UL      /* 1 MiB from XIP_BASE */
#define FLASH_FS_SIZE     0x300000UL      /* 3 MiB */
#define FLASH_FS_SECTORS  (FLASH_FS_SIZE / FLASH_BLOCK_SECTOR_SIZE)

/*============================================================================
 * Operations — offsets are RELATIVE to FLASH_FS_OFFSET, in [0, FLASH_FS_SIZE).
 *============================================================================*/

/** Read `count` bytes from FS offset `offs` into `buf`. Bounds-checked.
 *  Returns KERN_OK or KERN_ERR_PARAM. */
kern_err_t flash_block_read(uint32_t offs, void *buf, uint32_t count);

/** Erase `count` bytes starting at FS offset `offs` (must be sector-aligned,
 *  count a multiple of FLASH_BLOCK_SECTOR_SIZE). IRQs disabled around the
 *  SDK call. Returns KERN_OK or KERN_ERR_PARAM. */
kern_err_t flash_block_erase(uint32_t offs, uint32_t count);

/** Program `count` bytes at FS offset `offs` from `data` (offset+count must
 *  be page-aligned to FLASH_BLOCK_PAGE_SIZE per the SDK; callers should erase
 *  the containing sector first). IRQs disabled around the SDK call.
 *  Returns KERN_OK or KERN_ERR_PARAM. */
kern_err_t flash_block_program(uint32_t offs, const void *data, uint32_t count);

#endif /* BLOCK_DEVICE */
#endif /* FLASH_BLOCK_H */
