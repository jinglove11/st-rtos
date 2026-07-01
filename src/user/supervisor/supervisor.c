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

static supervisor_recipe_t recipes[SUPERVISOR_SERVICE_MAX];
static uint8_t recipe_used[SUPERVISOR_SERVICE_MAX];

int supervisor_register_recipe(const char *name,
                               void (*entry)(void *),
                               void *arg,
                               uint8_t priority,
                               uint32_t stack_size,
                               uint8_t cap_rights_mask) {
    if (name == NULL || entry == NULL) {
        return KERN_ERR_PARAM;
    }
    /* Idempotent: refresh existing recipe. */
    for (uint32_t i = 0; i < SUPERVISOR_SERVICE_MAX; i++) {
        if (recipe_used[i] && strcmp(recipes[i].name, name) == 0) {
            recipes[i].entry = entry;
            recipes[i].arg = arg;
            recipes[i].priority = priority;
            recipes[i].stack_size = stack_size;
            recipes[i].cap_rights_mask = cap_rights_mask;
            /* keep rate-limit state across re-registration */
            return KERN_OK;
        }
    }
    for (uint32_t i = 0; i < SUPERVISOR_SERVICE_MAX; i++) {
        if (!recipe_used[i]) {
            recipes[i].name = name;
            recipes[i].entry = entry;
            recipes[i].arg = arg;
            recipes[i].priority = priority;
            recipes[i].stack_size = stack_size;
            recipes[i].cap_rights_mask = cap_rights_mask;
            recipes[i].restart_count = 0;
            recipes[i].last_restart_tick = 0;
            recipes[i].backoff_ticks = SUPERVISOR_BACKOFF_BASE_MS;
            recipes[i].killed = 0;
            recipe_used[i] = 1;
            return KERN_OK;
        }
    }
    return KERN_ERR_RESOURCE;
}

supervisor_recipe_t *supervisor_find_recipe(const char *name) {
    if (name == NULL) {
        return NULL;
    }
    for (uint32_t i = 0; i < SUPERVISOR_SERVICE_MAX; i++) {
        if (recipe_used[i] && strcmp(recipes[i].name, name) == 0) {
            return &recipes[i];
        }
    }
    return NULL;
}

/* Caller passes a fault_event_t* (typed as const void* so the header doesn't
 * need to pull fault_endpoint.h for the prototype). */
int supervisor_handle_fault(const void *event) {
    const fault_event_t *evt = (const fault_event_t *)event;
    if (evt == NULL) {
        return 0;
    }

    supervisor_recipe_t *r = supervisor_find_recipe(evt->task_name);
    if (r == NULL) {
        /* Unknown task — not our responsibility. Log nothing (no printf path
         * yet); just record for the global fault counter via the registry. */
        return 0;
    }

    /* Already permanently killed — ignore further faults. */
    if (r->killed) {
        return 0;
    }

    /* Rate limit: at most one restart per RATE_WINDOW_MS. If within the
     * window, ignore (the task already faulted again too fast — let the
     * backoff elapse). */
    uint32_t now = evt->tick;
    uint32_t elapsed = now - r->last_restart_tick;
    if (r->restart_count > 0 && elapsed < SUPERVISOR_RATE_WINDOW_MS) {
        return 0;
    }

    /* Max restarts exceeded → permanent kill. */
    if (r->restart_count >= SUPERVISOR_MAX_RESTARTS) {
        r->killed = 1;
        return 0;
    }

    /* Honor exponential backoff before restarting. */
    if (r->restart_count > 0 && elapsed < r->backoff_ticks) {
        return 0;
    }

    /* Issue the restart via the §2.4 syscall (reduced cap, GRANT stripped). */
    int rc = sys_task_restart(r->name, r->entry, r->arg,
                              (int)r->priority, (int)r->stack_size,
                              (int)r->cap_rights_mask);
    if (rc < 0) {
        /* Restart failed (resource exhaustion, etc.) — leave it; will retry
         * on the next fault event. */
        return 0;
    }

    r->restart_count++;
    r->last_restart_tick = now;
    /* Exponential backoff: double, capped. */
    r->backoff_ticks <<= 1;
    if (r->backoff_ticks > SUPERVISOR_BACKOFF_CAP_MS) {
        r->backoff_ticks = SUPERVISOR_BACKOFF_CAP_MS;
    }
    return 1;
}

void supervisor_monitor_loop(void *arg) {
    (void)arg;

    int ep = sys_fault_subscribe();
    if (ep < 0) {
        /* No fault endpoint — nothing to monitor. Sleep forever. */
        while (1) {
            sys_task_delay(1000);
        }
    }

    fault_event_t evt;
    while (1) {
        /* Block up to 1s; -1 (forever) would also work but a bounded timeout
         * keeps the loop responsive to recipe changes. */
        int rc = sys_ep_recv(ep, &evt, 1000);
        if (rc < 0) {
            continue;  /* timeout or transient error */
        }
        (void)supervisor_handle_fault(&evt);
    }
}

#endif /* SUPERVISOR && FAULT_ENDPOINT */
