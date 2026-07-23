/**
 * @file fs_runtime.c
 * @brief Runtime binding state for user-space FS service discovery
 */

#include "fs_runtime.h"
#include "nameserver.h"
#include "user_api.h"

#if CAP_ENABLE

static cap_id_t fs_runtime_ns_ep_cap = KERN_INVALID_ID;
static cap_id_t fs_runtime_inbox_ep_cap = KERN_INVALID_ID;

void fs_runtime_clear_name_server(void) {
    fs_runtime_ns_ep_cap = KERN_INVALID_ID;
}

int fs_runtime_bind_name_server(cap_id_t ns_ep_cap) {
    if (ns_ep_cap <= 0) {
        return KERN_ERR_PARAM;
    }

    fs_runtime_ns_ep_cap = ns_ep_cap;
    return KERN_OK;
}

int fs_runtime_name_server_bound(void) {
    return fs_runtime_ns_ep_cap > 0;
}

cap_id_t fs_runtime_name_server_cap(void) {
    return fs_runtime_ns_ep_cap;
}

int fs_runtime_name_server_status(uint32_t timeout) {
    if (!fs_runtime_name_server_bound()) {
        return KERN_ERR_STATE;
    }

    return nameserver_ping(fs_runtime_ns_ep_cap, timeout);
}

void fs_runtime_clear_inbox(void) {
    fs_runtime_inbox_ep_cap = KERN_INVALID_ID;
}

int fs_runtime_bind_owned_inbox(cap_id_t inbox_cap) {
    if (inbox_cap <= 0) {
        return KERN_ERR_PARAM;
    }

    fs_runtime_inbox_ep_cap = inbox_cap;
    return KERN_OK;
}

int fs_runtime_inbox_bound(void) {
    return fs_runtime_inbox_ep_cap > 0;
}

cap_id_t fs_runtime_inbox_cap(void) {
    return fs_runtime_inbox_ep_cap;
}

int fs_runtime_lookup_ready(uint32_t timeout, const char **out_reason) {
    if (!fs_runtime_name_server_bound()) {
        if (out_reason != NULL) {
            *out_reason = "name-server";
        }
        return KERN_ERR_STATE;
    }

    int status = fs_runtime_name_server_status(timeout);
    if (status != KERN_OK) {
        if (out_reason != NULL) {
            *out_reason = "name-server";
        }
        return status;
    }

    if (!fs_runtime_inbox_bound()) {
        if (out_reason != NULL) {
            *out_reason = "inbox";
        }
        return KERN_ERR_STATE;
    }

    if (out_reason != NULL) {
        *out_reason = "ready";
    }
    return KERN_OK;
}

int fs_runtime_lookup_service(const char *service_name,
                              cap_id_t *out_service_cap,
                              uint32_t timeout) {
    if (out_service_cap == NULL) {
        return KERN_ERR_PARAM;
    }
    *out_service_cap = KERN_INVALID_ID;
    if (service_name == NULL) {
        return KERN_ERR_PARAM;
    }
    if (!fs_runtime_name_server_bound() || !fs_runtime_inbox_bound()) {
        return KERN_ERR_STATE;
    }

    return nameserver_lookup_begin(fs_runtime_ns_ep_cap, service_name,
                                   fs_runtime_inbox_ep_cap, out_service_cap,
                                   timeout);
}

void fs_runtime_release_lookup(void) {
    if (fs_runtime_inbox_bound()) {
        (void)nameserver_lookup_ack(fs_runtime_inbox_ep_cap);
    }
}

int fs_runtime_release_service(cap_id_t service_cap) {
    int err = KERN_ERR_STATE;

    if (fs_runtime_inbox_bound()) {
        err = nameserver_lookup_ack(fs_runtime_inbox_ep_cap);
    }
    if (service_cap > 0) {
        int cap_err = sys_cap_revoke(service_cap);
        if (err == KERN_OK && cap_err != KERN_OK) {
            err = cap_err;
        }
    }
    return err;
}

#endif /* CAP_ENABLE */
