/**
 * @file rp2350.h
 * @brief RP2350 寄存器定义
 */

#ifndef RP2350_H
#define RP2350_H

#include <stdint.h>
#include <stddef.h>

/*============================================================================
 * 基地址定义
 *============================================================================*/

// 外设基地址
#define PPB_BASE        0xe0000000      // Private Peripheral Bus
#define PADS_BASE       0x40020000      // Pad Control
#define IO_BANK0_BASE   0x40014000      // IO Bank 0
#define IO_BANK1_BASE   0x40018000      // IO Bank 1
#define SYSINFO_BASE    0x40004000      // System Info
#define SYSCFG_BASE     0x40008000      // System Config
#define CLOCKS_BASE     0x40010000      // Clocks
#define RESETS_BASE     0x4000c000      // Resets
#define UART0_BASE      0x40034000      // UART 0
#define UART1_BASE      0x40038000      // UART 1
#define SIO_BASE        0xd0000000      // Single-cycle IO
#define XIP_BASE        0x14000000      // XIP 控制器
#define XIP_SSI_BASE    0x18000000      // XIP SSI

/*============================================================================
 * NVIC (Nested Vectored Interrupt Controller)
 *============================================================================*/

typedef struct {
    volatile uint32_t ISER[8];         // 中断使能设置
    uint32_t reserved0[24];
    volatile uint32_t ICER[8];         // 中断使能清除
    uint32_t reserved1[24];
    volatile uint32_t ISPR[8];         // 中断挂起设置
    uint32_t reserved2[24];
    volatile uint32_t ICPR[8];         // 中断挂起清除
    uint32_t reserved3[24];
    volatile uint32_t IABR[8];         // 中断活跃位
    uint32_t reserved4[56];
    volatile uint32_t IP[240];         // 中断优先级
    uint32_t reserved5[644];
    volatile uint32_t STIR;            // 软件触发中断
} nvic_t;

#define NVIC            ((nvic_t *)(PPB_BASE + 0xe100))

/*============================================================================
 * SysTick
 *============================================================================*/

typedef struct {
    volatile uint32_t CSR;             // 控制和状态
    volatile uint32_t RVR;             // 重载值
    volatile uint32_t CVR;             // 当前值
    volatile uint32_t CALIB;           // 校准值
} systick_t;

#define SYSTICK         ((systick_t *)(PPB_BASE + 0xe010))

// SysTick CSR 位定义
#define SYSTICK_CSR_ENABLE      (1 << 0)
#define SYSTICK_CSR_TICKINT     (1 << 1)
#define SYSTICK_CSR_CLKSRC      (1 << 2)
#define SYSTICK_CSR_COUNTFLAG   (1 << 16)

/*============================================================================
 * SCB (System Control Block)
 *============================================================================*/

typedef struct {
    uint32_t reserved0;
    volatile uint32_t ICTR;
    volatile uint32_t ACTLR;
    uint32_t reserved1[1];
    volatile uint32_t CPACR;
    uint32_t reserved2[57];
    volatile uint32_t CPUID;
    volatile uint32_t ICSR;
    volatile uint32_t VTOR;
    volatile uint32_t AIRCR;
    volatile uint32_t SCR;
    volatile uint32_t CCR;
    uint32_t reserved3[1];
    volatile uint32_t SHPR[3];
    uint32_t reserved4[5];
    volatile uint32_t SHCSR;
    uint32_t reserved5[15];
    volatile uint32_t DFSR;
    volatile uint32_t MMFSR;
    volatile uint32_t BFSR;
    volatile uint32_t AFSR;
    uint32_t reserved6[1];
    volatile uint32_t MMAR;
    volatile uint32_t BFAR;
} scb_t;

#define SCB             ((scb_t *)(PPB_BASE + 0xe008))

// SCB AIRCR 位定义
#define SCB_AIRCR_VECTKEY       (0x5fa << 16)
#define SCB_AIRCR_SYSRESETREQ   (1 << 2)

/*============================================================================
 * SIO (Single-cycle IO) - GPIO 快速访问
 *============================================================================*/

