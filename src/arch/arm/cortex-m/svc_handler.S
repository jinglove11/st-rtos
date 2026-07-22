/**
 * @file svc_handler.S
 * @brief SVC 异常处理 — 首次任务启动 + 通用系统调用
 *
 * SVC（Supervisor Call）处理两个服务：
 *
 * 1. SVC #0：首次任务启动
 *    - 从 _next_task 恢复上下文
 *    - 根据任务 attrs 选择 CONTROL (特权=2, 用户=3)
 *    - 切换到线程模式 + PSP
 *
 * 2. SVC #1：通用系统调用
 *    - R0 = syscall 编号, R1-R3 = a1-a3
 *    - a4-a6 由用户 wrapper 压在 PSP 栈顶
 *    - 保存用户上下文 → 调用 kern_syscall_handler → 恢复
 *    - 返回值写入异常栈帧中的 R0
 *
 * SVC #1 栈布局:
 *
 *   入 SVC 时硬件异常帧位于 PSP:
 *     +0  R0(syscall_num), +4 R1(a1), +8 R2(a2), +12 R3(a3)
 *     +16 R12, +20 LR, +24 PC, +28 xPSR
 *
 *   本 handler 先在异常帧下方压入 R4-R11，并把 PSP 指向保存区:
 *     +0..+28   saved R4-R11
 *     +32..+60  hardware exception frame
 *     +64       a4
 *     +68       a5
 *     +72       a6
 *
 *   basic/extended frame 的 core frame 都从异常后 PSP 开始；extended
 *   frame 的 S0-S15/FPSCR 位于 core frame 之后。保存 R4-R11 后，R0
 *   地址始终是 PSP+32。额外参数在 basic frame 时从 PSP+64 开始，
 *   extended frame 时仅额外参数位置再增加 72 字节。
 */

    .syntax unified
    .thumb

    .extern _current_task
    .extern _next_task
    .extern kern_syscall_handler
    .extern kern_pendsv_handler
    .extern mpu_load_task_regions

#include "kernel_config.h"
#include "tcb_offsets.inc"

/* SIO_BASE for CPUID read (RP2350) */
.equ SIO_BASE, 0xd0000000

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

    .section .text
    .global SVC_Handler
    .global isr_svcall
    .type SVC_Handler, %function
    .align 2

