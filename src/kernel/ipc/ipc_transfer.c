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
            /* Phase H3:用 cap_delete (只删单个 slot) 替代 cap_revoke_for
             * (递归删子树)。cap_copy_to 创建的 child 挂在源子树下,
             * cap_revoke_for 会连带删 child (dst 收到的 cap 立即失效)。
             * cap_delete 只删源 slot,child 断开 parent 但保留 (dst 持有)。
             * cap_clear_slot 的 revoke_hook 因 refcount>0 (dst 有 copy) 不
             * 释放 backing,cleanup 同理。 */
            cap_delete(xfers[i].src_cap);
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
