/**
 * @file driver_runtime.h
 * @brief Runtime binding state for user-space driver discovery
 */

#ifndef DRIVER_RUNTIME_H
#define DRIVER_RUNTIME_H

#include "kernel_types.h"
#include <stdint.h>

typedef enum {
    DRIVER_RUNTIME_NS_UNBOUND = 0,
    DRIVER_RUNTIME_NS_BOUND,
    DRIVER_RUNTIME_NS_LIVE,
} driver_runtime_ns_state_t;

void driver_runtime_clear_name_server(void);
int driver_runtime_bind_name_server(cap_id_t ns_ep_cap);
int driver_runtime_name_server_bound(void);
cap_id_t driver_runtime_name_server_cap(void);
int driver_runtime_name_server_status(uint32_t timeout);
driver_runtime_ns_state_t driver_runtime_name_server_state(uint32_t timeout,
                                                           int *out_status);
void driver_runtime_clear_inbox(void);
int driver_runtime_bind_inbox(cap_id_t inbox_cap);
int driver_runtime_bind_owned_inbox(cap_id_t inbox_cap);
int driver_runtime_inbox_bound(void);
int driver_runtime_inbox_owned(void);
cap_id_t driver_runtime_inbox_cap(void);
int driver_runtime_lookup_ready(uint32_t timeout, const char **out_reason);
int driver_runtime_lookup_service(const char *service_name,
                                  uint32_t required_ops,
                                  uint32_t required_ioctls,
                                  uint32_t required_resources,
                                  cap_id_t inbox_cap,
                                  cap_id_t *out_service_cap,
                                  uint32_t timeout);
int driver_runtime_lookup_uart(cap_id_t inbox_cap,
                               cap_id_t *out_service_cap,
                               uint32_t timeout);

#endif /* DRIVER_RUNTIME_H */
