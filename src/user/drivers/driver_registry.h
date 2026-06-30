/**
 * @file driver_registry.h
 * @brief Static user-space driver service registry
 */

#ifndef DRIVER_REGISTRY_H
#define DRIVER_REGISTRY_H

#include "driver_proto.h"
#include <stdint.h>

#define DRIVER_OP_BIT_PING    (1U << 0)
#define DRIVER_OP_BIT_OPEN    (1U << 1)
#define DRIVER_OP_BIT_CLOSE   (1U << 2)
#define DRIVER_OP_BIT_READ    (1U << 3)
#define DRIVER_OP_BIT_WRITE   (1U << 4)
#define DRIVER_OP_BIT_IOCTL   (1U << 5)
#define DRIVER_OP_BIT_POLL    (1U << 6)
#define DRIVER_OP_BIT_ATTACH  (1U << 7)
#define DRIVER_OP_BIT_DETACH  (1U << 8)

#define DRIVER_OP_BITS_UART \
    (DRIVER_OP_BIT_PING | DRIVER_OP_BIT_OPEN | DRIVER_OP_BIT_CLOSE | \
     DRIVER_OP_BIT_READ | DRIVER_OP_BIT_WRITE | DRIVER_OP_BIT_IOCTL | \
     DRIVER_OP_BIT_POLL | DRIVER_OP_BIT_ATTACH | DRIVER_OP_BIT_DETACH)

#define DRIVER_IOCTL_BIT_GET_EVENTS    (1U << 0)
#define DRIVER_IOCTL_BIT_GET_RESOURCES (1U << 1)
#define DRIVER_IOCTL_BIT_GET_STATUS    (1U << 2)
#define DRIVER_IOCTL_BIT_CLEAR_STATUS  (1U << 3)

#define DRIVER_IOCTL_BITS_UART \
    (DRIVER_IOCTL_BIT_GET_EVENTS | DRIVER_IOCTL_BIT_GET_RESOURCES | \
     DRIVER_IOCTL_BIT_GET_STATUS | DRIVER_IOCTL_BIT_CLEAR_STATUS)

typedef struct {
    const char *service_name;
    const char *device_name;
    uint32_t ops;
    uint32_t ioctls;
    uint32_t resources;
    uint32_t required_resources;
    uint32_t optional_resources;
    uint32_t status_bits;
} driver_descriptor_t;

uint32_t driver_registry_count(void);
const driver_descriptor_t *driver_registry_get(uint32_t index);
const driver_descriptor_t *driver_registry_find(const char *service_name);
int driver_registry_query(const char *service_name,
                          const driver_descriptor_t **out_desc);
const driver_descriptor_t *driver_registry_find_by_caps(uint32_t required_ops,
                                                        uint32_t required_ioctls,
                                                        uint32_t required_resources);
int driver_registry_query_by_caps(uint32_t required_ops,
                                  uint32_t required_ioctls,
                                  uint32_t required_resources,
                                  const driver_descriptor_t **out_desc);
int driver_descriptor_supports(const driver_descriptor_t *desc,
                               uint32_t required_ops,
                               uint32_t required_ioctls,
                               uint32_t required_resources);
int driver_registry_validate_desc(const driver_descriptor_t *desc);
int driver_registry_validate_all(void);
int driver_op_bit_to_opcode(uint32_t op_bit, uint16_t *out_opcode);
int driver_opcode_to_op_bit(uint16_t opcode, uint32_t *out_op_bit);
int driver_ioctl_bit_to_command(uint32_t ioctl_bit, uint32_t *out_command);
int driver_ioctl_command_to_bit(uint32_t command, uint32_t *out_ioctl_bit);
int driver_resource_bit_to_type(uint32_t resource_bit, uint32_t *out_type);
int driver_resource_type_to_bit(uint32_t resource_type,
                                uint32_t *out_resource_bit);
const char *driver_op_bit_name(uint32_t op_bit);
const char *driver_ioctl_bit_name(uint32_t ioctl_bit);
const char *driver_resource_bit_name(uint32_t resource_bit);
const char *driver_status_bit_name(uint32_t status_bit);

#endif /* DRIVER_REGISTRY_H */
