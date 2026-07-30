/**
 * @file continuation.h
 * @brief M3-Task3: 统一阻塞 syscall continuation 状态机 — 两阶段协议
 *
 * 两阶段设计解决 SMP 唤醒竞态:
 *
 * 1. 持对象锁调 syscall_cont_prepare_locked():
 *    - 设 cont.active=1/op/object/result
 *    - 子系统紧接着 wait_queue_add (仍持锁)
 *    - 此时 waker 在另一核上被同一把锁挡住,看不到半初始化状态
 *
 * 2. 释放对象锁后调 syscall_cont_commit():
 *    - 设 cont.deadline
 *    - sched_remove_ready + state=BLOCKED
 *    - return KERN_SYSCALL_BLOCKED (调用者直接 return)
 *
 * 如果 waker 在阶段 1 和 2 之间获锁:
 *    - waker 看到 cont.active=1 + task 在 wait_queue → 正常唤醒
 *    - syscall_cont_commit 的 sched_remove_ready 不会移除已唤醒的 task
 *      (sched_wakeup 的 CAS 保证只唤醒一次)
 *
 * complete/cancel 由 waker/timeout 调用,不需拆阶段。
 */

#ifndef CONTINUATION_H
#define CONTINUATION_H

#include "kernel_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * 两阶段阻塞协议
 *============================================================================*/

/**
 * @brief 阶段 1: 持对象锁内调 — 设 continuation 状态
 *
 * 设 cont.active=1/op/object/result。调用方紧接着 wait_queue_add
 * (仍持锁)。然后释放锁,调 syscall_cont_commit()。
 *
 * @param op      block_reason_t
 * @param object  等待的内核对象
 */
void syscall_cont_prepare_locked(uint8_t op, void *object);

/**
 * @brief 阶段 2: 释放对象锁后调 — 完成阻塞提交
 *
 * 设 cont.deadline; sched_remove_ready; state=BLOCKED。
 * 返回 KERN_SYSCALL_BLOCKED。
 *
 * @param timeout 超时 ticks (0=永久)
 * @return KERN_SYSCALL_BLOCKED (调用者直接 return)
 */
int syscall_cont_commit(uint32_t timeout);

/*============================================================================
 * 唤醒/取消 (waker 和 timeout/fault 路径用)
 *============================================================================*/

/**
 * @brief 唤醒阻塞任务 (waker 调)
 *
 * 如果 cont.active==1,写 result 到 saved SVC frame R0。
 * 设 active=0; sched_wakeup。
 *
 * @param tcb    要唤醒的任务
 * @param result 唤醒结果
 */
void syscall_cont_complete(tcb_t *tcb, kern_err_t result);

/**
 * @brief 取消阻塞 (timeout/delete/fault 调)
 *
 * 按 cont.op 分发子系统 cleanup; 然后 complete。
 * 使用公开的 task_cancel_blocked_wait() (不依赖 static 函数)。
 *
 * @param tcb    要取消的任务
 * @param result 取消原因
 */
void syscall_cont_cancel(tcb_t *tcb, kern_err_t result);

#ifdef __cplusplus
}
#endif

#endif /* CONTINUATION_H */
