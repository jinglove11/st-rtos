/**
 * @file fault_endpoint.c
 * @brief Phase 2 S1 — kernel fault notification endpoint implementation
 */

#include "fault_endpoint.h"

#if FAULT_ENDPOINT

#include "bh.h"
#include "hal.h"
#include "kernel.h"
#include "spinlock.h"
#include <string.h>

/*============================================================================
 * State
 *============================================================================*/

ep_id_t kern_fault_ep = KERN_INVALID_ID;
uint32_t kern_fault_dropped_count = 0;

/** Single-producer (fault handler, in Handler mode) / single-consumer (BH
 *  task, in thread mode) ring. The producer runs with IRQs disabled by
 *  virtue of being in fault context; the consumer explicitly IRQ-saves
 *  around its drain loop. */
static fault_event_t fault_ring[KERN_FAULT_RING_SIZE];
static volatile uint32_t fault_ring_head = 0;  /* writer advances */
static volatile uint32_t fault_ring_tail = 0;  /* reader advances */
static irq_spinlock_t fault_ring_lock;

static int16_t fault_bh_id = -1;

/* Tick counter is owned by the scheduler; declared here (not via a scheduler
 * header) to keep the fault module's include surface minimal. Safe to read
 * from fault context (IRQs disabled) — it reads a volatile counter. */
extern uint32_t sched_get_tick_count(void);

/*============================================================================
 * BH drain — runs in thread mode (BH service task context)
 *============================================================================*/

static void fault_bh_handler(void *arg) {
    (void)arg;

    if (kern_fault_ep == KERN_INVALID_ID) {
        return;
    }

    for (;;) {
        uint32_t crit = irq_spin_lock(&fault_ring_lock);
        if (fault_ring_tail == fault_ring_head) {
            irq_spin_unlock(&fault_ring_lock, crit);
            break;
        }
        fault_event_t snapshot = fault_ring[fault_ring_tail];
        fault_ring_tail = (fault_ring_tail + 1U) % KERN_FAULT_RING_SIZE;
        irq_spin_unlock(&fault_ring_lock, crit);

        (void)endpoint_notify(kern_fault_ep, &snapshot);
    }
}

/*============================================================================
 * Init
 *============================================================================*/

void kern_fault_endpoint_init(void) {
    if (kern_fault_ep != KERN_INVALID_ID) {
        return;  /* already initialized */
    }

    irq_spin_init_rank(&fault_ring_lock, LOCKDEP_RANK_OBJECT);
    kern_fault_ep = endpoint_create(KERN_FAULT_EP_NAME,
                                    (uint16_t)sizeof(fault_event_t),
                                    KERN_FAULT_EP_PENDING);
    if (kern_fault_ep == KERN_INVALID_ID) {
        return;  /* endpoint pool exhausted; fault notify will be a no-op */
    }

    fault_bh_id = bh_create(fault_bh_handler, NULL);
    if (fault_bh_id < 0) {
        /* BH slot exhaustion — leave endpoint in place; notify will still
         * work if some other path drains the ring (none today, but the
         * API contract allows for it). */
        fault_bh_id = -1;
    }

    uint32_t crit = irq_spin_lock(&fault_ring_lock);
    fault_ring_head = 0;
    fault_ring_tail = 0;
    kern_fault_dropped_count = 0;
    irq_spin_unlock(&fault_ring_lock, crit);
}

/*============================================================================
 * Notify — called from fault_handler_c (Handler mode, IRQs disabled)
 *============================================================================*/

void kern_fault_notify(uint32_t fault_type,
                       uint32_t task_id,
                       const char *task_name,
                       uint32_t pc,
                       uint32_t cfsr,
                       uint32_t mmfar,
                       uint32_t bfar) {
    /* If init failed or hasn't run yet, drop silently. */
    if (kern_fault_ep == KERN_INVALID_ID) {
        return;
    }

    uint32_t crit = irq_spin_lock(&fault_ring_lock);

    uint32_t head = fault_ring_head;
    uint32_t next = (head + 1U) % KERN_FAULT_RING_SIZE;

    if (next == fault_ring_tail) {
        /* Ring full — overwrite oldest, bump drop counter so the
         * supervisor can detect lost events. */
        fault_ring_tail = (fault_ring_tail + 1U) % KERN_FAULT_RING_SIZE;
        kern_fault_dropped_count++;
    }

    fault_event_t *slot = &fault_ring[head];
    slot->fault_type = fault_type;
    slot->task_id    = task_id;
    slot->pc         = pc;
    slot->cfsr       = cfsr;
    slot->mmfar      = mmfar;
    slot->bfar       = bfar;
    slot->tick       = sched_get_tick_count();

    /* Copy the faulting task's name (best-effort, NUL-terminated). The TCB
     * is still valid here — fault_handler_c runs BEFORE task_reclaim_expired,
     * so current->name has not been zeroed. */
    slot->task_name[0] = '\0';
    if (task_name != NULL) {
        for (uint32_t i = 0; i < (KERN_TASK_NAME_LEN - 1U); i++) {
            slot->task_name[i] = task_name[i];
            if (task_name[i] == '\0') {
                break;
            }
        }
        slot->task_name[KERN_TASK_NAME_LEN - 1U] = '\0';
    }

    fault_ring_head = next;
    irq_spin_unlock(&fault_ring_lock, crit);

    if (fault_bh_id >= 0) {
        (void)bh_schedule(fault_bh_id);
    }
}

#endif /* FAULT_ENDPOINT */
