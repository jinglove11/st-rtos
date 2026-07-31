/**
 * @file continuation.c
 * @brief M3-Task3: 统一阻塞 syscall continuation — 两阶段协议实现
 *
 * 状态机: IDLE → ARMING → BLOCKED → COMPLETING → IDLE
 *
 * ARMING:  prepare_locked 设 active+op+object+ARMING。waker 看到 ARMING
 *          时记录 pending result 但不唤醒 (task 还在 running,不该入 ready)。
 * BLOCKED: commit 把 ARMING→BLOCKED + sched_remove_ready + return -128。
 * COMPLETING: complete CAS BLOCKED→COMPLETING,写 R0,wakeup→IDLE。
 */

#include "continuation.h"
#include "scheduler.h"
#include "task.h"
#include "hal.h"
#include "syscall.h"

/*============================================================================
 * 阶段 1: 持对象锁内调 — IDLE → ARMING
 *============================================================================*/

void syscall_cont_prepare_locked(uint8_t op, void *object) {
    tcb_t *current = sched_get_current();
    if (current == NULL) {
        return;
    }
    current->cont.active = 1;
    current->cont.op = op;
    current->cont.object = object;
    current->cont.result = KERN_OK;
    CONT_PHASE_SET(&current->cont, CONT_PHASE_ARMING);
    /* 不设 state=BLOCKED:ARMING 阶段 task 仍在 RUNNING。
     * commit 时才 ARMING→BLOCKED。 */
}

/*============================================================================
 * 阶段 2: 释放对象锁后调 — ARMING → BLOCKED
 *============================================================================*/

int syscall_cont_commit(uint32_t timeout) {
    tcb_t *current = sched_get_current();
    if (current == NULL) {
        return (int)KERN_ERR_STATE;
    }

    /* 如果 ARMING 期间被 waker 抢先处理 (phase 已回到 IDLE/COMPLETING),
     * 说明 waker 记录了 pending result 但 task 还在 running。
     * 直接返回 pending result,不做 sched_remove_ready。 */
    if (CONT_PHASE_GET(&current->cont) != CONT_PHASE_ARMING) {
        return (int)current->cont.result;
    }

    /* ARMING → BLOCKED */
    CONT_PHASE_SET(&current->cont, CONT_PHASE_BLOCKED);
    current->state = TASK_STATE_BLOCKED;

    /* 设超时 */
    if (timeout > 0) {
        current->cont.deadline = sched_timeout_deadline(timeout);
    } else {
        current->cont.deadline = 0;
    }

    /* 从就绪队列移除 */
    sched_remove_ready(current);

    return KERN_SYSCALL_BLOCKED;
}

/*============================================================================
 * 唤醒 (waker 调) — BLOCKED → COMPLETING → IDLE
 *============================================================================*/

void syscall_cont_complete(tcb_t *tcb, kern_err_t result) {
    if (tcb == NULL) {
        return;
    }

    uint8_t phase = CONT_PHASE_GET(&tcb->cont);

    if (phase == CONT_PHASE_ARMING) {
        /* Task 还在 RUNNING (prepare 后 commit 前)。
         * 只记录 pending result,等 commit 时返回。
         * 不能 sched_wakeup (task 还在 running)。 */
        tcb->cont.result = result;
        CONT_PHASE_SET(&tcb->cont, CONT_PHASE_IDLE);
        return;
    }

    if (phase != CONT_PHASE_BLOCKED) {
        /* IDLE 或 COMPLETING:已经处理过了,不重复唤醒。 */
        return;
    }

    /* BLOCKED → COMPLETING:写 R0 + 唤醒。
     * sched_wakeup 内部的 CAS (BLOCKED→READY) 保证只唤醒一次。 */
    CONT_PHASE_SET(&tcb->cont, CONT_PHASE_COMPLETING);

    if (tcb->cont.active) {
        task_complete_blocked_syscall(tcb, result);
    }
    tcb->cont.result = result;
    tcb->cont.active = 0;
    tcb->cont.deadline = 0;

    sched_wakeup(tcb, result);

    CONT_PHASE_SET(&tcb->cont, CONT_PHASE_IDLE);
}

/*============================================================================
 * 取消 (timeout/delete/fault 调) — 复用公开 API
 *============================================================================*/

void syscall_cont_cancel(tcb_t *tcb, kern_err_t result) {
    if (tcb == NULL) {
        return;
    }

    uint8_t phase = CONT_PHASE_GET(&tcb->cont);

    if (phase == CONT_PHASE_ARMING) {
        /* Task 还在 running,只记录 pending result。 */
        tcb->cont.result = result;
        CONT_PHASE_SET(&tcb->cont, CONT_PHASE_IDLE);
        return;
    }

    if (tcb->state != TASK_STATE_BLOCKED) {
        return;
    }

    /* 用公开 API 取消 (从子系统 wait queue 移除) */
    task_cancel_blocked_wait(tcb);
    syscall_cont_complete(tcb, result);
}
