/**
 * @file context.S
 * @brief Cortex-M7 上下文切换
 */

    .syntax unified
    .cpu cortex-m7
    .thumb

/*============================================================================
 * 外部符号
 *============================================================================*/

    .extern _current_task
    .extern _next_task
    .extern kern_pendsv_handler

/*============================================================================
 * PendSV 异常处理
 *
 * 关键设计：
 * - 调度器核心（C代码）只修改状态和队列
 * - PendSV（汇编）负责保存/恢复寄存器
 * - 这消除了 C 函数调用栈与 PendSV 保存位置冲突的 bug
 *============================================================================*/

    .section .text
    .global PendSV_Handler
    .type PendSV_Handler, %function
    .align 2

PendSV_Handler:
    // 禁用中断 (保护调度过程)
    cpsid   i

    // 获取当前任务 TCB
    ldr     r0, =_current_task
    ldr     r0, [r0]

    // 如果当前任务为 NULL，跳过保存 (首次切换)
    cmp     r0, #0
    beq     .L_select_next

    // 保存当前任务上下文
    mrs     r1, psp             // 获取进程栈指针
    stmdb   r1!, {r4-r11}       // 保存 R4-R11
    str     r1, [r0]            // 保存 SP 到 TCB->sp

.L_select_next:
    // 调用 C 函数选择下一任务
    // kern_pendsv_handler 会设置 _next_task
    push    {lr}
    bl      kern_pendsv_handler
    pop     {lr}

    // 获取下一任务的栈指针
    ldr     r0, =_next_task
    ldr     r0, [r0]
    cmp     r0, #0
    beq     .L_pendsv_halt
    ldr     r1, [r0]            // r1 = SP

    // 恢复上下文
    ldmia   r1!, {r4-r11}       // 恢复 R4-R11
    msr     psp, r1             // 设置 PSP

    // 使能中断
    cpsie   i

    // 从异常返回
    bx      lr

.L_pendsv_halt:
    // _next_task 为 NULL，死循环
    b       .

    .size PendSV_Handler, . - PendSV_Handler

/*============================================================================
 * SVC 处理 - 用于启动第一个任务
 *
 * 首次切换时 _current_task 为 NULL
 * 直接从 _next_task 恢复上下文并开始执行
 *============================================================================*/

    .section .text
    .global SVC_Handler
    .type SVC_Handler, %function
    .align 2

SVC_Handler:
    // 获取下一任务
    ldr     r0, =_next_task
    ldr     r0, [r0]
    ldr     r1, [r0]            // r1 = SP

    // 恢复 R4-R11
    ldmia   r1!, {r4-r11}

    // 设置 PSP
    msr     psp, r1

    // 设置 CONTROL 寄存器使用 PSP (SPSEL = 1)
    movs    r2, #2
    msr     control, r2
    isb                             // 指令同步屏障

    // 更新 _current_task = _next_task
    ldr     r2, =_current_task
    ldr     r3, =_next_task
    ldr     r3, [r3]
    str     r3, [r2]

    // 使能中断
    cpsie   i

    // 从异常返回到线程模式 (使用 PSP)
    ldr     lr, =0xFFFFFFFD
    bx      lr

    .size SVC_Handler, . - SVC_Handler

/*============================================================================
 * 触发第一次调度 (通过 SVC 0)
 *============================================================================*/

    .section .text
    .global hal_trigger_first_switch
    .type hal_trigger_first_switch, %function
    .align 2

hal_trigger_first_switch:
    // SVC 优先级已在 hal_interrupt_priority_init 中设置
    // 触发 SVC
    svc     #0

    // 不应该返回
    b       .

    .size hal_trigger_first_switch, . - hal_trigger_first_switch

    .end
