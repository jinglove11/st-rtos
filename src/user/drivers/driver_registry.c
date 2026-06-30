/**
 * @file driver_registry.c
 * @brief Static user-space driver service registry
 */

#include "driver_registry.h"
#include "kernel_types.h"
#include <string.h>

#if DRIVER_ENABLE

static const driver_descriptor_t driver_registry[] = {
    {
        .service_name = "dev.uart0",
        .device_name = "uart0",
        .ops = DRIVER_OP_BITS_UART,
        .ioctls = DRIVER_IOCTL_BITS_UART,
        .resources = DRV_RESOURCE_BIT_MMIO | DRV_RESOURCE_BIT_IRQ,
        .required_resources = DRV_RESOURCE_BIT_MMIO,
        .optional_resources = DRV_RESOURCE_BIT_IRQ,
        .status_bits = DRV_STATUS_OPEN | DRV_STATUS_MMIO_READY |
                       DRV_STATUS_IRQ_BOUND | DRV_STATUS_IRQ_PENDING |
                       DRV_STATUS_ERROR,
    },
};

static int driver_string_valid(const char *s) {
    return s != NULL && s[0] != '\0';
}

uint32_t driver_registry_count(void) {
    return (uint32_t)(sizeof(driver_registry) / sizeof(driver_registry[0]));
}

const driver_descriptor_t *driver_registry_get(uint32_t index) {
    if (index >= driver_registry_count()) {
        return NULL;
    }
    return &driver_registry[index];
}

const driver_descriptor_t *driver_registry_find(const char *service_name) {
    const driver_descriptor_t *desc = NULL;

    if (driver_registry_query(service_name, &desc) != KERN_OK) {
        return NULL;
    }
    return desc;
}

int driver_registry_query(const char *service_name,
                          const driver_descriptor_t **out_desc) {
    if (service_name == NULL || out_desc == NULL) {
        return KERN_ERR_PARAM;
    }
    *out_desc = NULL;
    for (uint32_t i = 0; i < driver_registry_count(); i++) {
        if (strcmp(driver_registry[i].service_name, service_name) == 0) {
            *out_desc = &driver_registry[i];
            return KERN_OK;
        }
    }
    return KERN_ERR_NOEXIST;
}

const driver_descriptor_t *driver_registry_find_by_caps(uint32_t required_ops,
                                                        uint32_t required_ioctls,
                                                        uint32_t required_resources) {
    const driver_descriptor_t *desc = NULL;

    if (driver_registry_query_by_caps(required_ops, required_ioctls,
                                      required_resources, &desc) != KERN_OK) {
        return NULL;
    }
    return desc;
}

int driver_registry_query_by_caps(uint32_t required_ops,
                                  uint32_t required_ioctls,
                                  uint32_t required_resources,
                                  const driver_descriptor_t **out_desc) {
    if (out_desc == NULL) {
        return KERN_ERR_PARAM;
    }
    *out_desc = NULL;

    for (uint32_t i = 0; i < driver_registry_count(); i++) {
        const driver_descriptor_t *desc = &driver_registry[i];

        if (driver_descriptor_supports(desc, required_ops, required_ioctls,
                                       required_resources)) {
            *out_desc = desc;
            return KERN_OK;
        }
    }
    return KERN_ERR_NOEXIST;
}

int driver_descriptor_supports(const driver_descriptor_t *desc,
                               uint32_t required_ops,
                               uint32_t required_ioctls,
                               uint32_t required_resources) {
    if (desc == NULL) {
        return 0;
    }
    if ((desc->ops & required_ops) != required_ops) {
        return 0;
    }
    if ((desc->ioctls & required_ioctls) != required_ioctls) {
        return 0;
    }
    if ((desc->resources & required_resources) != required_resources) {
        return 0;
    }
    return 1;
}

int driver_op_bit_to_opcode(uint32_t op_bit, uint16_t *out_opcode) {
    if (out_opcode == NULL) {
        return KERN_ERR_PARAM;
    }
    *out_opcode = 0;

    switch (op_bit) {
    case DRIVER_OP_BIT_PING:
        *out_opcode = DRV_OP_PING;
        return KERN_OK;
    case DRIVER_OP_BIT_OPEN:
        *out_opcode = DRV_OP_OPEN;
        return KERN_OK;
    case DRIVER_OP_BIT_CLOSE:
        *out_opcode = DRV_OP_CLOSE;
        return KERN_OK;
    case DRIVER_OP_BIT_READ:
        *out_opcode = DRV_OP_READ;
        return KERN_OK;
    case DRIVER_OP_BIT_WRITE:
        *out_opcode = DRV_OP_WRITE;
        return KERN_OK;
    case DRIVER_OP_BIT_IOCTL:
        *out_opcode = DRV_OP_IOCTL;
        return KERN_OK;
    case DRIVER_OP_BIT_POLL:
        *out_opcode = DRV_OP_POLL;
        return KERN_OK;
    case DRIVER_OP_BIT_ATTACH:
        *out_opcode = DRV_OP_ATTACH;
        return KERN_OK;
    case DRIVER_OP_BIT_DETACH:
        *out_opcode = DRV_OP_DETACH;
        return KERN_OK;
    default:
        return KERN_ERR_PARAM;
    }
}

