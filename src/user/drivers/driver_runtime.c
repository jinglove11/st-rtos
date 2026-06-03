/**
 * @file driver_runtime.c
 * @brief Runtime binding state for user-space driver discovery
 */

#include "driver_runtime.h"
#include "driver_client.h"

#if DRIVER_ENABLE && CAP_ENABLE

static cap_id_t driver_runtime_ns_ep_cap = KERN_INVALID_ID;
static cap_id_t driver_runtime_inbox_ep_cap = KERN_INVALID_ID;
static uint8_t driver_runtime_inbox_ep_owned = 0;

void driver_runtime_clear_name_server(void) {
    driver_runtime_ns_ep_cap = KERN_INVALID_ID;
}

int driver_runtime_bind_name_server(cap_id_t ns_ep_cap) {
    if (ns_ep_cap <= 0) {
        return KERN_ERR_PARAM;
    }

    driver_runtime_ns_ep_cap = ns_ep_cap;
    return KERN_OK;
}

int driver_runtime_name_server_bound(void) {
    return driver_runtime_ns_ep_cap > 0;
}

cap_id_t driver_runtime_name_server_cap(void) {
    return driver_runtime_ns_ep_cap;
}

int driver_runtime_name_server_status(uint32_t timeout) {
    if (!driver_runtime_name_server_bound()) {
        return KERN_ERR_STATE;
    }

    return driver_name_server_status(driver_runtime_ns_ep_cap, timeout);
}

driver_runtime_ns_state_t driver_runtime_name_server_state(uint32_t timeout,
                                                           int *out_status) {
    int status;

    if (!driver_runtime_name_server_bound()) {
        if (out_status != NULL) {
            *out_status = KERN_ERR_STATE;
        }
        return DRIVER_RUNTIME_NS_UNBOUND;
    }

    status = driver_name_server_status(driver_runtime_ns_ep_cap, timeout);
    if (out_status != NULL) {
        *out_status = status;
    }
    if (status == KERN_OK) {
        return DRIVER_RUNTIME_NS_LIVE;
    }
    return DRIVER_RUNTIME_NS_BOUND;
}

void driver_runtime_clear_inbox(void) {
    driver_runtime_inbox_ep_cap = KERN_INVALID_ID;
    driver_runtime_inbox_ep_owned = 0;
}

int driver_runtime_bind_inbox(cap_id_t inbox_cap) {
    if (inbox_cap <= 0) {
        return KERN_ERR_PARAM;
    }

    driver_runtime_inbox_ep_cap = inbox_cap;
    driver_runtime_inbox_ep_owned = 0;
    return KERN_OK;
}

int driver_runtime_bind_owned_inbox(cap_id_t inbox_cap) {
    if (inbox_cap <= 0) {
        return KERN_ERR_PARAM;
    }

    driver_runtime_inbox_ep_cap = inbox_cap;
    driver_runtime_inbox_ep_owned = 1;
    return KERN_OK;
}

int driver_runtime_inbox_bound(void) {
    return driver_runtime_inbox_ep_cap > 0;
}

int driver_runtime_inbox_owned(void) {
    return driver_runtime_inbox_bound() && driver_runtime_inbox_ep_owned;
}

cap_id_t driver_runtime_inbox_cap(void) {
    return driver_runtime_inbox_ep_cap;
}

int driver_runtime_lookup_ready(uint32_t timeout, const char **out_reason) {
    int status = KERN_OK;
    driver_runtime_ns_state_t state =
        driver_runtime_name_server_state(timeout, &status);

    if (state == DRIVER_RUNTIME_NS_UNBOUND) {
        if (out_reason != NULL) {
            *out_reason = "name-server";
        }
        return KERN_ERR_STATE;
    }
    if (state != DRIVER_RUNTIME_NS_LIVE) {
        if (out_reason != NULL) {
            *out_reason = "name-server";
        }
        return status;
    }
    if (!driver_runtime_inbox_bound()) {
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

int driver_runtime_lookup_service(const char *service_name,
                                  uint32_t required_ops,
                                  uint32_t required_ioctls,
                                  uint32_t required_resources,
                                  cap_id_t inbox_cap,
                                  cap_id_t *out_service_cap,
                                  uint32_t timeout) {
    (void)inbox_cap;

    if (out_service_cap == NULL) {
        return KERN_ERR_PARAM;
    }
    *out_service_cap = KERN_INVALID_ID;
    if (!driver_runtime_name_server_bound()) {
        return KERN_ERR_STATE;
    }
    if (!driver_runtime_inbox_bound()) {
        return KERN_ERR_STATE;
    }

    return driver_lookup_service(driver_runtime_ns_ep_cap, service_name,
                                 required_ops, required_ioctls,
                                 required_resources, driver_runtime_inbox_ep_cap,
                                 out_service_cap, timeout);
}

int driver_runtime_lookup_uart(cap_id_t inbox_cap,
                               cap_id_t *out_service_cap,
                               uint32_t timeout) {
    (void)inbox_cap;

    if (out_service_cap == NULL) {
        return KERN_ERR_PARAM;
    }
    *out_service_cap = KERN_INVALID_ID;
    if (!driver_runtime_name_server_bound()) {
        return KERN_ERR_STATE;
    }
    if (!driver_runtime_inbox_bound()) {
        return KERN_ERR_STATE;
    }

    return driver_lookup_uart(driver_runtime_ns_ep_cap,
                              driver_runtime_inbox_ep_cap,
                              out_service_cap, timeout);
}

#endif /* DRIVER_ENABLE && CAP_ENABLE */
