/**
 * @file ipc_transfer.c
 * @brief IPC capability transfer helpers
 */

#include "ipc_transfer.h"

#if CAP_ENABLE

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

    if (out_caps != NULL) {
        for (uint8_t i = 0; i < count; i++) {
            out_caps[i] = (cap_id_t)-1;
        }
    }

    cap_transaction_t txn;
    cap_txn_begin(&txn);
    for (uint8_t i = 0; i < count; i++) {
        kern_err_t err;
        if (xfers[i].flags == IPC_CAP_COPY) {
            err = cap_txn_prepare_copy(&txn, src, xfers[i].src_cap, dst,
                                       xfers[i].rights);
        } else if (xfers[i].flags == IPC_CAP_MOVE) {
            err = cap_txn_prepare_move(&txn, src, xfers[i].src_cap, dst,
                                       xfers[i].rights);
        } else {
            (void)cap_txn_rollback(&txn);
            return KERN_ERR_PARAM;
        }
        if (err != KERN_OK) {
            (void)cap_txn_rollback(&txn);
            return err;
        }
    }

    kern_err_t err = cap_txn_commit(&txn);
    if (err != KERN_OK) {
        return err;
    }
    if (out_caps != NULL) {
        for (uint8_t i = 0; i < count; i++) {
            out_caps[i] = txn.results[i];
        }
    }

    return KERN_OK;
}

#endif /* CAP_ENABLE */
