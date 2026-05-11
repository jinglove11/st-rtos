/**
 * @file boot2.S
 * @brief RP2350 Stage 2 Bootloader
 *
 * 这个文件必须放在 Flash 的起始位置 (0x10000000)
 * 它负责配置 XIP (Execute In Place) 然后跳转到用户代码
 *
 * 注意: 这是一个简化版本，实际使用时可能需要根据你的 Flash 类型调整
 */

    .syntax unified
    .cpu cortex-m33
    .thumb

/*============================================================================
 * 常量定义
 *============================================================================*/

// XIP_SSI 寄存器偏移
#define SSI_CTRLR0          0x00
#define SSI_CTRLR1          0x04
#define SSI_SSIENR          0x08
#define SSI_BAUDR           0x14
#define SSI_TXFLTR          0x18
#define SSI_RXFLR           0x1c
#define SSI_SR              0x28
#define SSI_DR0             0x60
#define SSI_XIP_SPI_CTRLR0  0xf4

// XIP_CTRL 寄存器偏移
#define XIP_CTRL            0x00
#define XIP_FLUSH           0x04

// Flash 命令
#define FLASH_CMD_READ      0x03    // 标准读取
#define FLASH_CMD_FAST_READ 0x0b    // 快速读取

/*============================================================================
 * 代码段
 *============================================================================*/

    .section .boot2, "ax"
    .global _boot2_entry
    .type _boot2_entry, %function
    .align 2

_boot2_entry:
    // ========================================
    // Stage2 bootloader 入口点
    // RP2350 BootROM 加载并执行这段代码
    // ========================================

    // 简化版: 直接跳转到 reset handler
    // 实际应用中需要配置 XIP 和 Flash 时序

    // 等待几周期确保稳定
    nop
    nop
    nop

    // 跳转到 reset handler
    ldr r0, =_reset_handler
    bx  r0

    // 不应该到达这里
_boot2_end:
    b   _boot2_end

    .size _boot2_entry, . - _boot2_entry

/*============================================================================
 * 向量表
 *============================================================================*/

    .section .vectors, "a"
    .global _vectors
    .align 8

_vectors:
    .word   _estack                 // 初始栈指针
    .word   _reset_handler          // Reset Handler
    .word   _default_handler        // NMI Handler
    .word   _default_handler        // Hard Fault Handler
    .word   _default_handler        // MPU Fault Handler
    .word   _default_handler        // Bus Fault Handler
    .word   _default_handler        // Usage Fault Handler
    .word   0                       // Reserved
    .word   0                       // Reserved
    .word   0                       // Reserved
    .word   0                       // Reserved
    .word   _default_handler        // SVCall Handler
    .word   _default_handler        // Debug Monitor Handler
    .word   0                       // Reserved
    .word   _default_handler        // PendSV Handler
    .word   _default_handler        // SysTick Handler

    // 外部中断 (RP2350 有很多中断，这里只列出部分)
    .rept 160
    .word   _default_handler
    .endr

/*============================================================================
 * 默认中断处理函数
 *============================================================================*/

    .section .text
    .global _default_handler
    .type _default_handler, %function
    .align 2

_default_handler:
    // 默认处理: 死循环
    b   _default_handler

    .size _default_handler, . - _default_handler
