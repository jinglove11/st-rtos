/**
 * @file continuation.h
 * @brief M3-Task3: 统一阻塞 syscall continuation 状态机 helper
 *
 * 3 个统一入口,替代各子系统手动设 TCB 阻塞字段:
 * - syscall_block_current: SVC 路径阻塞,返回 KERN_SYSCALL_BLOCKED
 * - syscall_complete: waker 唤醒阻塞任务,写 saved frame R0
 * - syscall_cancel: timeout/delete/fault 取消阻塞,清理子系统 wait queue
 */

#ifndef CONTINUATION_H
#define CONTINUATION_H

#include "kernel_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * 统一 continuation helper
 *============================================================================*/

/**
 * @brief 阻塞当前任务 (SVC 路径用)
 *
 * 设 cont.active=1/op/object/deadline; 从 ready_list 移除; state=BLOCKED。
 * 不返回给调用者 — 返回 KERN_SYSCALL_BLOCKED 让 SVC handler 切任务。
 *
 * @param op      block_reason_t (BLOCK_REASON_EP_SEND 等)
 * @param object  等待的内核对象指针
 * @param timeout 超时 ticks (0=永久等待)
 * @return KERN_SYSCALL_BLOCKED (调用者直接 return)
 */
int syscall_block_current(uint8_t op, void *object, uint32_t timeout);

/**
 * @brief 唤醒阻塞任务
 *
 * 由 waker (sem_post/ep_reply/channel_send 等) 调用。
 * 写 result 到 saved SVC frame R0; 设 active=0; 加回 ready_list。
 *
 * @param tcb    要唤醒的任务
 * @param result 唤醒结果 (KERN_OK / KERN_ERR_TIMEOUT / KERN_ERR_NOEXIST)
 */
void syscall_complete(tcb_t *tcb, kern_err_t result);

/**
 * @brief 取消阻塞 (timeout/delete/fault 路径用)
 *
 * 按 cont.op 分发到子系统 cleanup_task; 清 active。
 * 在 syscall_complete 之前调 (先从子系统 wait queue 移除,再唤醒)。
 *
 * @param tcb    要取消的任务
 * @param result 取消原因 (KERN_ERR_TIMEOUT / KERN_ERR_NOEXIST)
 */
void syscall_cancel(tcb_t *tcb, kern_err_t result);

#ifdef __cplusplus
}
#endif

#endif /* CONTINUATION_H */
