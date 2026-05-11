/**
 * @file ipc_transfer.h
 * @brief IPC capability transfer helpers
 */

#ifndef IPC_TRANSFER_H
#define IPC_TRANSFER_H

#include "kernel_types.h"
#include "capability.h"

#if CAP_ENABLE

#define IPC_CAPS_MAX 4

#define IPC_CAP_COPY 0x00
#define IPC_CAP_MOVE 0x01

typedef struct {
    cap_id_t src_cap;
    uint8_t  rights;
    uint8_t  flags;
} ipc_cap_xfer_t;

typedef struct {
    uint16_t len;
    uint8_t  flags;
    uint8_t  cap_count;
    ipc_cap_xfer_t caps[IPC_CAPS_MAX];
} ipc_msg_hdr_t;

kern_err_t ipc_transfer_caps(tcb_t *src,
                             tcb_t *dst,
                             const ipc_cap_xfer_t *xfers,
                             uint8_t count,
                             cap_id_t *out_caps);

#endif /* CAP_ENABLE */

#endif /* IPC_TRANSFER_H */
