/**
 * @file pendsv_handler.S
 * @brief PendSV 上下文切换
 *
 * PendSV（Pendable Service Call）是 ARM Cortex-M 的特殊异常：
 * - 可编程优先级（设为最低，不打断其他中断）
 * - 可延迟执行（通过设置 PENDSVSET 位）
 *
 * 切换流程：
 *   1. 禁用中断 (cpsid i)
 *   2. 获取 _current_task TCB
 *   3. 检查是否为首次切换 (current == NULL → 跳过保存)
 *   4. 检查任务状态是否为 TERMINATED (跳过保存)
 *   5. 保存 R4-R11、EXC_RETURN 和可选的 S16-S31
 *   6. 调用 kern_pendsv_handler() 选择下一个任务
 *   7. 加载下一个任务的 MPU regions
 *   8. 根据 next_task->attrs 更新 CONTROL (特权级切换)
 *   9. 恢复下一个任务的 R4-R11
 *  10. 使能中断 + 异常返回
 */

    .syntax unified
    .thumb

    .extern _current_task
    .extern _next_task
    .extern kern_pendsv_handler
    .extern mpu_load_task_regions

#include "kernel_config.h"
#include "tcb_offsets.inc"

/* SIO_BASE for CPUID read (RP2350) */
.equ SIO_BASE, 0xd0000000

    .section .text
    .global PendSV_Handler
    .global isr_pendsv
    .type PendSV_Handler, %function
    .align 2

/* Macro: load CPU id (0 or 1) into \reg, then compute byte offset into a
 * tcb_t* array (cpu_id * 4) into \off. */
.macro GET_CPU_OFFSET reg, off
#if SMP
    ldr     \reg, =SIO_BASE
    ldr     \reg, [\reg]            /* sio_hw->cpuid (0 or 1) */
    lsl     \off, \reg, #2          /* byte offset: cpu_id * sizeof(void*) */
#else
    movs    \reg, #0
    movs    \off, #0                /* UP has one per-CPU array entry */
#endif
.endm

PendSV_Handler:
    cpsid   i

    GET_CPU_OFFSET r3, r2           /* r3=cpu_id, r2=byte offset */
    ldr     r0, =_current_task
    ldr     r0, [r0, r2]            /* _current_task[cpu_id] */

    cmp     r0, #0
    beq     .L_select_next

    ldrb    r1, [r0, #OFF_STATE]   /* current->state */
    cmp     r1, #5                 /* TASK_STATE_TERMINATED? */
    beq     .L_select_next

    mrs     r1, psp
    stmdb   r1!, {r4-r11}
    str     r1, [r0, #OFF_SP]     /* TCB->sp = PSP */
    str     lr, [r0, #OFF_EXC_RETURN]

#if defined(__ARM_FP) && (__ARM_FP != 0)
    /* Extended hardware frames save S0-S15/FPSCR.  The ABI makes S16-S31
     * callee-saved, so the scheduler must preserve those per task. */
    tst     lr, #0x10
    bne     .L_no_fp_save
    add     r2, r0, #OFF_FP_HIGH
    vstmia  r2, {s16-s31}
.L_no_fp_save:
#endif

    /* Save current PSPLIM so it is restored on context-in. */
    mrs     r2, psplim
    str     r2, [r0, #OFF_SP_LIMIT]

.L_select_next:
    push    {lr}
    bl      kern_pendsv_handler
    pop     {lr}

    /* Recompute CPU offset (r2 was clobbered by PSPLIM save above) */
    GET_CPU_OFFSET r3, r2
    ldr     r0, =_next_task
    ldr     r0, [r0, r2]            /* _next_task[cpu_id] */
    cmp     r0, #0
    beq     .L_pendsv_halt

    push    {r0, lr}
    bl      mpu_load_task_regions
    pop     {r0, lr}

    ldr     lr, [r0, #OFF_EXC_RETURN]
    ldrb    r2, [r0, #OFF_ATTRS]   /* next_task->attrs */
    tst     r2, #1                 /* TASK_ATTR_USER? */
    ite     eq
    moveq   r2, #2                 /* 特权: SPSEL=1, nPRIV=0 */
    movne   r2, #3                 /* 用户: SPSEL=1, nPRIV=1 */
#if defined(__ARM_FP) && (__ARM_FP != 0)
    /* CONTROL.FPCA must describe the task being restored, not the task that
     * was interrupted.  Extended EXC_RETURN frames require FPCA=1. */
    tst     lr, #0x10
    it      eq
    orreq   r2, r2, #4
#endif
    msr     control, r2
    isb

    ldr     r1, [r0, #OFF_SP]     /* next->sp */

#if defined(__ARM_FP) && (__ARM_FP != 0)
    tst     lr, #0x10
    bne     .L_no_fp_restore
    add     r3, r0, #OFF_FP_HIGH
    vldmia  r3, {s16-s31}
.L_no_fp_restore:
#endif

    ldmia   r1!, {r4-r11}
    msr     psp, r1

    /* Restore next task's PSPLIM (M33 stack-limit). */
    ldr     r2, [r0, #OFF_SP_LIMIT]
    msr     psplim, r2

    cpsie   i
    bx      lr

.L_pendsv_halt:
    b       .

    .size PendSV_Handler, . - PendSV_Handler
    .thumb_set isr_pendsv, PendSV_Handler
    .end
