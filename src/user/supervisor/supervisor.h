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
    uint32_t pending_client_count;
    supervisor_restart_policy_t restart_policy;
    uint32_t max_restarts;
    int last_health;
} supervisor_service_t;

#define SUPERVISOR_SERVICE_INIT(initial_health) \
    { NULL, 0U, 0U, 0U, SUPERVISOR_RESTART_MANUAL, 0U, (initial_health) }
#define SUPERVISOR_SERVICE_NAMED_INIT(name, initial_health) \
    { (name), 0U, 0U, 0U, SUPERVISOR_RESTART_MANUAL, 0U, (initial_health) }

void supervisor_service_init(supervisor_service_t *svc, int initial_health);
void supervisor_service_init_named(supervisor_service_t *svc,
                                   const char *service_name,
                                   int initial_health);
void supervisor_set_service_name(supervisor_service_t *svc,
                                 const char *service_name);
void supervisor_record_restart(supervisor_service_t *svc);
void supervisor_record_recover(supervisor_service_t *svc);
void supervisor_set_health(supervisor_service_t *svc, int health);
void supervisor_set_pending_clients(supervisor_service_t *svc, uint32_t count);
void supervisor_set_restart_policy(supervisor_service_t *svc,
                                   supervisor_restart_policy_t policy,
                                   uint32_t max_restarts);
void supervisor_reset_service(supervisor_service_t *svc, int initial_health);
void supervisor_client_blocked(supervisor_service_t *svc);
void supervisor_client_unblocked(supervisor_service_t *svc);
uint32_t supervisor_restart_count(const supervisor_service_t *svc);
uint32_t supervisor_recover_count(const supervisor_service_t *svc);
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

#endif /* USER_SUPERVISOR_H */
