/**
 * @file ipc_transfer.c
 * @brief IPC capability transfer helpers
 */

#include "ipc_transfer.h"

#if CAP_ENABLE

static void ipc_rollback_caps(tcb_t *owner, cap_id_t *caps, uint8_t count) {
    for (uint8_t i = 0; i < count; i++) {
        if (caps[i] >= 0) {
            (void)cap_revoke_for(owner, caps[i]);
            caps[i] = (cap_id_t)-1;
        }
    }
}

kern_err_t ipc_transfer_caps(tcb_t *src,
                             tcb_t *dst,
                             const ipc_cap_xfer_t *xfers,
                             uint8_t count,
                             cap_id_t *out_caps) {
    if (src == NULL || dst == NULL || xfers == NULL) {
        return KERN_ERR_PARAM;
    }
    if (count > IPC_CAPS_MAX) {
        return KERN_ERR_PARAM;
    }

    cap_id_t staged[IPC_CAPS_MAX];
    uint8_t move_source[IPC_CAPS_MAX];
    for (uint8_t i = 0; i < IPC_CAPS_MAX; i++) {
        staged[i] = (cap_id_t)-1;
        move_source[i] = 0;
    }

    for (uint8_t i = 0; i < count; i++) {
        if (xfers[i].flags == IPC_CAP_MOVE ||
            xfers[i].flags == IPC_CAP_COPY) {
            staged[i] = cap_copy_to(src, xfers[i].src_cap, dst, xfers[i].rights);
            if (staged[i] < 0) {
                ipc_rollback_caps(dst, staged, i);
                return KERN_ERR_CAP;
            }
            move_source[i] = (xfers[i].flags == IPC_CAP_MOVE) ? 1U : 0U;
        } else {
            ipc_rollback_caps(dst, staged, i);
            return KERN_ERR_PARAM;
        }
    }

    for (uint8_t i = 0; i < count; i++) {
        if (move_source[i] != 0) {
            kern_err_t err = cap_revoke_for(src, xfers[i].src_cap);
            if (err != KERN_OK) {
                ipc_rollback_caps(dst, staged, count);
                return err;
            }
        }
    }

    if (out_caps != NULL) {
        for (uint8_t i = 0; i < count; i++) {
            out_caps[i] = staged[i];
        }
    }

    return KERN_OK;
}

#endif /* CAP_ENABLE */
