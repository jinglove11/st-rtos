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

/* M3-Step1 闭环: phase 是独立原子字段 (cont.phase),
 * 所有阶段转换用原子操作完成:
 * - GET/SET: 原子 load/store (release/acquire)
 * - CAS: __atomic_compare_exchange (ACQ_REL),用于 ARMING/BLOCKED
 *   转换点的竞争 (commit vs complete/cancel, 多 waker 互斥)。
 * 复杂子系统 (endpoint/channel) 的同步 deliver 路径暂用 SET 手动
 * 管理阶段 (节点 B 统一到 prepare/commit 后移除)。 */
#define CONT_PHASE_IDLE        0
#define CONT_PHASE_ARMING      1
#define CONT_PHASE_BLOCKED     2
#define CONT_PHASE_COMPLETING  3

#define CONT_PHASE_GET(c) \
    __atomic_load_n(&(c)->phase, __ATOMIC_ACQUIRE)
#define CONT_PHASE_SET(c, p) \
    __atomic_store_n(&(c)->phase, (p), __ATOMIC_RELEASE)
#define CONT_PHASE_CAS(c, exp, val) \
    __atomic_compare_exchange_n(&(c)->phase, (exp), (val), 0, \
                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)

#ifdef __cplusplus
}
#endif

#endif /* CONTINUATION_H */
