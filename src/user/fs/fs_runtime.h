/**
 * @file fs_runtime.h
 * @brief Runtime binding state for user-space FS service discovery
 */

#ifndef FS_RUNTIME_H
#define FS_RUNTIME_H

#include "kernel_types.h"
#include <stdint.h>

void fs_runtime_clear_name_server(void);
int fs_runtime_bind_name_server(cap_id_t ns_ep_cap);
int fs_runtime_name_server_bound(void);
cap_id_t fs_runtime_name_server_cap(void);
int fs_runtime_name_server_status(uint32_t timeout);

void fs_runtime_clear_inbox(void);
int fs_runtime_bind_owned_inbox(cap_id_t inbox_cap);
int fs_runtime_inbox_bound(void);
cap_id_t fs_runtime_inbox_cap(void);

int fs_runtime_lookup_ready(uint32_t timeout, const char **out_reason);
int fs_runtime_lookup_service(const char *service_name,
                              cap_id_t *out_service_cap,
                              uint32_t timeout);
void fs_runtime_release_lookup(void);
int fs_runtime_release_service(cap_id_t service_cap);

#endif /* FS_RUNTIME_H */
