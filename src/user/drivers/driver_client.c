/**
 * @file driver_client.c
 * @brief User-space driver discovery helpers
 */

#include "driver_client.h"
#include "nameserver.h"

#if DRIVER_ENABLE && CAP_ENABLE

int driver_lookup_service(int ns_ep_cap, const char *service_name,
                          uint32_t required_ops,
                          uint32_t required_ioctls,
                          uint32_t required_resources,
                          cap_id_t inbox_cap,
                          cap_id_t *out_service_cap,
                          uint32_t timeout) {
    const driver_descriptor_t *desc = NULL;
    int err;

    if (out_service_cap == NULL) {
        return KERN_ERR_PARAM;
    }
    *out_service_cap = KERN_INVALID_ID;
    if (ns_ep_cap <= 0 || inbox_cap <= 0) {
        return KERN_ERR_PARAM;
    }

    err = driver_registry_query(service_name, &desc);
    if (err != KERN_OK) {
        return err;
    }
    err = driver_registry_validate_desc(desc);
    if (err != KERN_OK) {
        return err;
    }
    if (!driver_descriptor_supports(desc, required_ops, required_ioctls,
                                    required_resources)) {
        return KERN_ERR_NOEXIST;
    }

    return nameserver_lookup_begin(ns_ep_cap, desc->service_name, inbox_cap,
                                   out_service_cap, timeout);
}

int driver_lookup_uart(int ns_ep_cap, cap_id_t inbox_cap,
                       cap_id_t *out_service_cap, uint32_t timeout) {
    const uint32_t ops = DRIVER_OP_BIT_PING | DRIVER_OP_BIT_OPEN |
                         DRIVER_OP_BIT_CLOSE | DRIVER_OP_BIT_READ |
                         DRIVER_OP_BIT_WRITE | DRIVER_OP_BIT_IOCTL |
                         DRIVER_OP_BIT_POLL;

    return driver_lookup_service(ns_ep_cap, "dev.uart0", ops,
                                 DRIVER_IOCTL_BIT_GET_EVENTS,
                                 DRV_RESOURCE_BIT_MMIO,
                                 inbox_cap, out_service_cap, timeout);
}

int driver_release_service(cap_id_t inbox_cap, cap_id_t service_cap) {
    if (inbox_cap <= 0) {
        return KERN_ERR_PARAM;
    }

    (void)service_cap;
    return nameserver_lookup_ack(inbox_cap);
}

int driver_name_server_status(int ns_ep_cap, uint32_t timeout) {
    if (ns_ep_cap <= 0) {
        return KERN_ERR_STATE;
    }
    return nameserver_ping(ns_ep_cap, timeout);
}

const char *driver_error_name(int err) {
    switch (err) {
    case KERN_OK:
        return "ok";
    case KERN_ERR_PARAM:
        return "param";
    case KERN_ERR_TIMEOUT:
        return "timeout";
    case KERN_ERR_RESOURCE:
        return "resource";
    case KERN_ERR_STATE:
        return "state";
    case KERN_ERR_CAP:
        return "cap";
    case KERN_ERR_BUSY:
        return "busy";
    case KERN_ERR_NOEXIST:
        return "noexist";
    case KERN_ERR_OVERFLOW:
        return "overflow";
    case KERN_ERR_PERM:
        return "perm";
    case KERN_ERR_FAULT:
        return "fault";
    default:
        return "unknown";
    }
}

#endif /* DRIVER_ENABLE && CAP_ENABLE */
