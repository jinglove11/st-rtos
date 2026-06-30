/**
 * @file nameserver.c
 * @brief Minimal user-space name-server service loop
 */

#include "nameserver.h"
#include "user_api.h"

#if CAP_ENABLE

static void ns_zero(void *ptr, uint32_t len) {
    uint8_t *p = (uint8_t *)ptr;
    for (uint32_t i = 0; i < len; i++) {
        p[i] = 0;
    }
}

static void ns_copy_name(char *dst, const char *src) {
    for (uint32_t i = 0; i < NS_NAME_MAX; i++) {
        dst[i] = src[i];
        if (src[i] == '\0') {
            return;
        }
    }
    dst[NS_NAME_MAX - 1U] = '\0';
}

static int ns_name_valid(const char *name) {
    if (name == NULL || name[0] == '\0') {
        return 0;
    }

    for (uint32_t i = 0; i < NS_NAME_MAX; i++) {
        if (name[i] == '\0') {
            return 1;
        }
    }

    return 0;
}

static int ns_name_eq(const char *a, const char *b) {
    for (uint32_t i = 0; i < NS_NAME_MAX; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
        if (a[i] == '\0') {
            return 1;
        }
    }
    return 1;
}

static ns_entry_t *ns_find(ns_entry_t *entries, const char *name) {
    for (uint32_t i = 0; i < NS_REGISTRY_MAX; i++) {
        if (entries[i].in_use && ns_name_eq(entries[i].name, name)) {
            return &entries[i];
        }
    }
    return NULL;
}

static ns_entry_t *ns_free_entry(ns_entry_t *entries) {
    for (uint32_t i = 0; i < NS_REGISTRY_MAX; i++) {
        if (!entries[i].in_use) {
            return &entries[i];
        }
    }
    return NULL;
}

static void ns_release_caps(cap_id_t *caps, uint8_t count) {
    if (caps == NULL) {
        return;
    }

    for (uint8_t i = 0; i < count && i < IPC_CAPS_MAX; i++) {
        if (caps[i] > 0) {
            (void)sys_cap_revoke(caps[i]);
            caps[i] = KERN_INVALID_ID;
        }
    }
}

static void ns_clear_entry(ns_entry_t *entry) {
    if (entry == NULL) {
        return;
    }
    if (entry->in_use && entry->endpoint_cap > 0) {
        (void)sys_cap_revoke(entry->endpoint_cap);
    }
    ns_zero(entry, sizeof(*entry));
}

static int ns_reply_status(int ep_cap, ns_name_msg_t *msg, int status) {
    msg->hdr.status = status;
    return sys_ep_reply(ep_cap, msg);
}

static void ns_init_xfers(ipc_cap_xfer_t *xfers) {
    for (uint32_t i = 0; i < IPC_CAPS_MAX; i++) {
        xfers[i].src_cap = KERN_INVALID_ID;
        xfers[i].rights = 0;
        xfers[i].flags = IPC_CAP_COPY;
    }
}

static void ns_init_caps(cap_id_t *caps) {
    for (uint32_t i = 0; i < IPC_CAPS_MAX; i++) {
        caps[i] = KERN_INVALID_ID;
    }
}

static int ns_client_prepare_msg(ns_name_msg_t *msg, uint16_t opcode,
                                 uint32_t seq, const char *name,
                                 uint32_t owner_badge) {
    if (msg == NULL) {
        return KERN_ERR_PARAM;
    }

    ns_zero(msg, sizeof(*msg));
    ns_msg_init(&msg->hdr, opcode, seq);
    msg->owner_badge = owner_badge;
    if (opcode != NS_OP_PING && !ns_name_valid(name)) {
        return KERN_ERR_PARAM;
    }
    if (name != NULL) {
        ns_copy_name(msg->name, name);
    }
    return KERN_OK;
}

int nameserver_ping(int ns_ep_cap, uint32_t timeout) {
    uint8_t msg_buf[KERN_EP_MSG_SIZE];
    ns_name_msg_t *msg = (ns_name_msg_t *)msg_buf;

    if (ns_ep_cap <= 0) {
        return KERN_ERR_PARAM;
    }

    int err = ns_client_prepare_msg(msg, NS_OP_PING, 1, NULL, 0);
    if (err != KERN_OK) {
        return err;
    }

    err = sys_ep_send(ns_ep_cap, msg_buf, (int)timeout);
    if (err != KERN_OK) {
        return err;
    }
    if (msg->hdr.magic != NS_MAGIC || msg->hdr.opcode != NS_OP_PING) {
        return KERN_ERR_STATE;
    }
    return msg->hdr.status;
}

