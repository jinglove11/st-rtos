/**
 * @file block_dev.h
 * @brief Phase 3 §3.3 — littlefs-style block device over onboard flash
 *
 * A thin sector-oriented wrapper over flash_block (which itself goes through
 * sys_flash_op / the kernel). Phase 4's littlefs will wire its lfs_config
 * .read/.prog/.erase callbacks directly to block_read/block_program/block_erase.
 *
 * "block" here means a flash SECTOR (4 KiB erase unit), matching littlefs's
 * lfs_block_t. read/program may span multiple pages (256 B) within a block;
 * erase operates on whole blocks. Callers erase before programming.
 *
 * Kconfig: BLOCK_DEVICE.
 */

#ifndef BLOCK_DEV_H
#define BLOCK_DEV_H

#include "kernel_types.h"
#include "kernel_config.h"

#if BLOCK_DEVICE

#include <stdint.h>

typedef struct {
    uint32_t base_offset;   /* FS region start, relative to XIP_BASE (FLASH_FS_OFFSET) */
    uint32_t total_size;    /* FS region size in bytes (FLASH_FS_SIZE) */
    uint32_t block_size;    /* erase unit (4096) */
    uint32_t page_size;     /* program unit (256) */
} block_dev_t;

/** Initialize a block_dev_t with the default onboard-flash FS geometry.
 *  Returns KERN_OK. */
kern_err_t block_dev_init(block_dev_t *bd);

/** Read `count` bytes from `block` (sector index) at byte offset 0 into `buf`.
 *  `count` may be <= block_size. Returns KERN_OK or KERN_ERR_PARAM. */
kern_err_t block_read(block_dev_t *bd, uint32_t block,
                      void *buf, uint32_t count);

/** Erase sector `block` (sets it to all 0xFF). Returns KERN_OK or KERN_ERR_PARAM. */
kern_err_t block_erase(block_dev_t *bd, uint32_t block);

/** Program `count` bytes into `block` at byte offset 0 from `data`.
 *  `count` must be a multiple of page_size and <= block_size; the caller must
 *  have erased the block first. Returns KERN_OK or KERN_ERR_PARAM. */
kern_err_t block_program(block_dev_t *bd, uint32_t block,
                         const void *data, uint32_t count);

#endif /* BLOCK_DEVICE */
#endif /* BLOCK_DEV_H */
