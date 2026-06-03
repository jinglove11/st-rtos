/**
 * @file channel.h
 * @brief Channel (P2P) 双向通信 — 一对一模型，带共享内存
 */

#ifndef CHANNEL_H
#define CHANNEL_H

#include "kernel_types.h"
#include "ipc_transfer.h"

void channel_init(void);

ch_id_t    channel_create(uint16_t msg_size, uint32_t shm_size);
kern_err_t channel_delete(ch_id_t ch_id);
kern_err_t channel_connect(ch_id_t ch_id, task_id_t peer_a, task_id_t peer_b);
kern_err_t channel_send(ch_id_t ch_id, const void *msg, uint32_t timeout);
#if SYSCALL_ENABLE
kern_err_t channel_send_syscall(ch_id_t ch_id, const void *msg,
                                uint32_t timeout);
#if CAP_ENABLE
kern_err_t channel_send_caps_syscall(ch_id_t ch_id, const void *msg,
                                     const ipc_cap_xfer_t *caps,
                                     uint8_t cap_count,
                                     uint32_t timeout);
#endif
#endif
#if CAP_ENABLE
kern_err_t channel_send_caps(ch_id_t ch_id, const void *msg,
                             const ipc_cap_xfer_t *caps,
                             uint8_t cap_count,
                             uint32_t timeout);
#endif
kern_err_t channel_recv(ch_id_t ch_id, void *msg, uint32_t timeout);
#if SYSCALL_ENABLE
kern_err_t channel_recv_syscall(ch_id_t ch_id, void *user_msg,
                                uint32_t timeout);
#if CAP_ENABLE
kern_err_t channel_recv_caps_syscall(ch_id_t ch_id,
                                     void *user_msg,
                                     cap_id_t *out_caps,
                                     uint8_t *out_cap_count,
                                     uint32_t timeout);
#endif
#endif
#if CAP_ENABLE
kern_err_t channel_recv_caps(ch_id_t ch_id, void *msg,
                             cap_id_t *out_caps,
                             uint8_t *out_cap_count,
                             uint32_t timeout);
#endif
void      *channel_get_shm(ch_id_t ch_id);
void       channel_cleanup_task(void *channel_obj, tcb_t *tcb);

#endif /* CHANNEL_H */
