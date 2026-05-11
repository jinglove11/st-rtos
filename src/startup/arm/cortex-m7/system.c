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
 * System clock frequency (set during SystemInit)
 *============================================================================*/

static uint32_t sysclk_hz = 16000000UL;  /* updated after PLL lock */

uint32_t hal_get_sysclk(void) {
    return sysclk_hz;
}

/*============================================================================
 * SystemInit - 系统时钟初始化
 *
 * HSI 16MHz → PLL → SYSCLK 48MHz (conservative, no VOS/OverDrive)
 *   HCLK  = 48MHz  (AHB prescaler /1)
 *   APB1  = 48MHz  (APB1 prescaler /1)
 *   APB2  = 48MHz  (APB2 prescaler /1)
 *   FLASH = 0 wait states, prefetch + ART enabled
 *
 * Does NOT touch PWR registers.  Safe for default VOS Scale 3.
 *============================================================================*/

#define PLL_M_HSI  16
#define PLL_N_HSI  192
#define PLL_P_DIV  4

void SystemInit(void) {
    uint32_t reg;

    /* 1. Enable HSI (always on at reset, but ensure it's stable) */
    RCC->CR |= RCC_CR_HSION;
    while (!(RCC->CR & RCC_CR_HSIRDY));

    /* 2. FLASH: 1 wait state (STM32F7 needs 1 WS for 30-60 MHz at Scale 1/2/3) */
    FLASH->ACR = FLASH_ACR_PRFTEN | FLASH_ACR_ARTEN | FLASH_ACR_LATENCY_1;

    /* 3. Configure PLL: HSI / 16 * 192 / 4 = 48 MHz (VCO=192MHz, min) */
    reg  = (PLL_M_HSI << RCC_PLLCFGR_PLLM_SHIFT);
    reg |= (PLL_N_HSI << RCC_PLLCFGR_PLLN_SHIFT);
    reg |= (1U << RCC_PLLCFGR_PLLP_SHIFT);   /* PLLP = 4 */
    /* PLLSRC = 0 (HSI) */
    RCC->PLLCFGR = reg;

    /* 4. Configure bus prescalers: AHB=/1, APB1=/1, APB2=/1 */
    reg = RCC->CFGR;
    reg &= ~((0xFU << RCC_CFGR_HPRE_SHIFT) |
             (0x7U << RCC_CFGR_PPRE1_SHIFT) |
             (0x7U << RCC_CFGR_PPRE2_SHIFT));
    RCC->CFGR = reg;

    /* 5. Enable PLL, wait for lock */
    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY));

    /* 6. Switch system clock to PLL */
    reg  = RCC->CFGR;
    reg &= ~0x3U;
    reg |= RCC_CFGR_SW_PLL;
    RCC->CFGR = reg;
    DSB();
    while ((RCC->CFGR & (0x3U << RCC_CFGR_SWS_SHIFT))
           != (RCC_CFGR_SW_PLL << RCC_CFGR_SWS_SHIFT));

    sysclk_hz = 48000000UL;

    /* 7. Copy vector table to RAM and remap VTOR */
    extern uint32_t _vectors;
    extern uint32_t __ram_vector_start;
    uint32_t *src = &_vectors;
    uint32_t *dst = &__ram_vector_start;
    for (int i = 0; i < 128; i++) {
        dst[i] = src[i];
    }
    SCB->VTOR = (uint32_t)&__ram_vector_start;
    DSB();
    ISB();
}
