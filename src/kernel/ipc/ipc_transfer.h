/**
 * @file ipc_transfer.h
 * @brief IPC capability transfer helpers
 */

#ifndef IPC_TRANSFER_H
#define IPC_TRANSFER_H

#include "kernel_types.h"
#include "capability.h"

#if CAP_ENABLE

/* IPC_CAPS_MAX 与 ipc_cap_xfer_t 定义在 kernel_types.h (M3-Step2b:
 * syscall_cont_t 的 ch_send payload 内联到 TCB,避免循环 include)。 */

#define IPC_CAP_COPY 0x00
#define IPC_CAP_MOVE 0x01

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
