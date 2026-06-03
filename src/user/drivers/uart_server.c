/**
 * @file uart_server.c
 * @brief Minimal user-space UART driver server prototype
 */

#include "driver_proto.h"
#include "user_api.h"
#include "capability.h"

#if DRIVER_ENABLE && CAP_ENABLE

static void drv_zero(void *ptr, uint32_t len) {
    uint8_t *p = (uint8_t *)ptr;
    for (uint32_t i = 0; i < len; i++) {
        p[i] = 0;
    }
}

int driver_opcode_valid(uint16_t opcode) {
    return opcode >= DRV_OP_PING && opcode <= DRV_OP_DETACH;
}

void driver_msg_init(drv_msg_t *msg, uint16_t opcode, uint32_t seq) {
    if (msg == NULL) {
        return;
    }
    drv_zero(msg, sizeof(*msg));
    msg->magic = DRV_MAGIC;
    msg->opcode = opcode;
    msg->flags = DRV_FLAG_NONE;
    msg->seq = seq;
    msg->status = 0;
    msg->result = 0;
    msg->length = 0;
    msg->command = 0;
}

static int uart_server_reply(int ep_cap, drv_msg_t *msg, int status,
                             int result) {
    msg->status = status;
    msg->result = result;
    return sys_ep_reply(ep_cap, msg);
}

static int driver_check_reply(const drv_msg_t *msg, uint16_t opcode) {
    if (msg->magic != DRV_MAGIC || msg->opcode != opcode) {
        return KERN_ERR_STATE;
    }
    return msg->status;
}

