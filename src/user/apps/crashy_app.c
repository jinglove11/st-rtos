/**
 * @file crashy_app.c
 * @brief Phase 2 §2 smoke-test service — deliberately crashes.
 *
 * A tiny user task that dereferences an unmapped address, triggering a
 * MemManage fault. Used to exercise the supervisor's fault-restart loop
 * end-to-end: crashy_app faults -> fault_event_t to kern.fault -> supervisor
 * schedules a restart -> backoff timer fires -> sys_task_restart recreates
 * it -> repeat until SUPERVISOR_MAX_RESTARTS, then permanent kill.
 *
 * It is spawned by the supervisor itself (supervisor_monitor_loop) after the
 * crashy_app recipe is registered, so the supervisor owns both the recipe and
 * the first launch.
 */

#include "kernel_config.h"
#include <stdint.h>

#if INIT_PROCESS

/* No headers needed — entry is called with a single void* arg (unused). */
void crashy_app_entry(void *arg) {
    (void)arg;
    /* 0xBBBBBBBB is outside any MPU region a user task has mapped (Flash RO,
     * its own stack). Writing through it raises MemManage immediately.
     * Marked volatile so the compiler cannot elide the store. */
    volatile uint32_t *bad = (volatile uint32_t *)0xBBBBBBBBU;
    *bad = 0xDEADU;
    /* Should never reach here. */
    while (1) { }
}

#endif /* INIT_PROCESS */
