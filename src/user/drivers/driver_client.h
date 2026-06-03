/**
 * @file driver_client.h
 * @brief User-space driver discovery helpers
 */

#ifndef DRIVER_CLIENT_H
#define DRIVER_CLIENT_H

#include "driver_registry.h"
#include "kernel_types.h"
#include <stdint.h>

int driver_lookup_service(int ns_ep_cap, const char *service_name,
                          uint32_t required_ops,
                          uint32_t required_ioctls,
                          uint32_t required_resources,
                          cap_id_t inbox_cap,
                          cap_id_t *out_service_cap,
                          uint32_t timeout);
int driver_lookup_uart(int ns_ep_cap, cap_id_t inbox_cap,
                       cap_id_t *out_service_cap, uint32_t timeout);
int driver_release_service(cap_id_t inbox_cap, cap_id_t service_cap);
int driver_name_server_status(int ns_ep_cap, uint32_t timeout);
const char *driver_error_name(int err);

#endif /* DRIVER_CLIENT_H */