int nameserver_register(int ns_ep_cap, const char *name,
                        cap_id_t service_ep_cap, uint32_t owner_badge,
                        uint32_t timeout) {
    uint8_t msg_buf[KERN_EP_MSG_SIZE];
    ns_name_msg_t *msg = (ns_name_msg_t *)msg_buf;
    ipc_cap_xfer_t xfers[IPC_CAPS_MAX];

    if (ns_ep_cap <= 0 || service_ep_cap <= 0) {
        return KERN_ERR_PARAM;
    }

    int err = ns_client_prepare_msg(msg, NS_OP_REGISTER, 2, name,
                                    owner_badge);
    if (err != KERN_OK) {
        return err;
    }
    ns_init_xfers(xfers);
    xfers[0].src_cap = service_ep_cap;
    xfers[0].rights = CAP_READ | CAP_WRITE | CAP_TRANSFER;
    xfers[0].flags = IPC_CAP_COPY;

    err = sys_ep_send_caps(ns_ep_cap, msg_buf, xfers, 1, (int)timeout);
    if (err != KERN_OK) {
        return err;
    }
    if (msg->hdr.magic != NS_MAGIC || msg->hdr.opcode != NS_OP_REGISTER) {
        return KERN_ERR_STATE;
    }
    return msg->hdr.status;
}

int nameserver_unregister(int ns_ep_cap, const char *name,
                          uint32_t owner_badge, uint32_t timeout) {
    uint8_t msg_buf[KERN_EP_MSG_SIZE];
    ns_name_msg_t *msg = (ns_name_msg_t *)msg_buf;

    if (ns_ep_cap <= 0) {
        return KERN_ERR_PARAM;
    }

    int err = ns_client_prepare_msg(msg, NS_OP_UNREG, 3, name, owner_badge);
    if (err != KERN_OK) {
        return err;
    }

    err = sys_ep_send(ns_ep_cap, msg_buf, (int)timeout);
    if (err != KERN_OK) {
        return err;
    }
    if (msg->hdr.magic != NS_MAGIC || msg->hdr.opcode != NS_OP_UNREG) {
        return KERN_ERR_STATE;
    }
    return msg->hdr.status;
}

int nameserver_lookup_begin(int ns_ep_cap, const char *name,
                            cap_id_t inbox_cap, cap_id_t *out_service_cap,
                            uint32_t timeout) {
    uint8_t msg_buf[KERN_EP_MSG_SIZE];
    ns_name_msg_t *msg = (ns_name_msg_t *)msg_buf;
    ipc_cap_xfer_t xfers[IPC_CAPS_MAX];
    cap_id_t caps[IPC_CAPS_MAX];
    uint8_t cap_count = 0;

    if (ns_ep_cap <= 0 || inbox_cap <= 0 || out_service_cap == NULL) {
        return KERN_ERR_PARAM;
    }
    *out_service_cap = KERN_INVALID_ID;

    int err = ns_client_prepare_msg(msg, NS_OP_LOOKUP, 4, name, 0);
    if (err != KERN_OK) {
        return err;
    }
    ns_init_xfers(xfers);
    ns_init_caps(caps);
    xfers[0].src_cap = inbox_cap;
    xfers[0].rights = CAP_READ | CAP_WRITE;
    xfers[0].flags = IPC_CAP_COPY;

    err = sys_ep_send_caps(ns_ep_cap, msg_buf, xfers, 1, (int)timeout);
    if (err != KERN_OK) {
        return err;
    }
    if (msg->hdr.magic != NS_MAGIC || msg->hdr.opcode != NS_OP_LOOKUP) {
        return KERN_ERR_STATE;
    }
    if (msg->hdr.status != KERN_OK) {
        return msg->hdr.status;
    }

    ns_zero(msg_buf, sizeof(msg_buf));
    err = sys_ep_recv_caps(inbox_cap, msg_buf, caps, &cap_count,
                           (int)timeout);
    if (err != KERN_OK) {
        return err;
    }
    if (cap_count != 1 || caps[0] <= 0) {
        return KERN_ERR_CAP;
    }

    *out_service_cap = caps[0];
    return KERN_OK;
}

int nameserver_lookup_ack(int inbox_cap) {
    uint8_t msg_buf[KERN_EP_MSG_SIZE];

    if (inbox_cap <= 0) {
        return KERN_ERR_PARAM;
    }
    ns_zero(msg_buf, sizeof(msg_buf));
    return sys_ep_reply(inbox_cap, msg_buf);
}

