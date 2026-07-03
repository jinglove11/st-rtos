/**
 * @file block_dev.c
 * @brief Phase 3 §3.3 — block_dev_t implementation over sys_flash_op
 */

#include "block_dev.h"

#if BLOCK_DEVICE

#include "user_api.h"
#include "flash_block.h"

kern_err_t block_dev_init(block_dev_t *bd) {
    if (bd == NULL) {
        return KERN_ERR_PARAM;
    }
    bd->base_offset = FLASH_FS_OFFSET;
    bd->total_size  = FLASH_FS_SIZE;
    bd->block_size  = FLASH_BLOCK_SECTOR_SIZE;
    bd->page_size   = FLASH_BLOCK_PAGE_SIZE;
    return KERN_OK;
}

/* Convert (block index, count) into an FS-relative offset for sys_flash_op.
 * sys_flash_op takes offsets relative to FLASH_FS_OFFSET, i.e. 0-based within
 * the FS region — which is exactly block * block_size. */
kern_err_t block_read(block_dev_t *bd, uint32_t block,
                      void *buf, uint32_t count) {
    if (bd == NULL || buf == NULL) {
        return KERN_ERR_PARAM;
    }
    if (count == 0 || count > bd->block_size) {
        return KERN_ERR_PARAM;
    }
    uint32_t offs = block * bd->block_size;
    if (offs > bd->total_size || count > (bd->total_size - offs)) {
        return KERN_ERR_PARAM;
    }
    int rc = sys_flash_op(SYS_FLASH_OP_READ, (int)offs, buf, (int)count);
    return (rc < 0) ? (kern_err_t)rc : KERN_OK;
}

kern_err_t block_erase(block_dev_t *bd, uint32_t block) {
    if (bd == NULL) {
        return KERN_ERR_PARAM;
    }
    uint32_t offs = block * bd->block_size;
    if (offs >= bd->total_size) {
        return KERN_ERR_PARAM;
    }
    int rc = sys_flash_op(SYS_FLASH_OP_ERASE, (int)offs, NULL,
                          (int)bd->block_size);
    return (rc < 0) ? (kern_err_t)rc : KERN_OK;
}

kern_err_t block_program(block_dev_t *bd, uint32_t block,
                         const void *data, uint32_t count) {
    if (bd == NULL || data == NULL) {
        return KERN_ERR_PARAM;
    }
    if (count == 0 || count > bd->block_size) {
        return KERN_ERR_PARAM;
    }
    if ((count % bd->page_size) != 0U) {
        return KERN_ERR_PARAM;
    }
    uint32_t offs = block * bd->block_size;
    if (offs > bd->total_size || count > (bd->total_size - offs)) {
        return KERN_ERR_PARAM;
    }
    int rc = sys_flash_op(SYS_FLASH_OP_PROGRAM, (int)offs, (void *)data,
                          (int)count);
    return (rc < 0) ? (kern_err_t)rc : KERN_OK;
}

#endif /* BLOCK_DEVICE */
