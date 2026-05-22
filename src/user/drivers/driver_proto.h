/**
 * @file driver_proto.h
 * @brief User-space driver server IPC protocol
 */

#ifndef DRIVER_PROTO_H
#define DRIVER_PROTO_H

#include "kernel_types.h"
#include <stdint.h>

#define DRV_MAGIC             0x44525652U
#define DRV_IRQ_NOTIFY_BADGE  0x44524951U
#define DRV_PAYLOAD_MAX       32U

#define DRV_OP_PING           1U
#define DRV_OP_OPEN           2U
#define DRV_OP_CLOSE          3U
#define DRV_OP_READ           4U
#define DRV_OP_WRITE          5U
#define DRV_OP_IOCTL          6U
#define DRV_OP_POLL           7U
#define DRV_OP_ATTACH         8U

#define DRV_FLAG_NONE         0U

#define DRV_EVENT_READABLE    (1U << 0)
#define DRV_EVENT_WRITABLE    (1U << 1)
#define DRV_EVENT_ERROR       (1U << 2)
#define DRV_EVENT_REMOVED     (1U << 3)

#define DRV_IOCTL_GET_EVENTS    1U
#define DRV_IOCTL_GET_RESOURCES 2U

#define DRV_RESOURCE_MMIO     1U
#define DRV_RESOURCE_IRQ      2U

#define DRV_RESOURCE_BIT_MMIO (1U << 0)
#define DRV_RESOURCE_BIT_IRQ  (1U << 1)

typedef struct {
    uint32_t magic;
    uint16_t opcode;
    uint16_t flags;
    uint32_t seq;
    int32_t status;
    int32_t result;
    uint32_t length;
    uint32_t command;
    uint8_t payload[DRV_PAYLOAD_MAX];
} drv_msg_t;

int driver_opcode_valid(uint16_t opcode);
void driver_msg_init(drv_msg_t *msg, uint16_t opcode, uint32_t seq);

int driver_ping(int ep_cap, uint32_t timeout);
int driver_open(int ep_cap, uint32_t flags, uint32_t timeout);
int driver_close(int ep_cap, uint32_t timeout);
int driver_read(int ep_cap, void *buf, uint32_t len, uint32_t timeout);
int driver_write(int ep_cap, const void *buf, uint32_t len, uint32_t timeout);
int driver_poll(int ep_cap, uint32_t *out_events, uint32_t timeout);
int driver_ioctl(int ep_cap, uint32_t command, uint32_t *out_value,
                 uint32_t timeout);
int driver_get_events(int ep_cap, uint32_t *out_events, uint32_t timeout);
int driver_get_resources(int ep_cap, uint32_t *out_resources,
                         uint32_t timeout);
int driver_attach_resource(int ep_cap, uint32_t resource_type,
                           cap_id_t resource_cap, uint32_t timeout);
int driver_attach_cap(int ep_cap, cap_id_t resource_cap, uint32_t timeout);

int uart_server_run(int ep_cap, uint32_t max_requests);

#endif /* DRIVER_PROTO_H */
