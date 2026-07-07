/**
 * @file fault_endpoint.h
 * @brief Phase 2 S1 — kernel fault notification endpoint
 *
 * When a user task faults (MemManage/BusFault/UsageFault/HardFault from
 * thread mode), the fault handler pushes a fault_event_t onto a small
 * kernel-side ring and schedules a bottom-half. The BH, running in thread
 * mode, drains the ring into the kern_fault_ep endpoint. A user-mode
 * supervisor task can then recv on that endpoint to learn about crashes
 * and decide whether to restart the faulted task.
 *
 * Lifecycle:
 *   kern_init()  -> kern_fault_endpoint_init()   creates endpoint + BH
 *   fault_handler_c() -> kern_fault_notify()     enqueues event + schedules BH
 *   supervisor task   -> sys_endpoint_recv(kern_fault_ep, &evt, ...)
 *
 * The endpoint is created even if no supervisor has subscribed; events
 * are dropped (with a drop counter) when max_pending is exceeded. This
 * means early boot faults are not lost as long as the supervisor binds
 * before the ring fills.
 *
 * Kconfig: FAULT_ENDPOINT (depends on FAULT_ENABLE + IPC_ENDPOINT).
 */

#ifndef FAULT_ENDPOINT_H
#define FAULT_ENDPOINT_H

#include "kernel_types.h"
#include "kernel_config.h"

#if FAULT_ENDPOINT

#include "endpoint.h"

/*============================================================================
 * fault_event_t — what the supervisor receives
 *============================================================================*/

typedef struct {
    uint32_t fault_type;   /* FAULT_TYPE_* (see fault.h)                       */
    uint32_t task_id;      /* faulted task slot, KERN_INVALID_ID if unknown    */
    uint32_t pc;           /* PC at the fault (from exception frame)           */
    uint32_t cfsr;         /* CFSR snapshot (W1C cleared before capture)       */
    uint32_t mmfar;        /* MMFAR (valid if CFSR.MMARVALID)                  */
    uint32_t bfar;         /* BFAR  (valid if CFSR.BFARVALID)                  */
    uint32_t tick;         /* sched tick at fault                              */
    char     task_name[KERN_TASK_NAME_LEN]; /* §2.2: copied from current->name
                                             * BEFORE reclaim, so the supervisor
                                             * can identify the crasher by name
                                             * even after the slot is reused.   */
} fault_event_t;

_Static_assert(sizeof(fault_event_t) <= IPC_EP_MSG_SIZE,
               "fault_event_t must fit in IPC_EP_MSG_SIZE (64..128)");

/*============================================================================
 * Kernel API
 *============================================================================*/

#define KERN_FAULT_EP_NAME     "kern.fault"
#define KERN_FAULT_EP_PENDING  4
#define KERN_FAULT_RING_SIZE   4

/** Endpoint ID assigned at init. KERN_INVALID_ID until kern_fault_endpoint_init
 *  has run, or if init failed (endpoint pool exhausted). */
extern ep_id_t kern_fault_ep;

/** Number of events dropped because the BH ring was full. Reset only by
 *  reboot. Read-only outside the fault module. */
extern uint32_t kern_fault_dropped_count;

/** Call from kern_init() after ipc_init() and bh_init(). Idempotent. */
void kern_fault_endpoint_init(void);

/** Call from fault_handler_c() in Handler mode. Never blocks. Safe to
 *  call with IRQs disabled (which is the default in fault context).
 *  task_name is copied from the faulting task's name; may be NULL to omit. */
void kern_fault_notify(uint32_t fault_type,
                       uint32_t task_id,
                       const char *task_name,
                       uint32_t pc,
                       uint32_t cfsr,
                       uint32_t mmfar,
                       uint32_t bfar);

#endif /* FAULT_ENDPOINT */
#endif /* FAULT_ENDPOINT_H */
