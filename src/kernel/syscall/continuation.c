/**
 * @file continuation.c
 * @brief M3-Task3: 统一阻塞 syscall continuation — 两阶段协议实现
 *
 * 状态机: IDLE → ARMING → BLOCKED → COMPLETING → IDLE
 *
 * ARMING:  prepare_locked 设 active+op+object+ARMING。waker 看到 ARMING
 *          时记录 pending result 但不唤醒 (task 还在 running,不该入 ready)。
 * BLOCKED: commit 把 ARMING→BLOCKED,顺序 deadline→出队→置 BLOCKED,
 *          return -128。
 * COMPLETING: complete CAS BLOCKED→COMPLETING,写 R0,归位 IDLE,
 *          再 sched_wakeup (IDLE 必须先于 wakeup,防滞后覆盖新一轮 phase)。
 *
 * M3-Step1 闭环: phase 是独立原子字段 (cont.phase),所有转换用 CAS:
 * - prepare_locked:  CAS IDLE→ARMING (失败则强制覆盖,prepare 是唯一入口)
 * - commit:          CAS ARMING→BLOCKED (失败 → 返回 waker 的 pending result)
 * - complete/cancel: CAS ARMING→IDLE (记 pending) / CAS BLOCKED→COMPLETING
 *                    (赢得唤醒权,写 R0 + wakeup)
 * CAS 保证 commit vs complete/cancel、多 waker 之间恰好一个赢家。
 *
 * 唤醒分派 (syscall_cont_wake) 按原子 phase,不按非原子 active;
 * sched_block 线程式阻塞同样置 phase=BLOCKED,由 complete 的 active
 * 守卫跳过 R0 写入后走同一条唤醒路径。
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
        /* phase 残留 (sched_block 线程式轮次未归位、或历史脏状态)。
         * 正常路径 complete/cancel 均在 wakeup 前归位 IDLE,这里只是
         * 防御性兜底: prepare 是进入阻塞的唯一入口,强制覆盖保证
         * 状态机可推进。 */
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
        /* 本轮未真正阻塞,清掉 ARMING 残留: active=1 滞留会让
         * syscall_cont_wake 把后续线程式阻塞 (mutex/join) 误路由进
         * complete → 唤醒丢失或往线程栈写 "R0"。 */
        current->cont.active = 0;
        current->cont.op = BLOCK_REASON_NONE;
        current->cont.object = NULL;
        return (int)current->cont.result;
    }

    /* 赢得 ARMING→BLOCKED。顺序必须是 deadline → 出队 → 置 BLOCKED:
     * - state=BLOCKED 而仍在就绪队列的窗口内,任务可被本核 tick 或对核
     *   steal 选中 (runq_get_highest/steal 不校验 state) → 同任务双跑;
     * - deadline 先于 state,超时扫描器看到 BLOCKED 时 deadline 已就绪。
     * 临界区压缩本核窗口;state 用 release 原子写,与扫描器的
     * 普通读跨核配对。 */
    uint32_t crit = hal_irq_save();
    current->cont.deadline = (timeout > 0U) ? sched_timeout_deadline(timeout)
                                            : 0U;
    sched_remove_ready(current);
    __atomic_store_n(&current->state, TASK_STATE_BLOCKED, __ATOMIC_RELEASE);
    hal_irq_restore(crit);

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
             * 先写 result 再 CAS ARMING→IDLE: commit 的 CAS (acquire)
             * 与这里 CAS (release) 同步,保证 pending result 对 commit
             * 可见 (写在 CAS 之后不保证)。输给 commit → 重试。 */
            tcb->cont.result = result;
            uint8_t expected = CONT_PHASE_ARMING;
            if (!CONT_PHASE_CAS(&tcb->cont, &expected, CONT_PHASE_IDLE)) {
                continue;
            }
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

            /* 赢得唤醒权: 写 R0 + result,归位 IDLE,再唤醒。
             * IDLE 必须在 sched_wakeup 之前归位: wakeup 后 task 可立即
             * 在另一核运行、返回用户态并再次阻塞 (prepare 设新 phase),
             * 滞后的 SET 会把新 phase 覆盖回 IDLE → 唤醒永久丢失。
             * state 仍为 BLOCKED 期间 task 不可能执行,提前归位安全;
             * [SET(IDLE), state CAS] 窗口内晚到的 waker 走 sched_wakeup
             * 旁路,state CAS 恰好一个赢家,不会双重入队。 */
            if (tcb->cont.active) {
                task_complete_blocked_syscall(tcb, result);
            }
            tcb->cont.result = result;
            tcb->cont.active = 0;
            tcb->cont.deadline = 0;

            CONT_PHASE_SET(&tcb->cont, CONT_PHASE_IDLE);
            sched_wakeup(tcb, result);
            return;
        }

        /* IDLE 或 COMPLETING: 已处理过,不重复唤醒。 */
        return;
    }
}

/*============================================================================
 * 统一唤醒入口 (M3-Step1闭环-C)
 *
 * 按原子 phase (acquire) 分派,不用非原子的 cont.active:
 * - ARMING:     SVC 两阶段 prepare 后 commit 前,complete 记 pending result;
 * - BLOCKED:    SVC 已 commit 或 sched_block 线程式阻塞 (complete 内
 *               active 守卫决定是否写 R0,两条路都收敛);
 * - COMPLETING: 已有 waker 在处理,complete 重读后幂等返回;
 * - IDLE:       未进 phase 机 (task_join 等旧式手工阻塞),直接
 *               sched_wakeup (CAS state,不依赖 phase)。
 * active 在 complete 里于 wakeup 前清零,ARM 弱序下跨核读它既过期又
 * 无序 (可能 state 见 BLOCKED 而 active 读到 0 → R0 漏写);
 * phase 的 CAS/SET 都是 acquire/release,天然有序。
 *============================================================================*/

void syscall_cont_wake(tcb_t *tcb, kern_err_t result) {
    if (tcb == NULL) {
        return;
    }
    if (CONT_PHASE_GET(&tcb->cont) != CONT_PHASE_IDLE) {
        syscall_cont_complete(tcb, result);
    } else {
        sched_wakeup(tcb, result);
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
            /* Task 还在 running: 先写 result 再 CAS ARMING→IDLE
             * (pending result 对 commit 可见,见 complete 注释)。 */
            tcb->cont.result = result;
            uint8_t expected = CONT_PHASE_ARMING;
            if (!CONT_PHASE_CAS(&tcb->cont, &expected, CONT_PHASE_IDLE)) {
                continue;
            }
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

        /* IDLE 在 sched_wakeup 之前归位 (同 complete,防滞后覆盖) */
        CONT_PHASE_SET(&tcb->cont, CONT_PHASE_IDLE);
        sched_wakeup(tcb, result);
        return;
    }
}
