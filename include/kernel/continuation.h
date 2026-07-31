/**
 * @file continuation.h
 * @brief M3-Task3: 统一阻塞 syscall continuation 状态机 — 两阶段协议
 *
 * 状态机: IDLE → ARMING → BLOCKED → COMPLETING → IDLE
 *
 * 1. 持对象锁调 syscall_cont_prepare_locked():
 *    IDLE → ARMING。设 active/op/object。Task 仍在 RUNNING。
 *    子系统紧接着 wait_queue_add (仍持锁)。
 *    如果 waker 在此阶段看到 task (ARMING),只记录 pending result,
 *    不唤醒 (task 还在 running)。
 *
 * 2. 释放对象锁后调 syscall_cont_commit():
 *    ARMING → BLOCKED。设 deadline/state=BLOCKED/sched_remove_ready。
 *    如果 ARMING 期间已被 waker 记录 pending result,直接返回它。
 *    否则返回 KERN_SYSCALL_BLOCKED。
 *
 * 3. waker 调 syscall_cont_complete():
 *    BLOCKED → COMPLETING → IDLE。CAS 保证只唤醒一次。
 *
 * 4. timeout/delete/fault 调 syscall_cont_cancel():
 *    复用 task_cancel_blocked_wait() 从子系统 wait queue 移除,
 *    然后 complete。
 */

#ifndef CONTINUATION_H
#define CONTINUATION_H

#include "kernel_types.h"

#ifdef __cplusplus
extern "C" {
#endif

void syscall_cont_prepare_locked(uint8_t op, void *object);
int  syscall_cont_commit(uint32_t timeout);
void syscall_cont_complete(tcb_t *tcb, kern_err_t result);
void syscall_cont_cancel(tcb_t *tcb, kern_err_t result);

/* M3-Task3: continuation 阶段 (存在 cont.flags 低字节)。
 * 复杂子系统 (endpoint/channel) 的同步 deliver 路径用这些宏手动
 * 管理阶段,不走 commit。 */
#define CONT_PHASE_IDLE        0
#define CONT_PHASE_ARMING      1
#define CONT_PHASE_BLOCKED     2
#define CONT_PHASE_COMPLETING  3

#define CONT_PHASE_SET(c, p)  do { (c)->flags = ((c)->flags & 0xFF00) | (p); } while (0)
#define CONT_PHASE_GET(c)     ((c)->flags & 0xFF)

#ifdef __cplusplus
}
#endif

#endif /* CONTINUATION_H */
