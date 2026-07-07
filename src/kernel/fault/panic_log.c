/**
 * @file panic_log.c
 * @brief Core completion #5 — persist crash dump to flash on panic
 */

#include "panic_log.h"

#if PANIC_LOG && BLOCK_DEVICE

#include "flash_block.h"
#include <string.h>

/* On-disk layout: magic + counter + crash_dump_t, zero-padded to page size. */
typedef struct {
    uint32_t magic;
    uint32_t counter;
    crash_dump_t dump;
} panic_log_record_t;

_Static_assert(sizeof(panic_log_record_t) <= PANIC_LOG_PAGE_SIZE,
               "panic log record fits in one flash page");

static uint32_t panic_counter = 0;

void panic_log_save(void) {
    /* Pack into a page-aligned buffer. crash_dump is the global filled by
     * fault_handler_c before kern_panic is called. */
    uint8_t buf[PANIC_LOG_PAGE_SIZE];
    memset(buf, 0xFF, sizeof(buf));  /* flash-erase state, safe default */

    panic_log_record_t *rec = (panic_log_record_t *)buf;
    rec->magic = PANIC_LOG_MAGIC;
    rec->counter = ++panic_counter;
    rec->dump = crash_dump;  /* struct copy */

    /* Erase the dedicated sector, then program the page. Both flash_block_*
     * mask IRQs internally, but we're already called from kern_panic with
     * IRQs disabled — the double-mask is harmless (PRIMASK is idempotent). */
    (void)flash_block_erase(PANIC_LOG_SECTOR_OFFSET, FLASH_BLOCK_SECTOR_SIZE);
    (void)flash_block_program(PANIC_LOG_SECTOR_OFFSET, buf, PANIC_LOG_PAGE_SIZE);
}

kern_err_t panic_log_check(crash_dump_t *out) {
    if (out == NULL) {
        return KERN_ERR_PARAM;
    }

    uint8_t buf[PANIC_LOG_PAGE_SIZE];
    kern_err_t e = flash_block_read(PANIC_LOG_SECTOR_OFFSET, buf, sizeof(buf));
    if (e != KERN_OK) {
        return e;
    }

    panic_log_record_t *rec = (panic_log_record_t *)buf;
    if (rec->magic != PANIC_LOG_MAGIC) {
        return KERN_ERR_NOEXIST;
    }

    *out = rec->dump;  /* struct copy */
    return KERN_OK;
}

kern_err_t panic_log_clear(void) {
    return flash_block_erase(PANIC_LOG_SECTOR_OFFSET, FLASH_BLOCK_SECTOR_SIZE);
}

#endif /* PANIC_LOG && BLOCK_DEVICE */