typedef struct {
    volatile uint32_t cpuid;           // CPU ID (0 or 1 for dual core)
    volatile uint32_t gpio_in;         // GPIO 输入状态
    volatile uint32_t gpio_hi_in;      // GPIO 高位输入
    uint32_t reserved0;
    volatile uint32_t gpio_out;        // GPIO 输出
    volatile uint32_t gpio_out_set;    // GPIO 输出置位
    volatile uint32_t gpio_out_clr;    // GPIO 输出清零
    volatile uint32_t gpio_out_xor;    // GPIO 输出翻转
    volatile uint32_t gpio_oe;         // GPIO 输出使能
    volatile uint32_t gpio_oe_set;     // GPIO 输出使能置位
    volatile uint32_t gpio_oe_clr;     // GPIO 输出使能清零
    volatile uint32_t gpio_oe_xor;     // GPIO 输出使能翻转
    volatile uint32_t gpio_hi_out;
    volatile uint32_t gpio_hi_out_set;
    volatile uint32_t gpio_hi_out_clr;
    volatile uint32_t gpio_hi_out_xor;
    volatile uint32_t gpio_hi_oe;
    volatile uint32_t gpio_hi_oe_set;
    volatile uint32_t gpio_hi_oe_clr;
    volatile uint32_t gpio_hi_oe_xor;
    uint32_t reserved1[4];
    volatile uint32_t spin_lock[32];   // 自旋锁
} sio_t;

#define SIO             ((sio_t *)SIO_BASE)

/*============================================================================
 * UART
 *============================================================================*/

typedef struct {
    volatile uint32_t dr;              // 数据寄存器
    volatile uint32_t rsr_ecr;         // 接收状态/错误清除
    uint32_t reserved0[4];
    volatile uint32_t fr;              // 标志寄存器
    uint32_t reserved1;
    volatile uint32_t ilpr;            // IrDA 低功耗分频
    volatile uint32_t ibrd;            // 整数波特率分频
    volatile uint32_t fbrd;            // 小数波特率分频
    volatile uint32_t lcr_h;           // 线控制寄存器
    volatile uint32_t cr;              // 控制寄存器
    volatile uint32_t ifls;            // 中断 FIFO 级别
    volatile uint32_t imsc;            // 中断屏蔽
    volatile uint32_t ris;             // 原始中断状态
    volatile uint32_t mis;             // 屏蔽中断状态
    volatile uint32_t icr;             // 中断清除
    volatile uint32_t dmacr;           // DMA 控制
} uart_t;

#define UART0           ((uart_t *)UART0_BASE)
#define UART1           ((uart_t *)UART1_BASE)

// UART FR 位定义
#define UART_FR_CTS     (1 << 0)
#define UART_FR_DSR     (1 << 1)
#define UART_FR_DCD     (1 << 2)
#define UART_FR_BUSY    (1 << 3)
#define UART_FR_RXFE    (1 << 4)       // 接收 FIFO 空
#define UART_FR_TXFF    (1 << 5)       // 发送 FIFO 满
#define UART_FR_RXFF    (1 << 6)
#define UART_FR_TXFE    (1 << 7)

// UART LCR_H 位定义
#define UART_LCRH_BRK   (1 << 0)
#define UART_LCRH_PEN   (1 << 1)       // 奇偶校验使能
#define UART_LCRH_EPS   (1 << 2)       // 偶校验
#define UART_LCRH_FEN   (1 << 4)       // FIFO 使能
#define UART_LCRH_WLEN_8 (3 << 5)      // 8位数据
#define UART_LCRH_SPS   (1 << 7)

// UART CR 位定义
#define UART_CR_UARTEN  (1 << 0)       // UART 使能
#define UART_CR_SIREN   (1 << 1)
#define UART_CR_SIRLP   (1 << 2)
#define UART_CR_LBE     (1 << 7)
#define UART_CR_TXE     (1 << 8)       // 发送使能
#define UART_CR_RXE     (1 << 9)       // 接收使能

/*============================================================================
 * GPIO (IO Bank)
 *============================================================================*/

typedef struct {
    volatile uint32_t status;
    volatile uint32_t ctrl;
} gpio_pad_t;

typedef struct {
    gpio_pad_t io[30];                 // GPIO 0-29
    uint32_t reserved0[2];
    gpio_pad_t io_hi[6];               // GPIO 30-35
    uint32_t reserved1[30];
    volatile uint32_t irqsum[4];
    uint32_t reserved2[4];
    volatile uint32_t proc0_irq[8];
    volatile uint32_t proc1_irq[8];
} io_bank_t;

#define IO_BANK0        ((io_bank_t *)IO_BANK0_BASE)