static void driver_release_caps(cap_id_t *caps, uint8_t count) {
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

int driver_ping(int ep_cap, uint32_t timeout) {
    drv_msg_t msg;
    int err;

    if (ep_cap <= 0) {
        return KERN_ERR_PARAM;
    }

    driver_msg_init(&msg, DRV_OP_PING, 0);
    err = sys_ep_send(ep_cap, &msg, (int)timeout);
    if (err != KERN_OK) {
        return err;
    }

    return driver_check_reply(&msg, DRV_OP_PING);
}

int driver_open(int ep_cap, uint32_t flags, uint32_t timeout) {
    drv_msg_t msg;
    int err;

    if (ep_cap <= 0) {
        return KERN_ERR_PARAM;
    }

    driver_msg_init(&msg, DRV_OP_OPEN, 0);
    msg.flags = (uint16_t)flags;
    err = sys_ep_send(ep_cap, &msg, (int)timeout);
    if (err != KERN_OK) {
        return err;
    }

    return driver_check_reply(&msg, DRV_OP_OPEN);
}

int driver_close(int ep_cap, uint32_t timeout) {
    drv_msg_t msg;
    int err;

    if (ep_cap <= 0) {
        return KERN_ERR_PARAM;
    }

    driver_msg_init(&msg, DRV_OP_CLOSE, 0);
    err = sys_ep_send(ep_cap, &msg, (int)timeout);
    if (err != KERN_OK) {
        return err;
    }

    return driver_check_reply(&msg, DRV_OP_CLOSE);
}

int driver_read(int ep_cap, void *buf, uint32_t len, uint32_t timeout) {
    drv_msg_t msg;
    uint8_t *dst = (uint8_t *)buf;
    uint32_t got;
    int err;

    if (ep_cap <= 0 || (buf == NULL && len > 0U) || len > DRV_PAYLOAD_MAX) {
        return KERN_ERR_PARAM;
    }
    if (buf != NULL) {
        for (uint32_t i = 0; i < len; i++) {
            dst[i] = 0;
        }
    }

    driver_msg_init(&msg, DRV_OP_READ, 0);
    msg.length = len;
    err = sys_ep_send(ep_cap, &msg, (int)timeout);
    if (err != KERN_OK) {
        return err;
    }

    err = driver_check_reply(&msg, DRV_OP_READ);
    if (err != KERN_OK) {
        return err;
    }

    got = (uint32_t)msg.result;
    if (got > len || got > DRV_PAYLOAD_MAX) {
        return KERN_ERR_OVERFLOW;
    }
    for (uint32_t i = 0; i < got; i++) {
        dst[i] = msg.payload[i];
    }

    return msg.result;
}

int driver_write(int ep_cap, const void *buf, uint32_t len, uint32_t timeout) {
    drv_msg_t msg;
    const uint8_t *src = (const uint8_t *)buf;
    int err;

    if (ep_cap <= 0 || (buf == NULL && len > 0U) || len > DRV_PAYLOAD_MAX) {
        return KERN_ERR_PARAM;
    }

    driver_msg_init(&msg, DRV_OP_WRITE, 0);
    msg.length = len;
    for (uint32_t i = 0; i < len; i++) {
        msg.payload[i] = src[i];
    }

    err = sys_ep_send(ep_cap, &msg, (int)timeout);
    if (err != KERN_OK) {
        return err;
    }

    err = driver_check_reply(&msg, DRV_OP_WRITE);
    if (err != KERN_OK) {
        return err;
    }

    return msg.result;
}

int driver_poll(int ep_cap, uint32_t *out_events, uint32_t timeout) {
    drv_msg_t msg;
    int err;

    if (out_events != NULL) {
        *out_events = 0;
    }
    if (ep_cap <= 0 || out_events == NULL) {
        return KERN_ERR_PARAM;
    }

    driver_msg_init(&msg, DRV_OP_POLL, 0);
    err = sys_ep_send(ep_cap, &msg, (int)timeout);
    if (err != KERN_OK) {
        return err;
    }

    err = driver_check_reply(&msg, DRV_OP_POLL);
    if (err != KERN_OK) {
        return err;
    }

    *out_events = (uint32_t)msg.result;
    return KERN_OK;
}

int driver_ioctl(int ep_cap, uint32_t command, uint32_t *out_value,
                 uint32_t timeout) {
    drv_msg_t msg;
    int err;

    if (out_value != NULL) {
        *out_value = 0;
    }
    if (ep_cap <= 0 || out_value == NULL) {
        return KERN_ERR_PARAM;
    }

    driver_msg_init(&msg, DRV_OP_IOCTL, 0);
    msg.command = command;
    err = sys_ep_send(ep_cap, &msg, (int)timeout);
    if (err != KERN_OK) {
        return err;
    }

    err = driver_check_reply(&msg, DRV_OP_IOCTL);
    if (err != KERN_OK) {
        return err;
    }

    *out_value = (uint32_t)msg.result;
    return KERN_OK;
}

int driver_get_events(int ep_cap, uint32_t *out_events, uint32_t timeout) {
    return driver_ioctl(ep_cap, DRV_IOCTL_GET_EVENTS, out_events, timeout);
}

int driver_get_resources(int ep_cap, uint32_t *out_resources,
                         uint32_t timeout) {
    return driver_ioctl(ep_cap, DRV_IOCTL_GET_RESOURCES, out_resources,
                        timeout);
}

int driver_get_status(int ep_cap, uint32_t *out_status, uint32_t timeout) {
    return driver_ioctl(ep_cap, DRV_IOCTL_GET_STATUS, out_status, timeout);
}

int driver_clear_status(int ep_cap, uint32_t timeout) {
    uint32_t ignored = 0;

    return driver_ioctl(ep_cap, DRV_IOCTL_CLEAR_STATUS, &ignored, timeout);
}

static int driver_resource_type_valid(uint32_t resource_type) {
    return resource_type == DRV_RESOURCE_MMIO ||
           resource_type == DRV_RESOURCE_IRQ;
}

static uint32_t uart_server_resource_bits(int mmio_attached,
                                          int irq_attached) {
    uint32_t bits = 0;

    if (mmio_attached) {
        bits |= DRV_RESOURCE_BIT_MMIO;
    }
    if (irq_attached) {
        bits |= DRV_RESOURCE_BIT_IRQ;
    }

    return bits;
}

static uint32_t uart_server_status_bits(int opened, int mmio_attached,
                                        int irq_attached, int irq_pending,
                                        int error_seen) {
    uint32_t bits = 0;

    if (opened) {
        bits |= DRV_STATUS_OPEN;
    }
    if (mmio_attached) {
        bits |= DRV_STATUS_MMIO_READY;
    }
    if (irq_attached) {
        bits |= DRV_STATUS_IRQ_BOUND;
    }
    if (irq_pending) {
        bits |= DRV_STATUS_IRQ_PENDING;
    }
    if (error_seen) {
        bits |= DRV_STATUS_ERROR;
    }

    return bits;
}

static int uart_server_open_allowed(int mmio_attached, int irq_attached) {
    if (!mmio_attached && irq_attached) {
        return 0;
    }
    return 1;
}

static int uart_server_resource_cap_valid(uint32_t resource_type,
                                          cap_id_t resource_cap) {
    int obj_type = sys_cap_type(resource_cap);
    int rights = sys_cap_rights(resource_cap);
    int required = CAP_READ | CAP_WRITE;

    if (obj_type < 0 || rights < 0) {
        return 0;
    }
    if (((uint8_t)rights & (uint8_t)required) != (uint8_t)required) {
        return 0;
    }
    if (resource_type == DRV_RESOURCE_MMIO) {
        return obj_type == CAP_OBJ_MMIO;
    }
    if (resource_type == DRV_RESOURCE_IRQ) {
        return obj_type == CAP_OBJ_IRQ;
    }
    return 0;
}

int driver_attach_resource(int ep_cap, uint32_t resource_type,
                           cap_id_t resource_cap, uint32_t timeout) {
    drv_msg_t msg;
    ipc_cap_xfer_t xfers[IPC_CAPS_MAX];
    int err;

    if (ep_cap <= 0 || resource_cap <= 0 ||
        !driver_resource_type_valid(resource_type)) {
        return KERN_ERR_PARAM;
    }

    for (uint32_t i = 0; i < IPC_CAPS_MAX; i++) {
        xfers[i].src_cap = KERN_INVALID_ID;
        xfers[i].rights = 0;
        xfers[i].flags = IPC_CAP_COPY;
    }
    xfers[0].src_cap = resource_cap;
    xfers[0].rights = CAP_READ | CAP_WRITE;
    xfers[0].flags = IPC_CAP_COPY;

    driver_msg_init(&msg, DRV_OP_ATTACH, 0);
    msg.command = resource_type;
    err = sys_ep_send_caps(ep_cap, &msg, xfers, 1, (int)timeout);
    if (err != KERN_OK) {
        return err;
    }

    return driver_check_reply(&msg, DRV_OP_ATTACH);
}

int driver_attach_cap(int ep_cap, cap_id_t resource_cap, uint32_t timeout) {
    return driver_attach_resource(ep_cap, DRV_RESOURCE_MMIO,
                                  resource_cap, timeout);
}

int driver_detach_resource(int ep_cap, uint32_t resource_type,
                           uint32_t timeout) {
    drv_msg_t msg;
    int err;

    if (ep_cap <= 0 || !driver_resource_type_valid(resource_type)) {
        return KERN_ERR_PARAM;
    }

    driver_msg_init(&msg, DRV_OP_DETACH, 0);
    msg.command = resource_type;
    err = sys_ep_send(ep_cap, &msg, (int)timeout);
    if (err != KERN_OK) {
        return err;
    }

    return driver_check_reply(&msg, DRV_OP_DETACH);
}

int uart_server_run(int ep_cap, uint32_t max_requests) {
    drv_msg_t msg;
    cap_id_t caps[IPC_CAPS_MAX];
    uint8_t cap_count = 0;
    int err = KERN_OK;
    int opened = 0;
    int mmio_attached = 0;
    int irq_attached = 0;
    int irq_pending = 0;
    int error_seen = 0;
    cap_id_t mmio_cap = KERN_INVALID_ID;
    cap_id_t irq_cap = KERN_INVALID_ID;

    if (ep_cap <= 0) {
        return KERN_ERR_PARAM;
    }

    for (uint32_t round = 0;
         (max_requests == 0U || round < max_requests) && err == KERN_OK;
         round++) {
        drv_zero(&msg, sizeof(msg));
        for (uint32_t i = 0; i < IPC_CAPS_MAX; i++) {
            caps[i] = KERN_INVALID_ID;
        }
        cap_count = 0;

        err = sys_ep_recv_caps(ep_cap, &msg, caps, &cap_count, 1000);
        if (err == KERN_ERR_TIMEOUT && max_requests == 0U) {
            err = KERN_OK;
            continue;
        }
        if (err != KERN_OK) {
            break;
        }

        if (msg.magic == DRV_IRQ_NOTIFY_BADGE) {
            driver_release_caps(caps, cap_count);
            irq_pending = 1;
            continue;
        }

        if (msg.magic != DRV_MAGIC || !driver_opcode_valid(msg.opcode)) {
            driver_release_caps(caps, cap_count);
            error_seen = 1;
            err = uart_server_reply(ep_cap, &msg, KERN_ERR_PARAM, 0);
            continue;
        }

        if (msg.opcode == DRV_OP_PING) {
            driver_release_caps(caps, cap_count);
            err = uart_server_reply(ep_cap, &msg, KERN_OK, 0);
        } else if (msg.opcode == DRV_OP_OPEN) {
            driver_release_caps(caps, cap_count);
            if (opened) {
                error_seen = 1;
                err = uart_server_reply(ep_cap, &msg, KERN_ERR_BUSY, 0);
            } else if (!uart_server_open_allowed(mmio_attached,
                                                 irq_attached)) {
                error_seen = 1;
                err = uart_server_reply(ep_cap, &msg, KERN_ERR_CAP, 0);
            } else {
                opened = 1;
                err = uart_server_reply(ep_cap, &msg, KERN_OK, 0);
            }
        } else if (msg.opcode == DRV_OP_CLOSE) {
            driver_release_caps(caps, cap_count);
            if (!opened) {
                error_seen = 1;
                err = uart_server_reply(ep_cap, &msg, KERN_ERR_STATE, 0);
            } else {
                opened = 0;
                err = uart_server_reply(ep_cap, &msg, KERN_OK, 0);
            }
        } else if (msg.opcode == DRV_OP_READ) {
            driver_release_caps(caps, cap_count);
            if (msg.length > DRV_PAYLOAD_MAX) {
                error_seen = 1;
                err = uart_server_reply(ep_cap, &msg, KERN_ERR_PARAM, 0);
            } else if (!opened) {
                error_seen = 1;
                err = uart_server_reply(ep_cap, &msg, KERN_ERR_STATE, 0);
            } else if (irq_pending && msg.length > 0U) {
                irq_pending = 0;
                msg.payload[0] = 0U;
                msg.length = 1;
                err = uart_server_reply(ep_cap, &msg, KERN_OK, 1);
            } else {
                msg.length = 0;
                err = uart_server_reply(ep_cap, &msg, KERN_OK, 0);
            }
        } else if (msg.opcode == DRV_OP_WRITE) {
            driver_release_caps(caps, cap_count);
            if (msg.length > DRV_PAYLOAD_MAX) {
                error_seen = 1;
                err = uart_server_reply(ep_cap, &msg, KERN_ERR_PARAM, 0);
            } else if (!opened) {
                error_seen = 1;
                err = uart_server_reply(ep_cap, &msg, KERN_ERR_STATE, 0);
            } else {
                err = uart_server_reply(ep_cap, &msg, KERN_OK,
                                        (int)msg.length);
            }
        } else if (msg.opcode == DRV_OP_POLL) {
            driver_release_caps(caps, cap_count);
            uint32_t events = opened ? DRV_EVENT_WRITABLE : 0U;
            if (irq_pending) {
                events |= DRV_EVENT_READABLE;
            }
            err = uart_server_reply(ep_cap, &msg, KERN_OK, (int)events);
        } else if (msg.opcode == DRV_OP_IOCTL) {
            driver_release_caps(caps, cap_count);
            if (msg.command == DRV_IOCTL_GET_EVENTS) {
                uint32_t events = opened ? DRV_EVENT_WRITABLE : 0U;
                if (irq_pending) {
                    events |= DRV_EVENT_READABLE;
                }
                err = uart_server_reply(ep_cap, &msg, KERN_OK, (int)events);
            } else if (msg.command == DRV_IOCTL_GET_RESOURCES) {
                uint32_t resources =
                    uart_server_resource_bits(mmio_attached, irq_attached);
                err = uart_server_reply(ep_cap, &msg, KERN_OK,
                                        (int)resources);
            } else if (msg.command == DRV_IOCTL_GET_STATUS) {
                uint32_t status =
                    uart_server_status_bits(opened, mmio_attached,
                                            irq_attached, irq_pending,
                                            error_seen);
                err = uart_server_reply(ep_cap, &msg, KERN_OK,
                                        (int)status);
            } else if (msg.command == DRV_IOCTL_CLEAR_STATUS) {
                irq_pending = 0;
                error_seen = 0;
                err = uart_server_reply(ep_cap, &msg, KERN_OK, 0);
            } else {
                error_seen = 1;
                err = uart_server_reply(ep_cap, &msg, KERN_ERR_PARAM, 0);
            }
        } else if (msg.opcode == DRV_OP_ATTACH) {
            if (cap_count != 1 || caps[0] <= 0) {
                driver_release_caps(caps, cap_count);
                error_seen = 1;
                err = uart_server_reply(ep_cap, &msg, KERN_ERR_CAP, 0);
            } else if (!driver_resource_type_valid(msg.command)) {
                driver_release_caps(caps, cap_count);
                error_seen = 1;
                err = uart_server_reply(ep_cap, &msg, KERN_ERR_PARAM, 0);
            } else {
                uint32_t resource_type = msg.command;
                int busy = 0;

                if (!uart_server_resource_cap_valid(resource_type, caps[0])) {
                    driver_release_caps(caps, cap_count);
                    error_seen = 1;
                    err = uart_server_reply(ep_cap, &msg, KERN_ERR_CAP, 0);
                } else {
                    if (resource_type == DRV_RESOURCE_MMIO) {
                        busy = mmio_attached;
                        if (!busy) {
                            mmio_attached = 1;
                            mmio_cap = caps[0];
                            caps[0] = KERN_INVALID_ID;
                        }
                    } else if (resource_type == DRV_RESOURCE_IRQ) {
                        busy = irq_attached;
                        if (!busy) {
                            int bind_err =
                                sys_irq_bind((int)caps[0], ep_cap,
                                             DRV_IRQ_NOTIFY_BADGE);
                            if (bind_err != KERN_OK) {
                                driver_release_caps(caps, cap_count);
                                error_seen = 1;
                                err = uart_server_reply(ep_cap, &msg,
                                                        bind_err, 0);
                                continue;
                            }
                            irq_attached = 1;
                            irq_cap = caps[0];
                            caps[0] = KERN_INVALID_ID;
                        }
                    }
                    driver_release_caps(caps, cap_count);
                    err = uart_server_reply(ep_cap, &msg,
                                            busy ? KERN_ERR_BUSY : KERN_OK,
                                            busy ? 0 : (int)resource_type);
                }
            }
        } else if (msg.opcode == DRV_OP_DETACH) {
            driver_release_caps(caps, cap_count);
            if (!driver_resource_type_valid(msg.command)) {
                error_seen = 1;
                err = uart_server_reply(ep_cap, &msg, KERN_ERR_PARAM, 0);
            } else if (opened) {
                error_seen = 1;
                err = uart_server_reply(ep_cap, &msg, KERN_ERR_BUSY, 0);
            } else if (msg.command == DRV_RESOURCE_MMIO) {
                if (!mmio_attached || mmio_cap <= 0) {
                    error_seen = 1;
                    err = uart_server_reply(ep_cap, &msg, KERN_ERR_STATE, 0);
                } else {
                    (void)sys_cap_revoke(mmio_cap);
                    mmio_cap = KERN_INVALID_ID;
                    mmio_attached = 0;
                    err = uart_server_reply(ep_cap, &msg, KERN_OK,
                                            DRV_RESOURCE_MMIO);
                }
            } else if (msg.command == DRV_RESOURCE_IRQ) {
                if (!irq_attached || irq_cap <= 0) {
                    error_seen = 1;
                    err = uart_server_reply(ep_cap, &msg, KERN_ERR_STATE, 0);
                } else {
                    (void)sys_cap_revoke(irq_cap);
                    irq_cap = KERN_INVALID_ID;
                    irq_attached = 0;
                    irq_pending = 0;
                    err = uart_server_reply(ep_cap, &msg, KERN_OK,
                                            DRV_RESOURCE_IRQ);
                }
            }
        } else {
            driver_release_caps(caps, cap_count);
            error_seen = 1;
            err = uart_server_reply(ep_cap, &msg, KERN_ERR_STATE, 0);
        }
    }

    if (mmio_cap > 0) {
        (void)sys_cap_revoke(mmio_cap);
    }
    if (irq_cap > 0) {
        (void)sys_cap_revoke(irq_cap);
    }
    return err;
}

#endif /* DRIVER_ENABLE && CAP_ENABLE */
