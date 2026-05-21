/**
 * @file driver_proto.h
 * @brief User-space driver server IPC protocol
 */

#ifndef DRIVER_PROTO_H
#define DRIVER_PROTO_H

#include "kernel_types.h"
#include <stdint.h>

#define DRV_MAGIC             0x44525652U
#define DRV_PAYLOAD_MAX       32U

#define DRV_OP_PING           1U
#define DRV_OP_OPEN           2U
#define DRV_OP_CLOSE          3U
#define DRV_OP_READ           4U
#define DRV_OP_WRITE          5U
#define DRV_OP_IOCTL          6U
#define DRV_OP_POLL           7U

#define DRV_FLAG_NONE         0U

typedef struct {
    uint32_t magic;
    uint16_t opcode;
    uint16_t flags;
    uint32_t seq;
    int32_t status;
    int32_t result;
    uint32_t length;
    uint8_t payload[DRV_PAYLOAD_MAX];
} drv_msg_t;

int driver_opcode_valid(uint16_t opcode);
void driver_msg_init(drv_msg_t *msg, uint16_t opcode, uint32_t seq);

int driver_ping(int ep_cap, uint32_t timeout);
int driver_write(int ep_cap, const void *buf, uint32_t len, uint32_t timeout);

int uart_server_run(int ep_cap, uint32_t max_requests);

#endif /* DRIVER_PROTO_H */
