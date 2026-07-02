/**
 * @file supervisor.c
 * @brief Minimal reusable service supervisor state
 */

#include "supervisor.h"
#include <string.h>

#if SUPERVISOR && FAULT_ENDPOINT
#include "kernel_config.h"
#include "user_api.h"
#include "fault_endpoint.h"
#endif

static supervisor_service_t supervisor_services[SUPERVISOR_SERVICE_MAX];
static uint8_t supervisor_service_used[SUPERVISOR_SERVICE_MAX];

void supervisor_service_init(supervisor_service_t *svc, int initial_health) {
    supervisor_service_init_named(svc, NULL, initial_health);
}

void supervisor_service_init_named(supervisor_service_t *svc,
                                   const char *service_name,
                                   int initial_health) {
    if (svc == NULL) {
        return;
    }
    svc->service_name = service_name;
    svc->restart_count = 0;
    svc->recover_count = 0;
    svc->fault_count = 0;
    svc->pending_client_count = 0;
    svc->restart_policy = SUPERVISOR_RESTART_MANUAL;
    svc->max_restarts = 0;
    svc->last_health = initial_health;
}

void supervisor_set_service_name(supervisor_service_t *svc,
                                 const char *service_name) {
    if (svc == NULL) {
        return;
    }
    svc->service_name = service_name;
}

void supervisor_record_restart(supervisor_service_t *svc) {
    if (svc == NULL) {
        return;
    }
    svc->restart_count++;
}

void supervisor_record_recover(supervisor_service_t *svc) {
    if (svc == NULL) {
        return;
    }
    svc->recover_count++;
}

void supervisor_record_fault(supervisor_service_t *svc) {
    if (svc == NULL) {
        return;
    }
    svc->fault_count++;
}

void supervisor_set_health(supervisor_service_t *svc, int health) {
    if (svc == NULL) {
        return;
    }
    svc->last_health = health;
}

void supervisor_set_pending_clients(supervisor_service_t *svc, uint32_t count) {
    if (svc == NULL) {
        return;
    }
    svc->pending_client_count = count;
}

void supervisor_set_restart_policy(supervisor_service_t *svc,
                                   supervisor_restart_policy_t policy,
                                   uint32_t max_restarts) {
    if (svc == NULL) {
        return;
    }
    if (policy != SUPERVISOR_RESTART_AUTO) {
        policy = SUPERVISOR_RESTART_MANUAL;
        max_restarts = 0;
    }
    svc->restart_policy = policy;
    svc->max_restarts = max_restarts;
}

void supervisor_reset_service(supervisor_service_t *svc, int initial_health) {
    if (svc == NULL) {
        return;
    }
    const char *service_name = svc->service_name;
    supervisor_service_init_named(svc, service_name, initial_health);
}

void supervisor_clear_counts(supervisor_service_t *svc) {
    if (svc == NULL) {
        return;
    }
    svc->restart_count = 0;
    svc->recover_count = 0;
    svc->fault_count = 0;
}

void supervisor_client_blocked(supervisor_service_t *svc) {
    if (svc == NULL) {
        return;
    }
    svc->pending_client_count++;
}

void supervisor_client_unblocked(supervisor_service_t *svc) {
    if (svc == NULL || svc->pending_client_count == 0U) {
        return;
    }
    svc->pending_client_count--;
}

uint32_t supervisor_restart_count(const supervisor_service_t *svc) {
    return svc ? svc->restart_count : 0U;
}

uint32_t supervisor_recover_count(const supervisor_service_t *svc) {
    return svc ? svc->recover_count : 0U;
}

uint32_t supervisor_fault_count(const supervisor_service_t *svc) {
    return svc ? svc->fault_count : 0U;
}

uint32_t supervisor_pending_clients(const supervisor_service_t *svc) {
    return svc ? svc->pending_client_count : 0U;
}

supervisor_restart_policy_t supervisor_restart_policy(
    const supervisor_service_t *svc) {
    return svc ? svc->restart_policy : SUPERVISOR_RESTART_MANUAL;
}

uint32_t supervisor_max_restarts(const supervisor_service_t *svc) {
    return svc ? svc->max_restarts : 0U;
}

int supervisor_should_auto_restart(const supervisor_service_t *svc) {
    if (svc == NULL) {
        return 0;
    }
    if (svc->restart_policy != SUPERVISOR_RESTART_AUTO) {
        return 0;
    }
    return svc->restart_count < svc->max_restarts;
}

