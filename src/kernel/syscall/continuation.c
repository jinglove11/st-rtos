/**
 * @file continuation.c
 * @brief M3-Task3: 统一阻塞 syscall continuation — 两阶段协议实现
 */

#include "continuation.h"
#include "scheduler.h"
#include "task.h"
#include "hal.h"
#include "syscall.h"

/*============================================================================
 * 阶段 1: 持对象锁内调
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
    /* 在持锁内就设 BLOCKED:让 waker 的 sched_wakeup CAS (BLOCKED→READY)
     * 能成功,即使 commit 还没执行。 */
    current->state = TASK_STATE_BLOCKED;
}

/*============================================================================
 * 阶段 2: 释放对象锁后调
 *============================================================================*/

int syscall_cont_commit(uint32_t timeout) {
    tcb_t *current = sched_get_current();
    if (current == NULL) {
        return (int)KERN_ERR_STATE;
    }

    /* 设超时 */
    if (timeout > 0) {
        extern uint32_t sched_timeout_deadline(uint32_t timeout);
        current->cont.deadline = sched_timeout_deadline(timeout);
    } else {
        current->cont.deadline = 0;
    }

    /* 从就绪队列移除 (state=BLOCKED 已在 prepare_locked 设) */
    extern void sched_remove_ready(tcb_t *tcb);
    sched_remove_ready(current);

    return KERN_SYSCALL_BLOCKED;
}

/*============================================================================
 * 唤醒 (waker 调)
 *============================================================================*/

void syscall_cont_complete(tcb_t *tcb, kern_err_t result) {
    if (tcb == NULL) {
        return;
    }

    /* 如果是 SVC 阻塞,写 result 到 saved frame R0 */
    if (tcb->cont.active) {
        extern void task_write_saved_svc_r0(tcb_t *tcb, kern_err_t result);
        task_write_saved_svc_r0(tcb, result);
    }
    tcb->cont.active = 0;
    tcb->cont.result = result;
    tcb->cont.deadline = 0;

    sched_wakeup(tcb, result);
}

/*============================================================================
 * 取消 (timeout/delete/fault 调)
 *============================================================================*/

void syscall_cont_cancel(tcb_t *tcb, kern_err_t result) {
    if (tcb == NULL || tcb->state != TASK_STATE_BLOCKED) {
        return;
    }

    /* 用公开 API 取消 (不依赖 static 函数) */
    task_cancel_blocked_wait(tcb);

    syscall_cont_complete(tcb, result);
}
