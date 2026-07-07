/**
 * @file panic_log.h
 * @brief Core completion #5 — persist crash dump across reboots
 *
 * When the kernel panics (kern_panic), the current crash_dump_t is written
 * to a dedicated sector at the end of the flash FS region. On the next boot,
 * panic_log_check() reads it back so the supervisor/shell can report the
 * last panic's registers and fault type before clearing it.
 *
 * Layout: the last 4 KiB sector of the FS region (FLASH_FS_OFFSET +
 * FLASH_FS_SIZE - FLASH_BLOCK_SECTOR_SIZE) holds a 256-byte page:
 *   bytes 0..3:   PANIC_LOG_MAGIC (0xDEADPANIC)
 *   bytes 4..7:   panic counter (incremented per save)
 *   bytes 8..111: crash_dump_t (104 bytes)
 *   bytes 112..255: zero-padded
 *
 * Kconfig: PANIC_LOG (depends on BLOCK_DEVICE).
 */

#ifndef PANIC_LOG_H
#define PANIC_LOG_H

#include "kernel_types.h"
#include "kernel_config.h"

#if PANIC_LOG && BLOCK_DEVICE

#include "fault.h"

#define PANIC_LOG_MAGIC    0x50414E49U   /* "PANI" */
#define PANIC_LOG_PAGE_SIZE 256U         /* flash program granularity */

/** Offset of the panic-log sector within the FS region (last sector). */
#define PANIC_LOG_SECTOR_OFFSET \
    (FLASH_FS_SIZE - FLASH_BLOCK_SECTOR_SIZE)

/** Write the current crash_dump to flash. Called from kern_panic (IRQs off).
 *  Best-effort: if flash write fails, the panic proceeds anyway. */
void panic_log_save(void);

/** Check for a saved panic log on boot. If present (magic matches), copies
 *  the crash_dump into `out` and returns KERN_OK. Returns KERN_ERR_NOEXIST
 *  if no log or the sector is blank. Does NOT clear the log on read. */
kern_err_t panic_log_check(crash_dump_t *out);

/** Clear the panic log (erase the sector). Call after the supervisor has
 *  reported the panic, so the next boot doesn't re-report a stale one. */
kern_err_t panic_log_clear(void);

#endif /* PANIC_LOG && BLOCK_DEVICE */
#endif /* PANIC_LOG_H */