const char *supervisor_restart_policy_name(
    supervisor_restart_policy_t policy) {
    switch (policy) {
    case SUPERVISOR_RESTART_AUTO:
        return "auto";
    case SUPERVISOR_RESTART_MANUAL:
    default:
        return "manual";
    }
}

int supervisor_parse_restart_policy(const char *name,
                                    supervisor_restart_policy_t *out_policy) {
    if (name == NULL || out_policy == NULL) {
        return KERN_ERR_PARAM;
    }
    if (strcmp(name, "manual") == 0) {
        *out_policy = SUPERVISOR_RESTART_MANUAL;
        return KERN_OK;
    }
    if (strcmp(name, "auto") == 0) {
        *out_policy = SUPERVISOR_RESTART_AUTO;
        return KERN_OK;
    }
    return KERN_ERR_PARAM;
}

const char *supervisor_service_name(const supervisor_service_t *svc) {
    return (svc && svc->service_name) ? svc->service_name : "(unnamed)";
}

int supervisor_last_health(const supervisor_service_t *svc) {
    return svc ? svc->last_health : 0;
}

void supervisor_registry_init(void) {
    for (uint32_t i = 0; i < SUPERVISOR_SERVICE_MAX; i++) {
        supervisor_service_used[i] = 0U;
        supervisor_service_init(&supervisor_services[i], KERN_ERR_STATE);
    }
}

supervisor_service_t *supervisor_find_service(const char *service_name) {
    if (service_name == NULL) {
        return NULL;
    }

    for (uint32_t i = 0; i < SUPERVISOR_SERVICE_MAX; i++) {
        if (supervisor_service_used[i] &&
            supervisor_services[i].service_name != NULL &&
            strcmp(supervisor_services[i].service_name, service_name) == 0) {
            return &supervisor_services[i];
        }
    }
    return NULL;
}

supervisor_service_t *supervisor_register_service(const char *service_name,
                                                  int initial_health) {
    if (service_name == NULL) {
        return NULL;
    }

    supervisor_service_t *existing = supervisor_find_service(service_name);
    if (existing != NULL) {
        return existing;
    }

    for (uint32_t i = 0; i < SUPERVISOR_SERVICE_MAX; i++) {
        if (!supervisor_service_used[i]) {
            supervisor_service_used[i] = 1U;
            supervisor_service_init_named(&supervisor_services[i],
                                          service_name,
                                          initial_health);
            return &supervisor_services[i];
        }
    }
    return NULL;
}

supervisor_service_t *supervisor_service_at(uint32_t index) {
    uint32_t seen = 0;

    for (uint32_t i = 0; i < SUPERVISOR_SERVICE_MAX; i++) {
        if (!supervisor_service_used[i]) {
            continue;
        }
        if (seen == index) {
            return &supervisor_services[i];
        }
        seen++;
    }
    return NULL;
}

uint32_t supervisor_service_count(void) {
    uint32_t count = 0;

    for (uint32_t i = 0; i < SUPERVISOR_SERVICE_MAX; i++) {
        if (supervisor_service_used[i]) {
            count++;
        }
    }
    return count;
}

/*============================================================================
 * Phase 2 §2.2 — fault-driven monitor loop + restart recipes
 *============================================================================*/

#if SUPERVISOR && FAULT_ENDPOINT

void supervisor_runtime_init(supervisor_runtime_t *runtime) {
    if (runtime != NULL) {
        memset(runtime, 0, sizeof(*runtime));
        runtime->timer_cap = -1;
    }
}

