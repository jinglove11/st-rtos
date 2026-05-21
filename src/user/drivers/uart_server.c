/**
 * @file uart_server.c
 * @brief Minimal user-space UART driver server prototype
 */

#include "driver_proto.h"
#include "user_api.h"

#if DRIVER_ENABLE && CAP_ENABLE

static void drv_zero(void *ptr, uint32_t len) {
    uint8_t *p = (uint8_t *)ptr;
    for (uint32_t i = 0; i < len; i++) {
        p[i] = 0;
    }
}

int driver_opcode_valid(uint16_t opcode) {
    return opcode >= DRV_OP_PING && opcode <= DRV_OP_POLL;
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

int uart_server_run(int ep_cap, uint32_t max_requests) {
    drv_msg_t msg;
    int err = KERN_OK;

    if (ep_cap <= 0) {
        return KERN_ERR_PARAM;
    }

    for (uint32_t round = 0;
         (max_requests == 0U || round < max_requests) && err == KERN_OK;
         round++) {
        drv_zero(&msg, sizeof(msg));

        err = sys_ep_recv(ep_cap, &msg, 1000);
        if (err != KERN_OK) {
            break;
        }

        if (msg.magic != DRV_MAGIC || !driver_opcode_valid(msg.opcode)) {
            err = uart_server_reply(ep_cap, &msg, KERN_ERR_PARAM, 0);
            continue;
        }

        if (msg.opcode == DRV_OP_PING) {
            err = uart_server_reply(ep_cap, &msg, KERN_OK, 0);
        } else if (msg.opcode == DRV_OP_WRITE) {
            if (msg.length > DRV_PAYLOAD_MAX) {
                err = uart_server_reply(ep_cap, &msg, KERN_ERR_PARAM, 0);
            } else {
                err = uart_server_reply(ep_cap, &msg, KERN_OK,
                                        (int)msg.length);
            }
        } else {
            err = uart_server_reply(ep_cap, &msg, KERN_ERR_STATE, 0);
        }
    }

    return err;
}

#endif /* DRIVER_ENABLE && CAP_ENABLE */