int nameserver_service_run(int ep_cap, uint32_t max_requests) {
    ns_entry_t entries[NS_REGISTRY_MAX];
    uint8_t msg_buf[KERN_EP_MSG_SIZE];
    ns_name_msg_t *msg = (ns_name_msg_t *)msg_buf;
    cap_id_t caps[IPC_CAPS_MAX];
    ipc_cap_xfer_t xfers[IPC_CAPS_MAX];
    uint8_t cap_count = 0;
    int err = KERN_OK;

    if (ep_cap <= 0) {
        return KERN_ERR_PARAM;
    }

    ns_zero(entries, sizeof(entries));

    for (uint32_t round = 0;
         (max_requests == 0U || round < max_requests) && err == KERN_OK;
         round++) {
        ns_zero(msg_buf, sizeof(msg_buf));
        for (uint32_t i = 0; i < IPC_CAPS_MAX; i++) {
            caps[i] = KERN_INVALID_ID;
            xfers[i].src_cap = KERN_INVALID_ID;
            xfers[i].rights = 0;
            xfers[i].flags = IPC_CAP_COPY;
        }
        cap_count = 0;

        err = sys_ep_recv_caps(ep_cap, msg_buf, caps, &cap_count, 1000);
        if (err == KERN_ERR_TIMEOUT && max_requests == 0U) {
            err = KERN_OK;
            continue;
        }
        if (err != KERN_OK) {
            break;
        }

        if (msg->hdr.magic != NS_MAGIC || !ns_opcode_valid(msg->hdr.opcode)) {
            ns_release_caps(caps, cap_count);
            err = ns_reply_status(ep_cap, msg, KERN_ERR_PARAM);
            continue;
        }

        if (msg->hdr.opcode == NS_OP_PING) {
            ns_release_caps(caps, cap_count);
            err = ns_reply_status(ep_cap, msg, KERN_OK);
            continue;
        }

        if (!ns_name_valid(msg->name)) {
            ns_release_caps(caps, cap_count);
            err = ns_reply_status(ep_cap, msg, KERN_ERR_PARAM);
            continue;
        }

        if (msg->hdr.opcode == NS_OP_REGISTER) {
            ns_entry_t *entry = ns_find(entries, msg->name);
            if (entry != NULL) {
                ns_release_caps(caps, cap_count);
                err = ns_reply_status(ep_cap, msg, KERN_ERR_BUSY);
            } else if (cap_count != 1 || caps[0] <= 0) {
                ns_release_caps(caps, cap_count);
                err = ns_reply_status(ep_cap, msg, KERN_ERR_CAP);
            } else {
                entry = ns_free_entry(entries);
                if (entry == NULL) {
                    ns_release_caps(caps, cap_count);
                    err = ns_reply_status(ep_cap, msg, KERN_ERR_RESOURCE);
                } else {
                    entry->in_use = 1;
                    ns_copy_name(entry->name, msg->name);
                    entry->endpoint_cap = caps[0];
                    entry->rights = CAP_READ | CAP_WRITE;
                    entry->owner_badge = msg->owner_badge;
                    err = ns_reply_status(ep_cap, msg, KERN_OK);
                }
            }
        } else if (msg->hdr.opcode == NS_OP_LOOKUP) {
            ns_entry_t *entry = ns_find(entries, msg->name);
            if (entry == NULL) {
                ns_release_caps(caps, cap_count);
                err = ns_reply_status(ep_cap, msg, KERN_ERR_NOEXIST);
            } else if (cap_count != 1 || caps[0] <= 0) {
                ns_release_caps(caps, cap_count);
                err = ns_reply_status(ep_cap, msg, KERN_ERR_CAP);
            } else {
                err = ns_reply_status(ep_cap, msg, KERN_OK);
                if (err == KERN_OK) {
                    ns_msg_init(&msg->hdr, NS_OP_LOOKUP, msg->hdr.seq);
                    msg->hdr.status = KERN_OK;
                    xfers[0].src_cap = entry->endpoint_cap;
                    xfers[0].rights = entry->rights;
                    xfers[0].flags = IPC_CAP_COPY;
                    err = sys_ep_send_caps(caps[0], msg_buf, xfers, 1, 1000);
                }
                ns_release_caps(caps, cap_count);
            }
        } else if (msg->hdr.opcode == NS_OP_UNREG) {
            ns_entry_t *entry = ns_find(entries, msg->name);
            if (entry == NULL) {
                ns_release_caps(caps, cap_count);
                err = ns_reply_status(ep_cap, msg, KERN_ERR_NOEXIST);
            } else if (entry->owner_badge != msg->owner_badge) {
                ns_release_caps(caps, cap_count);
                err = ns_reply_status(ep_cap, msg, KERN_ERR_PERM);
            } else {
                ns_release_caps(caps, cap_count);
                ns_clear_entry(entry);
                err = ns_reply_status(ep_cap, msg, KERN_OK);
            }
        } else {
            ns_release_caps(caps, cap_count);
            err = ns_reply_status(ep_cap, msg, KERN_ERR_PARAM);
        }
    }

    for (uint32_t i = 0; i < NS_REGISTRY_MAX; i++) {
        ns_clear_entry(&entries[i]);
    }

    return err;
}

#endif /* CAP_ENABLE */
