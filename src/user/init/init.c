/**
 * @file init.c
 * @brief Phase 2 §2.3 — user-mode init process
 *
 * The init process is the first user task (spawned by the bootstrap path once
 * INIT_PROCESS is enabled). In Phase 2 its job is deliberately small:
 *   1. spawn the supervisor as a user task,
 *   2. exit — init does not stick around.
 *
 * Where it is launched: in test builds it runs AFTER test_run_all_modules()
 * completes (so it never disturbs the test suite), just before the shell. In
 * a production build (TEST_ENABLE=n) it is the only thing the bootstrap spawns.
 *
 * The shell is intentionally NOT spawned by init yet: the current shell is a
 * privileged kernel task that calls kernel APIs directly, so making it a
 * child of a user-mode init would fault. The shell stays on the kernel boot
 * path for now (Phase 3 will split it into a proper user-mode service).
 */

#include "kernel_config.h"

#if INIT_PROCESS

#include "supervisor.h"
#include "user_api.h"
#include <stdint.h>

#if FAULT_ENDPOINT && SUPERVISOR
#define INIT_HAS_SUPERVISOR 1
#else
#define INIT_HAS_SUPERVISOR 0
#endif

/* Supervisor task tuning. Priority 2 (just above the BH/timer service tasks),
 * a comfortable stack for the monitor loop. */
#define SUPERVISOR_PRIORITY   2
#define SUPERVISOR_STACK      2048

void init_main(void *arg) {
    (void)arg;

#if INIT_HAS_SUPERVISOR
    /* Spawn the supervisor as a user task. sys_task_create returns a cap id
     * (CAP_ENABLE) or raw task id; sys_task_start accepts either. */
    int sup = sys_task_create("supervisor", supervisor_monitor_loop, NULL,
                              SUPERVISOR_PRIORITY, SUPERVISOR_STACK);
    if (sup >= 0) {
        (void)sys_task_start(sup);
    }
    /* If supervisor creation failed, init still exits — the system degrades
     * to "no fault restart", which is safe (faults still terminate the task,
     * just nothing restarts it). */
#endif

    /* init's job is done; it does not linger. */
    sys_task_exit(NULL);
}

#endif /* INIT_PROCESS */
