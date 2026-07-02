/**
 * @file supervisor.h
 * @brief Minimal reusable service supervisor state
 */

#ifndef USER_SUPERVISOR_H
#define USER_SUPERVISOR_H

#include "kernel_types.h"
#include <stdint.h>

#define SUPERVISOR_SERVICE_MAX 4

typedef enum {
    SUPERVISOR_RESTART_MANUAL = 0,
    SUPERVISOR_RESTART_AUTO = 1,
} supervisor_restart_policy_t;

typedef struct {
    const char *service_name;
    uint32_t restart_count;
    uint32_t recover_count;
    uint32_t fault_count;
    uint32_t pending_client_count;
    supervisor_restart_policy_t restart_policy;
    uint32_t max_restarts;
    int last_health;
} supervisor_service_t;

#define SUPERVISOR_SERVICE_INIT(initial_health) \
    { NULL, 0U, 0U, 0U, 0U, SUPERVISOR_RESTART_MANUAL, 0U, (initial_health) }
#define SUPERVISOR_SERVICE_NAMED_INIT(name, initial_health) \
    { (name), 0U, 0U, 0U, 0U, SUPERVISOR_RESTART_MANUAL, 0U, (initial_health) }

void supervisor_service_init(supervisor_service_t *svc, int initial_health);
void supervisor_service_init_named(supervisor_service_t *svc,
                                   const char *service_name,
                                   int initial_health);
void supervisor_set_service_name(supervisor_service_t *svc,
                                 const char *service_name);
void supervisor_record_restart(supervisor_service_t *svc);
void supervisor_record_recover(supervisor_service_t *svc);
void supervisor_record_fault(supervisor_service_t *svc);
void supervisor_set_health(supervisor_service_t *svc, int health);
void supervisor_set_pending_clients(supervisor_service_t *svc, uint32_t count);
void supervisor_set_restart_policy(supervisor_service_t *svc,
                                   supervisor_restart_policy_t policy,
                                   uint32_t max_restarts);
void supervisor_clear_counts(supervisor_service_t *svc);
void supervisor_reset_service(supervisor_service_t *svc, int initial_health);
void supervisor_client_blocked(supervisor_service_t *svc);
void supervisor_client_unblocked(supervisor_service_t *svc);
uint32_t supervisor_restart_count(const supervisor_service_t *svc);
uint32_t supervisor_recover_count(const supervisor_service_t *svc);
uint32_t supervisor_fault_count(const supervisor_service_t *svc);
uint32_t supervisor_pending_clients(const supervisor_service_t *svc);
supervisor_restart_policy_t supervisor_restart_policy(
    const supervisor_service_t *svc);
uint32_t supervisor_max_restarts(const supervisor_service_t *svc);
int supervisor_should_auto_restart(const supervisor_service_t *svc);
const char *supervisor_restart_policy_name(
    supervisor_restart_policy_t policy);
int supervisor_parse_restart_policy(const char *name,
                                    supervisor_restart_policy_t *out_policy);
const char *supervisor_service_name(const supervisor_service_t *svc);
int supervisor_last_health(const supervisor_service_t *svc);

void supervisor_registry_init(void);
supervisor_service_t *supervisor_register_service(const char *service_name,
                                                  int initial_health);
supervisor_service_t *supervisor_find_service(const char *service_name);
supervisor_service_t *supervisor_service_at(uint32_t index);
uint32_t supervisor_service_count(void);

/*============================================================================
 * Phase 2 §2.2 — fault-driven monitor loop + restart recipes
 *============================================================================*/

#if SUPERVISOR && FAULT_ENDPOINT

#include "kernel_config.h"

/** A restart recipe: everything the supervisor needs to recreate a faulted
 *  task. Populated at boot (hardcoded in Phase 2; future: read from config). */
typedef struct {
    const char *name;          /* matched against fault_event_t.task_name */
    void (*entry)(void *);     /* task entry to restart from              */
    void  *arg;                /* arg passed to entry                     */
    uint8_t priority;          /* task priority                           */
    uint32_t stack_size;       /* stack size in bytes                     */
    uint8_t cap_rights_mask;   /* rights for the restarted task's cap
                                * (CAP_GRANT is always stripped regardless) */
    /* rate-limit state (managed by supervisor_on_fault / supervisor_do_restarts) */
    uint32_t restart_count;    /* restarts issued for this service        */
    uint32_t last_fault_tick;  /* tick of the most recent fault           */
    uint32_t next_restart_tick;/* earliest tick a restart may be issued   */
    uint32_t backoff_ticks;    /* exponential: 1000/2000/4000/8000 ms     */
    uint8_t  killed;           /* set once max restarts exceeded          */
    uint8_t  pending_restart;  /* fault seen, waiting for backoff to fire */
} supervisor_recipe_t;

/** Mutable state owned by one supervisor task.
 *
 * USER tasks cannot access kernel/global SRAM. Keep this object in memory
 * mapped to the supervisor, normally as a local in supervisor_monitor_loop().
 */
typedef struct {
    supervisor_recipe_t recipes[SUPERVISOR_SERVICE_MAX];
    uint8_t recipe_used[SUPERVISOR_SERVICE_MAX];
    int     timer_cap;         /* one-shot timer cap (callback-less)        */
    uint8_t timer_armed;       /* a pending_restart is waiting on the timer */
} supervisor_runtime_t;

/** Restart policy knobs. */
#define SUPERVISOR_MAX_RESTARTS   3   /* permanent kill after this many   */
#define SUPERVISOR_BACKOFF_BASE_MS 1000
#define SUPERVISOR_BACKOFF_CAP_MS  8000

/** Badge the supervisor's one-shot timer stamps onto its endpoint notify.
 * Must not collide with fault_event_t.fault_type (1..4). Picked far away. */
#define SUPERVISOR_TIMER_BADGE 0xDEADBEEFu

/** Register a restart recipe. Called at boot (e.g. from init.c) before the
 *  monitor loop starts. Returns 0 on success, <0 on table-full. */
void supervisor_runtime_init(supervisor_runtime_t *runtime);

int supervisor_register_recipe(supervisor_runtime_t *runtime,
                               const char *name,
                               void (*entry)(void *),
                               void *arg,
                               uint8_t priority,
                               uint32_t stack_size,
                               uint8_t cap_rights_mask);

/** Look up a recipe by service name (NULL-safe). */
supervisor_recipe_t *supervisor_find_recipe(supervisor_runtime_t *runtime,
                                            const char *name);

/** The monitor loop entry point — run as a user task. Creates a one-shot
 *  timer bound to the fault endpoint, then recv-loops forever, dispatching
 *  fault events to supervisor_on_fault and timer notifies to
 *  supervisor_do_restarts. Never returns. */
void supervisor_monitor_loop(void *arg);

/** Record a fault: classify it (kill / schedule restart) and arm the backoff
 *  timer if a restart is pending. Does NOT restart synchronously — the actual
 *  sys_task_restart happens when the timer fires (supervisor_do_restarts).
 *  Returns 1 if a restart was scheduled, 0 if killed/ignored. */
int supervisor_on_fault(supervisor_runtime_t *runtime,
                        const void *event,
                        int fault_ep_cap);

/** Walk pending recipes and restart any whose backoff has elapsed. Called
 *  when the backoff timer fires. Restarts via sys_task_restart (reduced cap,
 *  GRANT stripped). Returns the number of restarts issued. */
int supervisor_do_restarts(supervisor_runtime_t *runtime,
                           int fault_ep_cap);

#endif /* SUPERVISOR && FAULT_ENDPOINT */

#endif /* USER_SUPERVISOR_H */