int driver_opcode_to_op_bit(uint16_t opcode, uint32_t *out_op_bit) {
    if (out_op_bit == NULL) {
        return KERN_ERR_PARAM;
    }
    *out_op_bit = 0;

    switch (opcode) {
    case DRV_OP_PING:
        *out_op_bit = DRIVER_OP_BIT_PING;
        return KERN_OK;
    case DRV_OP_OPEN:
        *out_op_bit = DRIVER_OP_BIT_OPEN;
        return KERN_OK;
    case DRV_OP_CLOSE:
        *out_op_bit = DRIVER_OP_BIT_CLOSE;
        return KERN_OK;
    case DRV_OP_READ:
        *out_op_bit = DRIVER_OP_BIT_READ;
        return KERN_OK;
    case DRV_OP_WRITE:
        *out_op_bit = DRIVER_OP_BIT_WRITE;
        return KERN_OK;
    case DRV_OP_IOCTL:
        *out_op_bit = DRIVER_OP_BIT_IOCTL;
        return KERN_OK;
    case DRV_OP_POLL:
        *out_op_bit = DRIVER_OP_BIT_POLL;
        return KERN_OK;
    case DRV_OP_ATTACH:
        *out_op_bit = DRIVER_OP_BIT_ATTACH;
        return KERN_OK;
    case DRV_OP_DETACH:
        *out_op_bit = DRIVER_OP_BIT_DETACH;
        return KERN_OK;
    default:
        return KERN_ERR_PARAM;
    }
}

int driver_ioctl_bit_to_command(uint32_t ioctl_bit, uint32_t *out_command) {
    if (out_command == NULL) {
        return KERN_ERR_PARAM;
    }
    *out_command = 0;

    switch (ioctl_bit) {
    case DRIVER_IOCTL_BIT_GET_EVENTS:
        *out_command = DRV_IOCTL_GET_EVENTS;
        return KERN_OK;
    case DRIVER_IOCTL_BIT_GET_RESOURCES:
        *out_command = DRV_IOCTL_GET_RESOURCES;
        return KERN_OK;
    case DRIVER_IOCTL_BIT_GET_STATUS:
        *out_command = DRV_IOCTL_GET_STATUS;
        return KERN_OK;
    case DRIVER_IOCTL_BIT_CLEAR_STATUS:
        *out_command = DRV_IOCTL_CLEAR_STATUS;
        return KERN_OK;
    default:
        return KERN_ERR_PARAM;
    }
}

int driver_ioctl_command_to_bit(uint32_t command, uint32_t *out_ioctl_bit) {
    if (out_ioctl_bit == NULL) {
        return KERN_ERR_PARAM;
    }
    *out_ioctl_bit = 0;

    switch (command) {
    case DRV_IOCTL_GET_EVENTS:
        *out_ioctl_bit = DRIVER_IOCTL_BIT_GET_EVENTS;
        return KERN_OK;
    case DRV_IOCTL_GET_RESOURCES:
        *out_ioctl_bit = DRIVER_IOCTL_BIT_GET_RESOURCES;
        return KERN_OK;
    case DRV_IOCTL_GET_STATUS:
        *out_ioctl_bit = DRIVER_IOCTL_BIT_GET_STATUS;
        return KERN_OK;
    case DRV_IOCTL_CLEAR_STATUS:
        *out_ioctl_bit = DRIVER_IOCTL_BIT_CLEAR_STATUS;
        return KERN_OK;
    default:
        return KERN_ERR_PARAM;
    }
}

int driver_resource_bit_to_type(uint32_t resource_bit, uint32_t *out_type) {
    if (out_type == NULL) {
        return KERN_ERR_PARAM;
    }
    *out_type = 0;

    switch (resource_bit) {
    case DRV_RESOURCE_BIT_MMIO:
        *out_type = DRV_RESOURCE_MMIO;
        return KERN_OK;
    case DRV_RESOURCE_BIT_IRQ:
        *out_type = DRV_RESOURCE_IRQ;
        return KERN_OK;
    default:
        return KERN_ERR_PARAM;
    }
}