int supervisor_register_recipe(supervisor_runtime_t *runtime,
                               const char *name,
                               void (*entry)(void *),
                               void *arg,
                               uint8_t priority,
                               uint32_t stack_size,
                               uint8_t cap_rights_mask) {
    if (runtime == NULL || name == NULL || entry == NULL) {
        return KERN_ERR_PARAM;
    }
    /* Idempotent: refresh existing recipe. */
    for (uint32_t i = 0; i < SUPERVISOR_SERVICE_MAX; i++) {
        if (runtime->recipe_used[i] &&
            strcmp(runtime->recipes[i].name, name) == 0) {
            runtime->recipes[i].entry = entry;
            runtime->recipes[i].arg = arg;
            runtime->recipes[i].priority = priority;
            runtime->recipes[i].stack_size = stack_size;
            runtime->recipes[i].cap_rights_mask = cap_rights_mask;
            /* keep rate-limit state across re-registration */
            return KERN_OK;
        }
    }
    for (uint32_t i = 0; i < SUPERVISOR_SERVICE_MAX; i++) {
        if (!runtime->recipe_used[i]) {
            runtime->recipes[i].name = name;
            runtime->recipes[i].entry = entry;
            runtime->recipes[i].arg = arg;
            runtime->recipes[i].priority = priority;
            runtime->recipes[i].stack_size = stack_size;
            runtime->recipes[i].cap_rights_mask = cap_rights_mask;
            runtime->recipes[i].restart_count = 0;
            runtime->recipes[i].last_fault_tick = 0;
            runtime->recipes[i].next_restart_tick = 0;
            runtime->recipes[i].backoff_ticks = SUPERVISOR_BACKOFF_BASE_MS;
            runtime->recipes[i].killed = 0;
            runtime->recipes[i].pending_restart = 0;
            runtime->recipe_used[i] = 1;
            return KERN_OK;
        }
    }
    return KERN_ERR_RESOURCE;
}

supervisor_recipe_t *supervisor_find_recipe(supervisor_runtime_t *runtime,
                                            const char *name) {
    if (runtime == NULL || name == NULL) {
        return NULL;
    }
    for (uint32_t i = 0; i < SUPERVISOR_SERVICE_MAX; i++) {
        if (runtime->recipe_used[i] &&
            strcmp(runtime->recipes[i].name, name) == 0) {
            return &runtime->recipes[i];
        }
    }
    return NULL;
}

/* Re-arm the one-shot backoff timer to fire at the EARLIEST pending
 * next_restart_tick across all recipes. Caller passes the fault ep cap so the
 * timer's notify lands where the supervisor recv's. No-op if nothing pending
 * or the timer was never created. */
static void supervisor_arm_timer(supervisor_runtime_t *runtime,
                                 int fault_ep_cap) {
    if (runtime == NULL || runtime->timer_cap < 0) {
        return;
    }
    uint32_t earliest = 0;
    uint8_t have_pending = 0;
    for (uint32_t i = 0; i < SUPERVISOR_SERVICE_MAX; i++) {
        supervisor_recipe_t *r = &runtime->recipes[i];
        if (runtime->recipe_used[i] && r->pending_restart && !r->killed) {
            if (!have_pending || r->next_restart_tick < earliest) {
                earliest = r->next_restart_tick;
            }
            have_pending = 1;
        }
    }
    if (!have_pending) {
        runtime->timer_armed = 0;
        return;
    }
    /* USER tasks read the tick via syscall (kernel SRAM is unmapped). */
    uint32_t now = (uint32_t)sys_get_tick();
    uint32_t delta = (earliest > now) ? (earliest - now) : 1U;
    if (delta == 0) {
        delta = 1;  /* never start a 0-period one-shot */
    }
    (void)fault_ep_cap;
    (void)sys_timer_start(runtime->timer_cap, (int)delta);
    runtime->timer_armed = 1;
}

/* event is a fault_event_t* (const void* so the header stays clean). */
int supervisor_on_fault(supervisor_runtime_t *runtime,
                        const void *event,
                        int fault_ep_cap) {
    const fault_event_t *evt = (const fault_event_t *)event;
    if (runtime == NULL || evt == NULL) {
        return 0;
    }

    supervisor_recipe_t *r = supervisor_find_recipe(runtime, evt->task_name);
    if (r == NULL) {
        /* Unknown task — not our responsibility. */
        return 0;
    }
    if (r->killed) {
        /* Permanently killed; ignore further faults. */
        return 0;
    }

    r->last_fault_tick = evt->tick;

    /* Max restarts exceeded → permanent kill. */
    if (r->restart_count >= SUPERVISOR_MAX_RESTARTS) {
        r->killed = 1;
        r->pending_restart = 0;
        return 0;
    }

    /* Schedule a (re)start after the backoff elapses. The actual
     * sys_task_restart happens in supervisor_do_restarts when the timer
     * fires — this is what avoids the deadlock (a skipped restart would
     * otherwise leave the faulted task dead with no wake source). */
    r->pending_restart = 1;
    r->next_restart_tick = evt->tick + r->backoff_ticks;

    supervisor_arm_timer(runtime, fault_ep_cap);
    return 1;
}

