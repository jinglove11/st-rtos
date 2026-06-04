/**
 * @file supervisor.c
 * @brief Minimal reusable service supervisor state
 */

#include "supervisor.h"
#include <string.h>

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