// GPIO 功能选择
#define GPIO_FUNC_XIP   0
#define GPIO_FUNC_SPI   1
#define GPIO_FUNC_UART  2
#define GPIO_FUNC_I2C   3
#define GPIO_FUNC_PWM   4
#define GPIO_FUNC_SIO   5
#define GPIO_FUNC_PIO0  6
#define GPIO_FUNC_PIO1  7
#define GPIO_FUNC_GPCK  8
#define GPIO_FUNC_USB   9
#define GPIO_FUNC_UART_AUX 10

/*============================================================================
 * PADS (Pad Control)
 *============================================================================*/

typedef struct {
    volatile uint32_t voltage_select;
    volatile uint32_t io[30];
    uint32_t reserved0[2];
    volatile uint32_t io_hi[6];
    uint32_t reserved1[10];
    volatile uint32_t swd;
    volatile uint32_t qspi;
    volatile uint32_t xip;
} pads_bank_t;

#define PADS_BANK0      ((pads_bank_t *)PADS_BASE)

// PADS 位定义
#define PADS_IE         (1 << 6)       // 输入使能
#define PADS_OD         (1 << 7)       // 输出禁用
#define PADS_PUE        (1 << 3)       // 上拉使能
#define PADS_PDE        (1 << 2)       // 下拉使能

/*============================================================================
 * RESETS
 *============================================================================*/

typedef struct {
    volatile uint32_t reset;
    volatile uint32_t done;
    volatile uint32_t selected;
    uint32_t reserved0;
    volatile uint32_t force;
    volatile uint32_t wdone;
    volatile uint32_t wselected;
} resets_t;

#define RESETS          ((resets_t *)RESETS_BASE)

// 复位位定义
#define RESETS_RESET_UART0      (1 << 12)
#define RESETS_RESET_UART1      (1 << 13)
#define RESETS_RESET_IO_BANK0   (1 << 5)

/*============================================================================
 * CLOCKS
 *============================================================================*/

typedef struct {
    volatile uint32_t clk_ref_ctrl;
    volatile uint32_t clk_ref_div;
    volatile uint32_t clk_ref_selected;
    uint32_t reserved0;
    volatile uint32_t clk_sys_ctrl;
    volatile uint32_t clk_sys_div;
    volatile uint32_t clk_sys_selected;
    uint32_t reserved1;
    volatile uint32_t clk_peri_ctrl;
    volatile uint32_t clk_peri_div;
    volatile uint32_t clk_peri_selected;
    uint32_t reserved2;
    volatile uint32_t clk_usb_ctrl;
    volatile uint32_t clk_usb_div;
    volatile uint32_t clk_usb_selected;
    uint32_t reserved3;
    volatile uint32_t clk_adc_ctrl;
    volatile uint32_t clk_adc_div;
    volatile uint32_t clk_adc_selected;
    uint32_t reserved4[4];
    volatile uint32_t clk_rtc_ctrl;
    volatile uint32_t clk_rtc_div;
    volatile uint32_t clk_rtc_selected;
} clocks_t;

#define CLOCKS          ((clocks_t *)CLOCKS_BASE)

/*============================================================================
 * 系统配置
 *============================================================================*/

typedef struct {
    volatile uint32_t vreg_and_chip_reset;
    uint32_t reserved0[3];
    volatile uint32_t proc_config;
    uint32_t reserved1;
    volatile uint32_t gpo;
    volatile uint32_t gpo_set;
    volatile uint32_t gpo_clr;
    volatile uint32_t gpo_xor;
} syscfg_t;

#define SYSCFG          ((syscfg_t *)SYSCFG_BASE)

/*============================================================================
 * 工具宏
 *============================================================================*/

#define BIT(x)          (1U << (x))
#define SETBIT(x, b)    ((x) |= BIT(b))
#define CLRBIT(x, b)    ((x) &= ~BIT(b))
#define TGLBIT(x, b)    ((x) ^= BIT(b))

#define REG32(addr)     (*(volatile uint32_t *)(addr))

// 空操作
#define NOP()           __asm volatile("nop")

// 内存屏障
#define DMB()           __asm volatile("dmb")
#define DSB()           __asm volatile("dsb")
#define ISB()           __asm volatile("isb")

// 使能/禁用中断
#define ENABLE_IRQ()    __asm volatile("cpsie i")
#define DISABLE_IRQ()   __asm volatile("cpsid i")

#endif // RP2350_H