int driver_resource_type_to_bit(uint32_t resource_type,
                                uint32_t *out_resource_bit) {
    if (out_resource_bit == NULL) {
        return KERN_ERR_PARAM;
    }
    *out_resource_bit = 0;

    switch (resource_type) {
    case DRV_RESOURCE_MMIO:
        *out_resource_bit = DRV_RESOURCE_BIT_MMIO;
        return KERN_OK;
    case DRV_RESOURCE_IRQ:
        *out_resource_bit = DRV_RESOURCE_BIT_IRQ;
        return KERN_OK;
    default:
        return KERN_ERR_PARAM;
    }
}

const char *driver_op_bit_name(uint32_t op_bit) {
    switch (op_bit) {
    case DRIVER_OP_BIT_PING:
        return "ping";
    case DRIVER_OP_BIT_OPEN:
        return "open";
    case DRIVER_OP_BIT_CLOSE:
        return "close";
    case DRIVER_OP_BIT_READ:
        return "read";
    case DRIVER_OP_BIT_WRITE:
        return "write";
    case DRIVER_OP_BIT_IOCTL:
        return "ioctl";
    case DRIVER_OP_BIT_POLL:
        return "poll";
    case DRIVER_OP_BIT_ATTACH:
        return "attach";
    case DRIVER_OP_BIT_DETACH:
        return "detach";
    default:
        return NULL;
    }
}

const char *driver_ioctl_bit_name(uint32_t ioctl_bit) {
    switch (ioctl_bit) {
    case DRIVER_IOCTL_BIT_GET_EVENTS:
        return "events";
    case DRIVER_IOCTL_BIT_GET_RESOURCES:
        return "resources";
    case DRIVER_IOCTL_BIT_GET_STATUS:
        return "status";
    case DRIVER_IOCTL_BIT_CLEAR_STATUS:
        return "clear-status";
    default:
        return NULL;
    }
}

const char *driver_resource_bit_name(uint32_t resource_bit) {
    switch (resource_bit) {
    case DRV_RESOURCE_BIT_MMIO:
        return "mmio";
    case DRV_RESOURCE_BIT_IRQ:
        return "irq";
    default:
        return NULL;
    }
}

const char *driver_status_bit_name(uint32_t status_bit) {
    switch (status_bit) {
    case DRV_STATUS_OPEN:
        return "open";
    case DRV_STATUS_MMIO_READY:
        return "mmio";
    case DRV_STATUS_IRQ_BOUND:
        return "irq";
    case DRV_STATUS_IRQ_PENDING:
        return "pending";
    case DRV_STATUS_ERROR:
        return "error";
    default:
        return NULL;
    }
}

int driver_registry_validate_desc(const driver_descriptor_t *desc) {
    const uint32_t known_ops = DRIVER_OP_BITS_UART;
    const uint32_t known_ioctls = DRIVER_IOCTL_BITS_UART;
    const uint32_t known_resources = DRV_RESOURCE_BIT_MMIO |
                                     DRV_RESOURCE_BIT_IRQ;
    const uint32_t known_status = DRV_STATUS_OPEN | DRV_STATUS_MMIO_READY |
                                  DRV_STATUS_IRQ_BOUND |
                                  DRV_STATUS_IRQ_PENDING | DRV_STATUS_ERROR;

    if (desc == NULL || !driver_string_valid(desc->service_name) ||
        !driver_string_valid(desc->device_name)) {
        return KERN_ERR_PARAM;
    }
    if (desc->ops == 0U || (desc->ops & ~known_ops) != 0U) {
        return KERN_ERR_PARAM;
    }
    if ((desc->ops & DRIVER_OP_BIT_IOCTL) == 0U && desc->ioctls != 0U) {
        return KERN_ERR_PARAM;
    }
    if ((desc->ioctls & ~known_ioctls) != 0U) {
        return KERN_ERR_PARAM;
    }
    if ((desc->resources & ~known_resources) != 0U) {
        return KERN_ERR_PARAM;
    }
    if ((desc->required_resources & ~desc->resources) != 0U ||
        (desc->optional_resources & ~desc->resources) != 0U) {
        return KERN_ERR_PARAM;
    }
    if ((desc->required_resources & desc->optional_resources) != 0U) {
        return KERN_ERR_PARAM;
    }
    if ((desc->status_bits & ~known_status) != 0U) {
        return KERN_ERR_PARAM;
    }
    return KERN_OK;
}

int driver_registry_validate_all(void) {
    uint32_t count = driver_registry_count();

    for (uint32_t i = 0; i < count; i++) {
        int err = driver_registry_validate_desc(&driver_registry[i]);
        if (err != KERN_OK) {
            return err;
        }
        for (uint32_t j = i + 1U; j < count; j++) {
            if (strcmp(driver_registry[i].service_name,
                       driver_registry[j].service_name) == 0) {
                return KERN_ERR_BUSY;
            }
        }
    }
    return KERN_OK;
}

#endif /* DRIVER_ENABLE */
