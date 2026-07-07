/**
 * @file endpoint.h
 * @brief Endpoint (C/S) 消息传递 — 多对一模型
 */

#ifndef ENDPOINT_H
#define ENDPOINT_H

#include "kernel_types.h"

#define ENDPOINT_NAME_LEN   16

#include "ipc_transfer.h"

void endpoint_init(void);

ep_id_t    endpoint_create(const char *name, uint16_t msg_size, uint16_t max_pending);
kern_err_t endpoint_delete(ep_id_t ep_id);
int        endpoint_exists(ep_id_t ep_id);
uint16_t   endpoint_msg_size(ep_id_t ep_id);
kern_err_t endpoint_send(ep_id_t ep_id, void *msg, uint32_t timeout);
kern_err_t endpoint_notify(ep_id_t ep_id, const void *msg);
kern_err_t endpoint_send_syscall(ep_id_t ep_id,
                                 const void *msg,
                                 void *user_reply_msg,
                                 uint32_t timeout);
#if CAP_ENABLE
kern_err_t endpoint_send_caps_syscall(ep_id_t ep_id,
                                      const void *msg,
                                      void *user_reply_msg,
                                      const ipc_cap_xfer_t *caps,
                                      uint8_t cap_count,
                                      uint32_t timeout);
kern_err_t endpoint_send_caps(ep_id_t ep_id,
                              void *msg,
                              const ipc_cap_xfer_t *caps,
                              uint8_t cap_count,
                              uint32_t timeout);
#endif
kern_err_t endpoint_recv(ep_id_t ep_id, void *msg, uint32_t timeout);
kern_err_t endpoint_recv_syscall(ep_id_t ep_id, void *user_msg, uint32_t timeout);
#if CAP_ENABLE
kern_err_t endpoint_recv_caps_syscall(ep_id_t ep_id,
                                      void *user_msg,
                                      cap_id_t *out_caps,
                                      uint8_t *out_cap_count,
                                      uint32_t timeout);
kern_err_t endpoint_recv_caps(ep_id_t ep_id,
                              void *msg,
                              cap_id_t *out_caps,
                              uint8_t *out_cap_count,
                              uint32_t timeout);
#endif
kern_err_t endpoint_reply(ep_id_t ep_id, const void *msg);
#if CAP_ENABLE
cap_id_t   endpoint_take_reply_cap(ep_id_t ep_id);
kern_err_t endpoint_reply_cap(void *reply_obj, const void *msg);
#endif
void       endpoint_cleanup_task(void *endpoint_obj, tcb_t *tcb);

#endif /* ENDPOINT_H */
