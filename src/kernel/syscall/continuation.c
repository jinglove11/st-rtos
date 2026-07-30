/**
 * @file continuation.c
 * @brief M3-Task3: 统一阻塞 syscall continuation 状态机 helper 实现
 */

#include "continuation.h"
#include "scheduler.h"
#include "task.h"
#include "hal.h"
#include "syscall.h"

/*============================================================================
 * syscall_block_current — SVC 路径阻塞当前任务
 *============================================================================*/

int syscall_block_current(uint8_t op, void *object, uint32_t timeout) {
    tcb_t *current = sched_get_current();
    if (current == NULL) {
        return (int)KERN_ERR_STATE;
    }

    /* 设 continuation 状态 */
    current->cont.active = 1;
    current->cont.op = op;
    current->cont.object = object;
    current->cont.result = KERN_OK;
    if (timeout > 0) {
        extern uint32_t sched_timeout_deadline(uint32_t timeout);
        current->cont.deadline = sched_timeout_deadline(timeout);
    } else {
        current->cont.deadline = 0;
    }

    /* 从就绪队列移除 + 设阻塞状态 */
    {
        extern void sched_remove_ready(tcb_t *tcb);
        sched_remove_ready(current);
    }
    current->state = TASK_STATE_BLOCKED;

    /* 返回 KERN_SYSCALL_BLOCKED — SVC handler 会切到下一个任务。
     * waker 后续通过 syscall_complete 写 result 到 saved frame R0。 */
    return KERN_SYSCALL_BLOCKED;
}

/*============================================================================
 * syscall_complete — waker 唤醒阻塞任务
 *============================================================================*/

void syscall_complete(tcb_t *tcb, kern_err_t result) {
    if (tcb == NULL) {
        return;
    }

    /* 如果是 SVC 阻塞 (active=1),写 result 到 saved frame R0 */
    if (tcb->cont.active) {
        extern void task_write_saved_svc_r0(tcb_t *tcb, kern_err_t result);
        task_write_saved_svc_r0(tcb, result);
    }
    tcb->cont.active = 0;
    tcb->cont.result = result;
    tcb->cont.deadline = 0;

    /* 加入就绪队列 (sched_wakeup 会 CAS state BLOCKED→READY) */
    sched_wakeup(tcb, result);
}

/*============================================================================
 * syscall_cancel — timeout/delete/fault 取消阻塞
 *============================================================================*/

void syscall_cancel(tcb_t *tcb, kern_err_t result) {
    if (tcb == NULL || tcb->state != TASK_STATE_BLOCKED) {
        return;
    }

    /* 按 op 分发到子系统 cleanup (从 wait queue 移除) */
    extern kern_err_t task_unlink_blocked(tcb_t *tcb);
    (void)task_unlink_blocked(tcb);

    /* 唤醒 */
    syscall_complete(tcb, result);
}
