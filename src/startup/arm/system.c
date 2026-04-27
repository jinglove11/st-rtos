/**
 * @file system_stm32f767.c
 * @brief STM32F767 系统初始化 (使用 HSI 简化配置)
 */

#include "stm32f767.h"

// UART 寄存器直接访问（用于异常处理）
#define USART3_REG_BASE     0x40004800UL
#define USART3_REG_ISR      (*(volatile uint32_t *)(USART3_REG_BASE + 0x1C))
#define USART3_REG_TDR      (*(volatile uint32_t *)(USART3_REG_BASE + 0x28))
#define USART_ISR_TXE_BIT   (1 << 7)

// 简单的字符输出
static void debug_putc(char c) {
    while (!(USART3_REG_ISR & USART_ISR_TXE_BIT));
    USART3_REG_TDR = c;
}

static void debug_puts(const char *s) {
    while (*s) debug_putc(*s++);
}

static void debug_puthex(uint32_t value) {
    const char hex[] = "0123456789ABCDEF";
    debug_puts("0x");
    for (int i = 28; i >= 0; i -= 4) {
        debug_putc(hex[(value >> i) & 0xF]);
    }
}

/*============================================================================
 * HardFault 打印函数
 *============================================================================*/

void hardfault_print(uint32_t psp, uint32_t msp, uint32_t cfsr, uint32_t hfsr) {
    debug_puts("\r\n!!! HARDFAULT !!!\r\n");

    debug_puts("PSP: "); debug_puthex(psp); debug_puts("\r\n");
    debug_puts("MSP: "); debug_puthex(msp); debug_puts("\r\n");
    debug_puts("CFSR: "); debug_puthex(cfsr); debug_puts("\r\n");
    debug_puts("HFSR: "); debug_puthex(hfsr); debug_puts("\r\n");

    // 解析 CFSR
    debug_puts("\r\nCFSR Analysis:\r\n");

    // BusFault (中字节)
    if (cfsr & 0x00FF0000) {
        debug_puts("  BusFault:\r\n");
        if (cfsr & (1 << 16)) debug_puts("    BFAR valid\r\n");
        if (cfsr & (1 << 20)) debug_puts("    IMPRECISERR\r\n");
        if (cfsr & (1 << 21)) debug_puts("    PRECISERR\r\n");
        if (cfsr & (1 << 22)) debug_puts("    IBUSERR\r\n");

        // 打印 BFAR
        if (cfsr & (1 << 16)) {
            uint32_t bfar = *(volatile uint32_t *)0xE000ED38;
            debug_puts("    BFAR: "); debug_puthex(bfar); debug_puts("\r\n");
        }
    }

    // UsageFault (低字节)
    if (cfsr & 0x0000FFFF) {
        debug_puts("  UsageFault:\r\n");
        if (cfsr & (1 << 0)) debug_puts("    UNDEFINSTR\r\n");
        if (cfsr & (1 << 1)) debug_puts("    INVSTATE\r\n");
        if (cfsr & (1 << 2)) debug_puts("    INVPC\r\n");
        if (cfsr & (1 << 3)) debug_puts("    NOCP\r\n");
        if (cfsr & (1 << 8)) debug_puts("    UNALIGNED\r\n");
        if (cfsr & (1 << 9)) debug_puts("    DIVBYZERO\r\n");
    }

    // 解析 HFSR
    debug_puts("\r\nHFSR Analysis:\r\n");
    if (hfsr & (1 << 0)) debug_puts("  DEBUGEVT\r\n");
    if (hfsr & (1 << 1)) debug_puts("  FORCED\r\n");
    if (hfsr & (1 << 30)) debug_puts("  VECTTBL\r\n");

    // 打印栈帧中的 PC 和 LR
    if (psp != 0) {
        debug_puts("\r\nStack Frame (PSP):\r\n");
        uint32_t *sp = (uint32_t *)psp;
        debug_puts("  R0:  "); debug_puthex(sp[0]); debug_puts("\r\n");
        debug_puts("  R1:  "); debug_puthex(sp[1]); debug_puts("\r\n");
        debug_puts("  R2:  "); debug_puthex(sp[2]); debug_puts("\r\n");
        debug_puts("  R3:  "); debug_puthex(sp[3]); debug_puts("\r\n");
        debug_puts("  R12: "); debug_puthex(sp[4]); debug_puts("\r\n");
        debug_puts("  LR:  "); debug_puthex(sp[5]); debug_puts("\r\n");
        debug_puts("  PC:  "); debug_puthex(sp[6]); debug_puts("\r\n");
        debug_puts("  xPSR:"); debug_puthex(sp[7]); debug_puts("\r\n");

        // 打印更多栈内容用于调试
        debug_puts("\r\nMore Stack (PSP-32 to PSP+32):\r\n");
        for (int i = -8; i <= 16; i++) {
            debug_puts("  [");
            if (i < 0) debug_putc('-');
            debug_puthex(i >= 0 ? (uint32_t)(psp + i*4) : (uint32_t)(psp - (-i)*4));
            debug_puts("]: ");
            debug_puthex(sp[i]);
            debug_puts("\r\n");
        }
    }

    debug_puts("\r\nSystem halted.\r\n");
}

/*============================================================================
 * SystemInit - 系统时钟初始化
 *
 * 使用 HSI 16MHz (内部振荡器) 作为系统时钟
 * 简单可靠，适合调试
 *============================================================================*/

void SystemInit(void) {
    // 1. 使能 HSI (默认已使能)
    RCC->CR |= RCC_CR_HSION;
    while (!(RCC->CR & RCC_CR_HSIRDY));

    // 2. 选择 HSI 作为系统时钟
    RCC->CFGR = (RCC->CFGR & ~3) | 0;  // SW = HSI

    // 3. 等待切换完成
    while ((RCC->CFGR & (3 << 2)) != 0);

    // 4. 设置向量表位置
    SCB->VTOR = 0x08000000UL;
}