SVC_Handler:
    /*
     * 提取 SVC 号。
     *
     * 不能用 PSP 是否为 0 判断异常帧位置：首次切换前 PSP 可能已有非零旧值。
     * EXC_RETURN(LR) bit2 才是硬件给出的栈选择：0=MSP, 1=PSP。
     */
    tst     lr, #4
    ite     eq
    mrseq   r12, msp
    mrsne   r12, psp
    ldr     r12, [r12, #24]       /* stacked PC */
    ldrb    r12, [r12, #-2]        /* SVC 立即数 */

    cmp     r12, #0
    beq     .L_maybe_first_switch

    cmp     r12, #1
    beq     .L_general_syscall

    b       .L_bad_svc_param

/*============================================================================
 * SVC #0: 首次任务启动
 *============================================================================*/
.L_maybe_first_switch:
    GET_CPU_OFFSET r1, r2          /* r1=cpu_id, r2=byte offset */
    ldr     r3, =_current_task
    ldr     r3, [r3, r2]           /* _current_task[cpu_id] */
    cmp     r3, #0
    bne     .L_bad_svc_state
    b       .L_first_switch

.L_first_switch:
    /* r2 still holds byte offset */
    ldr     r0, =_next_task
    ldr     r0, [r0, r2]           /* _next_task[cpu_id] */
    ldr     r1, [r0, #OFF_SP]     /* R1 = first_task->sp */

    ldmia   r1!, {r4-r11}
    msr     psp, r1

    /* First task gets its PSPLIM installed before running. */
    ldr     r2, [r0, #OFF_SP_LIMIT]
    msr     psplim, r2

    push    {r0, lr}
    bl      mpu_load_task_regions
    pop     {r0, lr}

    ldr     lr, [r0, #OFF_EXC_RETURN]
    ldrb    r2, [r0, #OFF_ATTRS]   /* TCB->attrs */
    tst     r2, #1                 /* TASK_ATTR_USER? */
    ite     eq
    moveq   r2, #2
    movne   r2, #3
#if defined(__ARM_FP) && (__ARM_FP != 0)
    tst     lr, #0x10
    it      eq
    orreq   r2, r2, #4
#endif
    msr     control, r2
    isb

    /* After first switch: set _current_task[cpu] = _next_task[cpu] */
    GET_CPU_OFFSET r2, r3           /* r2=cpu_id, r3=byte offset */
    ldr     r1, =_current_task
    ldr     r0, =_next_task
    ldr     r0, [r0, r3]
    str     r0, [r1, r3]

    cpsie   i
    bx      lr

/*============================================================================
 * SVC #1: 通用系统调用
 *============================================================================*/
.L_general_syscall:
    mrs     r12, psp
    stmdb   r12!, {r4-r11}
    msr     psp, r12

    GET_CPU_OFFSET r4, r5           /* r4=cpu_id, r5=byte offset */
    ldr     r3, =_current_task
    ldr     r4, [r3, r5]           /* _current_task[cpu_id] */
    cmp     r4, #0
    beq     1f
    str     r12, [r4, #OFF_SP]    /* TCB->sp = PSP */
    str     lr, [r4, #OFF_EXC_RETURN]
#if defined(__ARM_FP) && (__ARM_FP != 0)
    tst     lr, #0x10
    bne     .L_svc_no_fp_save
    add     r2, r4, #OFF_FP_HIGH
    vstmia  r2, {s16-s31}
.L_svc_no_fp_save:
#endif
    /* Save PSPLIM alongside R4-R11 so blocked syscalls switch cleanly. */
    mrs     r2, psplim
    str     r2, [r4, #OFF_SP_LIMIT]
1:
    add     r7, r12, #32           /* core frame: saved R4-R11 后 */
    add     r3, r7, #32            /* basic frame 后的 a4 */
    tst     lr, #0x10
    it      eq
    addeq   r3, r3, #72            /* extended FP state 位于 core frame 后 */

    /* 从用户栈读取 a4-a6 */
    ldr     r4, [r3, #0]
    ldr     r5, [r3, #4]
    ldr     r6, [r3, #8]
    push    {r4, r5, r6, lr}

    /*
     * Handler bookkeeping above uses r1-r3 (notably r2 for PSPLIM), so the
     * live registers no longer contain the syscall arguments.  Reload the
     * hardware-stacked r0-r3 immediately before entering the C dispatcher.
     */
    ldmia   r7, {r0-r3}
    bl      kern_syscall_handler
    add     sp, sp, #12
    pop     {lr}

    ldr     r1, =-128              /* KERN_SYSCALL_BLOCKED */
    cmp     r0, r1
    beq     .L_syscall_blocked

    str     r0, [r7]               /* 返回值写入异常帧 R0 */

    GET_CPU_OFFSET r4, r5           /* r4=cpu_id, r5=byte offset */
    ldr     r3, =_current_task
    ldr     r4, [r3, r5]           /* _current_task[cpu_id] */
    cmp     r4, #0
    beq     2f
    ldr     r1, [r4, #OFF_SP]
    /* Restore PSPLIM before clobbering r4 with the saved R4-R11. */
    ldr     r2, [r4, #OFF_SP_LIMIT]
    msr     psplim, r2
    ldr     lr, [r4, #OFF_EXC_RETURN]
#if defined(__ARM_FP) && (__ARM_FP != 0)
    tst     lr, #0x10
    bne     .L_svc_no_fp_restore
    add     r2, r4, #OFF_FP_HIGH
    vldmia  r2, {s16-s31}
.L_svc_no_fp_restore:
#endif
    ldmia   r1!, {r4-r11}
    msr     psp, r1
2:
    bx      lr

.L_bad_svc_param:
    tst     lr, #4
    ite     eq
    mrseq   r12, msp
    mrsne   r12, psp
    ldr     r1, =-2               /* KERN_ERR_PARAM */
    str     r1, [r12]              /* stacked R0 */
    bx      lr

.L_bad_svc_state:
    tst     lr, #4
    ite     eq
    mrseq   r12, msp
    mrsne   r12, psp
    ldr     r1, =-5               /* KERN_ERR_STATE */
    str     r1, [r12]              /* stacked R0 */
    bx      lr

.L_syscall_blocked:
    /*
     * 当前任务已经在 syscall handler 中进入 BLOCKED 状态，且
     * TCB->sp 指向 saved R4-R11 + hardware exception frame。
     * 不能返回原用户任务；直接选择下一个任务执行。唤醒方会把最终
     * 返回值写入该任务保存的异常帧 R0。
     */
    push    {lr}
    bl      kern_pendsv_handler
    pop     {lr}

    GET_CPU_OFFSET r1, r2           /* r1=cpu_id, r2=byte offset */
    ldr     r0, =_next_task
    ldr     r0, [r0, r2]           /* _next_task[cpu_id] */
    cmp     r0, #0
    beq     .L_svc_halt

    push    {r0, lr}
    bl      mpu_load_task_regions
    pop     {r0, lr}

    ldr     lr, [r0, #OFF_EXC_RETURN]
    ldrb    r2, [r0, #OFF_ATTRS]   /* next_task->attrs */
    tst     r2, #1                 /* TASK_ATTR_USER? */
    ite     eq
    moveq   r2, #2
    movne   r2, #3
#if defined(__ARM_FP) && (__ARM_FP != 0)
    tst     lr, #0x10
    it      eq
    orreq   r2, r2, #4
#endif
    msr     control, r2
    isb

    ldr     r1, [r0, #OFF_SP]     /* next->sp */
#if defined(__ARM_FP) && (__ARM_FP != 0)
    tst     lr, #0x10
    bne     .L_blocked_no_fp_restore
    add     r3, r0, #OFF_FP_HIGH
    vldmia  r3, {s16-s31}
.L_blocked_no_fp_restore:
#endif
    ldmia   r1!, {r4-r11}
    msr     psp, r1

    /* Switched to next task — install its PSPLIM. */
    ldr     r2, [r0, #OFF_SP_LIMIT]
    msr     psplim, r2

    bx      lr

.L_svc_halt:
    b       .

    .size SVC_Handler, . - SVC_Handler
    .thumb_set isr_svcall, SVC_Handler
    .end
