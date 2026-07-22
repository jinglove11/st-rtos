/**
 * @file smp.h
 * @brief Core completion #7 — SMP core1 launch
 */

#ifndef SMP_H
#define SMP_H

#include <stdint.h>
#include "kernel_config.h"
#include "kernel_types.h"

#if SMP

/* Coalesced scheduler IPI reasons.  The FIFO carries only a wake-up token;
 * the reason bits live in shared memory so FIFO depth cannot lose events. */
#define SMP_IPI_REMOTE_QUEUE    (1UL << 0)
#define SMP_IPI_STEAL_REQUEST   (1UL << 1)
#define SMP_IPI_STEAL_COMPLETE  (1UL << 2)
#define SMP_IPI_RESCHEDULE      (1UL << 3)
#define SMP_IPI_FLASH_LOCKOUT   (1UL << 4)

/** Launch the second core from core0 during kern_start(). */
kern_err_t smp_init_core1(void);

/** Send coalesced scheduler work to another online CPU. */
void smp_send_ipi(uint32_t target_cpu, uint32_t reasons);

/**
 * Park the peer CPU in SRAM while the caller temporarily makes XIP flash
 * unavailable.  The caller must serialize flash operations separately.
 */
kern_err_t smp_flash_lockout_start(void);

/** Release a peer parked by smp_flash_lockout_start(). */
void smp_flash_lockout_end(void);

#endif /* SMP */

#endif /* SMP_H */
