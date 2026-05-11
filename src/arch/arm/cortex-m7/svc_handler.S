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
 *   因此异常帧 R0 地址是 PSP+32，额外参数从 PSP+64 开始。
 */

    .syntax unified
    .cpu cortex-m7
    .thumb

    .extern _current_task
    .extern _next_task
    .extern kern_syscall_handler
    .extern mpu_load_task_regions

    .section .text
    .global SVC_Handler
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
    beq     .L_first_switch

    cmp     r12, #1
    beq     .L_general_syscall

    b       .

/*============================================================================
 * SVC #0: 首次任务启动
 *============================================================================*/
.L_first_switch:
    ldr     r0, =_next_task
    ldr     r0, [r0]
    ldr     r1, [r0]               /* R1 = first_task->sp */

    ldmia   r1!, {r4-r11}
    msr     psp, r1

    push    {r0, lr}
    bl      mpu_load_task_regions
    pop     {r0, lr}

    ldrb    r2, [r0, #96]          /* TCB->attrs */
    tst     r2, #1                 /* TASK_ATTR_USER? */
    ite     eq
    moveq   r2, #2
    movne   r2, #3
    msr     control, r2
    isb

    ldr     r2, =_current_task
    ldr     r3, =_next_task
    ldr     r3, [r3]
    str     r3, [r2]

    cpsie   i
    ldr     lr, =0xFFFFFFFD
    bx      lr

/*============================================================================
 * SVC #1: 通用系统调用
 *============================================================================*/
.L_general_syscall:
    mrs     r12, psp
    stmdb   r12!, {r4-r11}
    msr     psp, r12

    ldr     r4, =_current_task
    ldr     r4, [r4]
    cmp     r4, #0
    beq     1f
    str     r12, [r4]              /* TCB->sp = PSP */
1:
    add     r7, r12, #32           /* R7 = 异常帧中 R0 地址 */

    /* 从用户栈读取 a4-a6 */
    ldr     r4, [r12, #64]
    ldr     r5, [r12, #68]
    ldr     r6, [r12, #72]
    push    {r4, r5, r6, lr}
    bl      kern_syscall_handler
    add     sp, sp, #12
    pop     {lr}

    str     r0, [r7]               /* 返回值写入异常帧 R0 */

    ldr     r4, =_current_task
    ldr     r4, [r4]
    cmp     r4, #0
    beq     2f
    ldr     r1, [r4]
    ldmia   r1!, {r4-r11}
    msr     psp, r1
2:
    ldr     lr, =0xFFFFFFFD
    bx      lr

    .size SVC_Handler, . - SVC_Handler
    .end
