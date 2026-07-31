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
 *
 * M3-Step1 闭环: phase 是独立原子字段 (cont.phase),所有转换用 CAS:
 * - prepare_locked:  CAS IDLE→ARMING (失败则强制覆盖,prepare 是唯一入口)
 * - commit:          CAS ARMING→BLOCKED (失败 → 返回 waker 的 pending result)
 * - complete/cancel: CAS ARMING→IDLE (记 pending) / CAS BLOCKED→COMPLETING
 *                    (赢得唤醒权,写 R0 + wakeup)
 * CAS 保证 commit vs complete/cancel、多 waker 之间恰好一个赢家。
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

    uint8_t expected = CONT_PHASE_IDLE;
    if (!CONT_PHASE_CAS(&current->cont, &expected, CONT_PHASE_ARMING)) {
        /* 上一阻塞的 phase 未归位 (节点 C 统一唤醒路径前可能残留)。
         * prepare 是进入阻塞的唯一入口,强制覆盖保证状态机可推进。 */
        CONT_PHASE_SET(&current->cont, CONT_PHASE_ARMING);
    }
    current->cont.active = 1;
    current->cont.op = op;
    current->cont.object = object;
    current->cont.result = KERN_OK;
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

    /* CAS ARMING→BLOCKED: 与 waker (complete/cancel) 竞争。
     * 输掉 → waker 已记录 pending result (phase 已离开 ARMING),
     * 直接返回它,不做 sched_remove_ready。 */
    uint8_t expected = CONT_PHASE_ARMING;
    if (!CONT_PHASE_CAS(&current->cont, &expected, CONT_PHASE_BLOCKED)) {
        return (int)current->cont.result;
    }

    /* 赢得 ARMING→BLOCKED: 设阻塞状态 */
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

    for (;;) {
        uint8_t phase = CONT_PHASE_GET(&tcb->cont);

        if (phase == CONT_PHASE_ARMING) {
            /* Task 还在 RUNNING (prepare 后 commit 前)。
             * CAS ARMING→IDLE: 与 commit 竞争。赢 → 记录 pending result
             * (commit 时读到并返回);输 → commit 已推进到 BLOCKED,重试。 */
            uint8_t expected = CONT_PHASE_ARMING;
            if (!CONT_PHASE_CAS(&tcb->cont, &expected, CONT_PHASE_IDLE)) {
                continue;
            }
            tcb->cont.result = result;
            return;
        }

        if (phase == CONT_PHASE_BLOCKED) {
            /* CAS BLOCKED→COMPLETING: 多个 waker (deliver/timeout/delete/
             * fault) 竞争,恰好一个赢。 */
            uint8_t expected = CONT_PHASE_BLOCKED;
            if (!CONT_PHASE_CAS(&tcb->cont, &expected,
                                CONT_PHASE_COMPLETING)) {
                continue;  /* 另一个 waker 已赢 → 重读 phase */
            }

            /* 赢得唤醒权: 写 R0 + result + 唤醒 + 归位 IDLE */
            if (tcb->cont.active) {
                task_complete_blocked_syscall(tcb, result);
            }
            tcb->cont.result = result;
            tcb->cont.active = 0;
            tcb->cont.deadline = 0;

            sched_wakeup(tcb, result);

            CONT_PHASE_SET(&tcb->cont, CONT_PHASE_IDLE);
            return;
        }

        /* IDLE 或 COMPLETING: 已处理过,不重复唤醒。 */
        return;
    }
}

/*============================================================================
 * 取消 (timeout/delete/fault 调) — 复用公开 API
 *============================================================================*/

void syscall_cont_cancel(tcb_t *tcb, kern_err_t result) {
    if (tcb == NULL) {
        return;
    }

    for (;;) {
        uint8_t phase = CONT_PHASE_GET(&tcb->cont);

        if (phase == CONT_PHASE_ARMING) {
            /* Task 还在 running: CAS ARMING→IDLE,记录 pending result。 */
            uint8_t expected = CONT_PHASE_ARMING;
            if (!CONT_PHASE_CAS(&tcb->cont, &expected, CONT_PHASE_IDLE)) {
                continue;
            }
            tcb->cont.result = result;
            return;
        }

        if (phase != CONT_PHASE_BLOCKED) {
            /* IDLE / COMPLETING: 已处理。 */
            return;
        }

        /* CAS BLOCKED→COMPLETING: 赢得取消权 (与 deliver 等 waker 竞争) */
        uint8_t expected = CONT_PHASE_BLOCKED;
        if (!CONT_PHASE_CAS(&tcb->cont, &expected, CONT_PHASE_COMPLETING)) {
            continue;
        }

        /* 从子系统 wait queue 移除 (公开 API,按 cont.op 分派) */
        task_cancel_blocked_wait(tcb);

        if (tcb->cont.active) {
            task_complete_blocked_syscall(tcb, result);
        }
        tcb->cont.result = result;
        tcb->cont.active = 0;
        tcb->cont.deadline = 0;

        sched_wakeup(tcb, result);

        CONT_PHASE_SET(&tcb->cont, CONT_PHASE_IDLE);
        return;
    }
}
