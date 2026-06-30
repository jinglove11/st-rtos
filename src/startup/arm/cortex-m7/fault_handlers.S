/**
 * @file fault_handlers.S
 * @brief Cortex-M7 Fault Handler — HardFault / MemManage / BusFault / UsageFault
 *
 * FAULT_ENTRY 宏统一处理:
 *   1. 保存 EXC_RETURN (LR)
 *   2. 从 LR bit 2 判断异常栈 (MSP=0, PSP=1)
 *   3. 保存 R4-R11 到 crash_dump
 *   4. 保存 MSP, PSP 到 crash_dump
 *   5. 调用 fault_handler_c(fault_type, exc_frame)
 *   6. 异常返回
 */

    .syntax unified
    .thumb

    .extern fault_handler_c
    .extern crash_dump

/*============================================================================
 * FAULT_ENTRY 宏
 *============================================================================*/

.macro FAULT_ENTRY fault_type
    push    {lr}

    tst     lr, #4
    ite     eq
    mrseq   r0, msp
    mrsne   r0, psp               /* R0 = SP */
    it      eq
    addeq   r0, r0, #4            /* handler: skip pushed LR to reach exc_frame */

    ldr     r1, =crash_dump
    add     r1, #16               /* offset to crash_dump.r4 */
    stmia   r1, {r4-r11}          /* save R4..R11 */

    mrs     r2, msp
    add     r2, r2, #4            /* undo push {lr} (always to MSP in handler) */
    mrs     r3, psp
    ldr     r1, =crash_dump
    str     r2, [r1, #84]         /* crash_dump.msp */
    str     r3, [r1, #88]         /* crash_dump.psp */

    mov     r1, r0                /* R1 = exc_frame */
    mov     r0, #\fault_type      /* R0 = fault_type */
    mov     r2, lr                /* R2 = EXC_RETURN */
    bl      fault_handler_c

    pop     {lr}
    bx      lr
.endm

/*============================================================================
 * HardFault_Handler
 *============================================================================*/

    .section .text
    .global HardFault_Handler
    .global isr_hardfault
    .type HardFault_Handler, %function
    .align 2

HardFault_Handler:
    FAULT_ENTRY 0                  /* FAULT_TYPE_HARD = 0 */
    .size HardFault_Handler, . - HardFault_Handler
    .thumb_set isr_hardfault, HardFault_Handler

/*============================================================================
 * MemManage_Handler — MPU 违规 / 栈溢出
 *============================================================================*/

    .section .text
    .global MemManage_Handler
    .global isr_memmanage
    .type MemManage_Handler, %function
    .align 2

MemManage_Handler:
    FAULT_ENTRY 1                  /* FAULT_TYPE_MEMMANAGE = 1 */
    .size MemManage_Handler, . - MemManage_Handler
    .thumb_set isr_memmanage, MemManage_Handler

/*============================================================================
 * BusFault_Handler — 总线错误
 *============================================================================*/

    .section .text
    .global BusFault_Handler
    .global isr_busfault
    .type BusFault_Handler, %function
    .align 2

BusFault_Handler:
    FAULT_ENTRY 2                  /* FAULT_TYPE_BUS = 2 */
    .size BusFault_Handler, . - BusFault_Handler
    .thumb_set isr_busfault, BusFault_Handler

/*============================================================================
 * UsageFault_Handler — 未定义指令/非法操作
 *============================================================================*/

    .section .text
    .global UsageFault_Handler
    .global isr_usagefault
    .type UsageFault_Handler, %function
    .align 2

UsageFault_Handler:
    FAULT_ENTRY 3                  /* FAULT_TYPE_USAGE = 3 */
    .size UsageFault_Handler, . - UsageFault_Handler
    .thumb_set isr_usagefault, UsageFault_Handler

    .end