int supervisor_do_restarts(supervisor_runtime_t *runtime,
                           int fault_ep_cap) {
    if (runtime == NULL) {
        return 0;
    }
    /* USER tasks read the tick via syscall (kernel SRAM is unmapped). */
    uint32_t now = (uint32_t)sys_get_tick();
    int issued = 0;

    for (uint32_t i = 0; i < SUPERVISOR_SERVICE_MAX; i++) {
        supervisor_recipe_t *r = &runtime->recipes[i];
        if (!runtime->recipe_used[i] || !r->pending_restart || r->killed) {
            continue;
        }
        if (now < r->next_restart_tick) {
            continue;  /* this one's backoff hasn't elapsed yet */
        }
        r->pending_restart = 0;

        int rc = sys_task_restart(r->name, r->entry, r->arg,
                                  (int)r->priority, (int)r->stack_size,
                                  (int)r->cap_rights_mask);
        if (rc < 0) {
            /* Restart failed (resource exhaustion). Re-arm: the timer will
             * retry shortly. Keep pending so arm_timer picks it up. */
            r->pending_restart = 1;
            r->next_restart_tick = now + r->backoff_ticks;
            continue;
        }

        r->restart_count++;
        /* Exponential backoff for the NEXT fault: double, capped. */
        r->backoff_ticks <<= 1;
        if (r->backoff_ticks > SUPERVISOR_BACKOFF_CAP_MS) {
            r->backoff_ticks = SUPERVISOR_BACKOFF_CAP_MS;
        }
        issued++;
    }

    /* If some pending restarts still await a later tick, re-arm the timer. */
    supervisor_arm_timer(runtime, fault_ep_cap);
    return issued;
}

void supervisor_monitor_loop(void *arg) {
    (void)arg;

    /* USER writable state must live in this task's mapped stack. */
    supervisor_runtime_t runtime;
    supervisor_runtime_init(&runtime);

    int ep = sys_fault_subscribe();
    if (ep < 0) {
        /* No fault endpoint — nothing to monitor. Sleep forever. */
        while (1) {
            sys_task_delay(1000);
        }
    }

    /* One-shot, callback-less timer. Its expiry notify is delivered to the
     * SAME fault endpoint (bound below), so fault events and timer events
     * arrive on the single ep we recv on. We distinguish them by badge:
     * fault_event_t.fault_type is 1..4; the timer stamps SUPERVISOR_TIMER_BADGE. */
    runtime.timer_cap = sys_timer_create("sup_timer", NULL, NULL, 1);
    if (runtime.timer_cap >= 0) {
        (void)sys_timer_bind(runtime.timer_cap, ep, (int)SUPERVISOR_TIMER_BADGE);
    }

    /* Phase 2 §2 smoke test: register the crashy_app recipe and launch its
     * first instance. The supervisor owns both the recipe and the initial
     * spawn because the runtime lives on this task's stack (no other task
     * can register into it). When crashy_app faults, on_fault schedules a
     * restart; when the backoff timer fires, do_restarts recreates it via
     * sys_task_restart. After SUPERVISOR_MAX_RESTARTS it is permanently
     * killed and not relaunched. */
#if INIT_PROCESS
    {
        extern void crashy_app_entry(void *arg);
        (void)supervisor_register_recipe(&runtime, "crashy_app",
                                         crashy_app_entry, NULL,
                                         8 /* priority */, 1024 /* stack */,
                                         0x1F /* CAP_FULL; GRANT stripped on restart */);
        int app = sys_task_create("crashy_app", crashy_app_entry, NULL, 8, 1024);
        if (app >= 0) {
            (void)sys_task_start(app);
        }
    }
#endif

    fault_event_t evt;
    while (1) {
        int rc = sys_ep_recv(ep, &evt, (int)KERN_WAIT_FOREVER);
        if (rc < 0) {
            continue;  /* transient error — re-arm the wait */
        }
        if (evt.fault_type == SUPERVISOR_TIMER_BADGE) {
            /* Backoff timer fired: run any due restarts. */
            (void)supervisor_do_restarts(&runtime, ep);
        } else {
            /* A task faulted: classify + schedule its restart. */
            (void)supervisor_on_fault(&runtime, &evt, ep);
        }
    }
}

#endif /* SUPERVISOR && FAULT_ENDPOINT */
