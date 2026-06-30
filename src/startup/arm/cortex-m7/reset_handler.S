/**
 * @file reset_handler.S
 * @brief Reset Handler — 数据段复制、BSS 清零、跳转 main
 */

    .syntax unified
    .cpu cortex-m7
    .thumb

    .extern SystemInit
    .extern main

/*============================================================================
 * Reset Handler
 *============================================================================*/

    .section .text
    .global _reset_handler
    .type _reset_handler, %function
    .align 2

_reset_handler:
    ldr r0, =_estack
    mov sp, r0

    /* 复制数据段 */
    ldr r0, =_sdata
    ldr r1, =_edata
    ldr r2, =_sidata

copy_data:
    cmp r0, r1
    bge zero_bss
    ldr r3, [r2], #4
    str r3, [r0], #4
    b copy_data

zero_bss:
    ldr r0, =_sbss
    ldr r1, =_ebss
    mov r2, #0

zero_loop:
    cmp r0, r1
    bge call_main
    str r2, [r0], #4
    b zero_loop

call_main:
    bl SystemInit
    bl main

infinite_loop:
    b infinite_loop

    .size _reset_handler, . - _reset_handler

/*============================================================================
 * Default Handler — 未配置中断的默认处理
 *============================================================================*/

    .section .text
    .global _default_handler
    .type _default_handler, %function
    .align 2

_default_handler:
    b   _default_handler

    .size _default_handler, . - _default_handler
    .end
