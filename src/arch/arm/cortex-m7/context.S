/**
 * @file hal_context_stm32f7.S
 * @brief STM32F7 上下文切换 (Cortex-M7)
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
 * 调用流程:
 * 1. 禁用中断
 * 2. 获取当前任务 TCB
 * 3. 如果当前任务存在, 保存 R4-R11 到其栈
 * 4. 调用 kern_pendsv_handler 选择下一任务
 * 5. 恢复下一任务的 R4-R11
 * 6. 使能中断并返回
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
    str     r1, [r0]            // 保存 SP 到 TCB

.L_select_next:
    // 调用 C 函数选择下一任务
    push    {lr}
    bl      kern_pendsv_handler
    pop     {lr}

    // 获取下一任务的栈指针
    ldr     r0, =_next_task
    ldr     r0, [r0]
    ldr     r1, [r0]            // r1 = SP

    // 恢复上下文
    ldmia   r1!, {r4-r11}       // 恢复 R4-R11
    msr     psp, r1             // 设置 PSP

    // 使能中断
    cpsie   i

    // 从异常返回
    bx      lr

    .size PendSV_Handler, . - PendSV_Handler

/*============================================================================
 * SVC 处理 - 用于阻塞式上下文切换
 *
 * 当任务需要阻塞时，通过 SVC 立即触发上下文切换
 *============================================================================*/

    .section .text
    .global SVC_Handler
    .type SVC_Handler, %function
    .align 2

SVC_Handler:
    // 获取当前任务 TCB
    ldr     r0, =_current_task
    ldr     r0, [r0]

    // 如果当前任务存在，保存上下文
    cmp     r0, #0
    beq     .L_svc_select_next

    // 保存当前任务上下文
    mrs     r1, psp
    stmdb   r1!, {r4-r11}
    str     r1, [r0]

.L_svc_select_next:
    // 调用 C 函数选择下一任务
    push    {lr}
    bl      kern_pendsv_handler
    pop     {lr}

    // 获取下一任务的栈指针
    ldr     r0, =_next_task
    ldr     r0, [r0]
    ldr     r1, [r0]

    // 恢复上下文
    ldmia   r1!, {r4-r11}
    msr     psp, r1

    // 使能中断
    cpsie   i

    // 从异常返回
    bx      lr

    .size SVC_Handler, . - SVC_Handler

/*============================================================================
 * 触发阻塞式上下文切换 (通过 SVC)
 *============================================================================*/

    .section .text
    .global hal_trigger_block_switch
    .type hal_trigger_block_switch, %function
    .align 2

hal_trigger_block_switch:
    // 设置 SVC 优先级为最高 (0)
    ldr     r0, =0xE000ED18     // SHPR2 (SVC 优先级)
    mov     r1, #0
    str     r1, [r0]

    // 触发 SVC 进行上下文切换
    svc     #0

    // 返回 (任务被唤醒后继续执行)
    bx      lr

    .size hal_trigger_block_switch, . - hal_trigger_block_switch

/*============================================================================
 * 触发第一次调度 (通过 SVC 0)
 *============================================================================*/

    .section .text
    .global hal_trigger_first_switch
    .type hal_trigger_first_switch, %function
    .align 2

hal_trigger_first_switch:
    // 设置 SVC 优先级为最高 (0)
    ldr     r0, =0xE000ED18     // SHPR2 (SVC 优先级)
    mov     r1, #0
    str     r1, [r0]

    // 触发 SVC
    svc     #0

    // 不应该返回
    b       .

    .size hal_trigger_first_switch, . - hal_trigger_first_switch

    .end
