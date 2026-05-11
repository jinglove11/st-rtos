/**
 * @file stm32f767.h
 * @brief STM32F767ZIT6 寄存器定义
 */

#ifndef STM32F767_H
#define STM32F767_H

#include <stdint.h>
#include <stddef.h>

/*============================================================================
 * 内存映射
 *============================================================================*/

#define FLASH_BASE          0x08000000UL
#define FLASH_SIZE          (2 * 1024 * 1024)      // 2MB
#define SRAM1_BASE          0x20010000UL
#define SRAM1_SIZE          (368 * 1024)           // 368KB
#define SRAM2_BASE          0x2001C000UL
#define SRAM2_SIZE          (16 * 1024)            // 16KB
#define SRAM_BASE           SRAM1_BASE
#define SRAM_SIZE           (SRAM1_SIZE + SRAM2_SIZE)

#define PERIPH_BASE         0x40000000UL
#define APB1PERIPH_BASE     PERIPH_BASE
#define APB2PERIPH_BASE     (PERIPH_BASE + 0x00010000UL)
#define AHB1PERIPH_BASE     (PERIPH_BASE + 0x00020000UL)
#define AHB2PERIPH_BASE     (PERIPH_BASE + 0x10000000UL)

/*============================================================================
 * 外设基地址
 *============================================================================*/

// GPIO
#define GPIOA_BASE          (AHB1PERIPH_BASE + 0x0000UL)
#define GPIOB_BASE          (AHB1PERIPH_BASE + 0x0400UL)
#define GPIOC_BASE          (AHB1PERIPH_BASE + 0x0800UL)
#define GPIOD_BASE          (AHB1PERIPH_BASE + 0x0C00UL)
#define GPIOE_BASE          (AHB1PERIPH_BASE + 0x1000UL)
#define GPIOF_BASE          (AHB1PERIPH_BASE + 0x1400UL)
#define GPIOG_BASE          (AHB1PERIPH_BASE + 0x1800UL)
#define GPIOH_BASE          (AHB1PERIPH_BASE + 0x1C00UL)
#define GPIOI_BASE          (AHB1PERIPH_BASE + 0x2000UL)

// UART/USART
#define USART1_BASE         (APB2PERIPH_BASE + 0x1000UL)
#define USART2_BASE         (APB1PERIPH_BASE + 0x4400UL)
#define USART3_BASE         (APB1PERIPH_BASE + 0x4800UL)
#define UART4_BASE          (APB1PERIPH_BASE + 0x4C00UL)
#define UART5_BASE          (APB1PERIPH_BASE + 0x5000UL)
#define USART6_BASE         (APB2PERIPH_BASE + 0x1400UL)

// RCC
#define RCC_BASE            (AHB1PERIPH_BASE + 0x3800UL)

// PWR
#define PWR_BASE            (APB1PERIPH_BASE + 0x7000UL)

// SYSCFG
#define SYSCFG_BASE         (APB2PERIPH_BASE + 0x3800UL)

/*============================================================================
 * GPIO 寄存器结构
 *============================================================================*/

typedef struct {
    volatile uint32_t MODER;        // 模式寄存器
    volatile uint32_t OTYPER;       // 输出类型寄存器
    volatile uint32_t OSPEEDR;      // 输出速度寄存器
    volatile uint32_t PUPDR;        // 上拉/下拉寄存器
    volatile uint32_t IDR;          // 输入数据寄存器
    volatile uint32_t ODR;          // 输出数据寄存器
    volatile uint32_t BSRR;         // 置位/复位寄存器
    volatile uint32_t LCKR;         // 配置锁定寄存器
    volatile uint32_t AFR[2];       // 复用功能寄存器
} gpio_t;

#define GPIOA               ((gpio_t *)GPIOA_BASE)
#define GPIOB               ((gpio_t *)GPIOB_BASE)
#define GPIOC               ((gpio_t *)GPIOC_BASE)
#define GPIOD               ((gpio_t *)GPIOD_BASE)
#define GPIOE               ((gpio_t *)GPIOE_BASE)
#define GPIOF               ((gpio_t *)GPIOF_BASE)
#define GPIOG               ((gpio_t *)GPIOG_BASE)
#define GPIOH               ((gpio_t *)GPIOH_BASE)
#define GPIOI               ((gpio_t *)GPIOI_BASE)

// GPIO 模式
#define GPIO_MODE_INPUT     0
#define GPIO_MODE_OUTPUT    1
#define GPIO_MODE_AF        2
#define GPIO_MODE_ANALOG    3

// GPIO 输出类型
#define GPIO_OTYPE_PP       0
#define GPIO_OTYPE_OD       1

// GPIO 速度
#define GPIO_SPEED_LOW      0
#define GPIO_SPEED_MEDIUM   1
#define GPIO_SPEED_HIGH     2
#define GPIO_SPEED_VERYHIGH 3

// GPIO 上拉/下拉
#define GPIO_PUPD_NONE      0
#define GPIO_PUPD_UP        1
#define GPIO_PUPD_DOWN      2

/*============================================================================
 * USART 寄存器结构
 *============================================================================*/

typedef struct {
    volatile uint32_t CR1;          // 控制寄存器1
    volatile uint32_t CR2;          // 控制寄存器2
    volatile uint32_t CR3;          // 控制寄存器3
    volatile uint32_t BRR;          // 波特率寄存器
    volatile uint32_t GTPR;         // 保护时间和预分频寄存器
    volatile uint32_t RTOR;         // 接收超时寄存器
    volatile uint32_t RQR;          // 请求寄存器
    volatile uint32_t ISR;          // 中断和状态寄存器
    volatile uint32_t ICR;          // 中断标志清除寄存器
    volatile uint32_t RDR;          // 接收数据寄存器
    volatile uint32_t TDR;          // 发送数据寄存器
} usart_t;

#define USART1              ((usart_t *)USART1_BASE)
#define USART2              ((usart_t *)USART2_BASE)
#define USART3              ((usart_t *)USART3_BASE)
#define UART4               ((usart_t *)UART4_BASE)
#define UART5               ((usart_t *)UART5_BASE)
#define USART6              ((usart_t *)USART6_BASE)

// USART CR1 位定义
#define USART_CR1_UE        (1U << 0)       // USART使能
#define USART_CR1_RE        (1U << 2)       // 接收使能
#define USART_CR1_TE        (1U << 3)       // 发送使能
#define USART_CR1_RXNEIE    (1U << 5)       // 接收中断使能
#define USART_CR1_TCIE      (1U << 6)       // 发送完成中断使能
#define USART_CR1_M0        (1U << 12)      // 字长位0
#define USART_CR1_M1        (1U << 28)      // 字长位1
#define USART_CR1_OVER8     (1U << 15)      // 过采样模式

// USART ISR 位定义
#define USART_ISR_RXNE      (1U << 5)       // 接收数据寄存器非空
#define USART_ISR_TC        (1U << 6)       // 发送完成
#define USART_ISR_TXE       (1U << 7)       // 发送数据寄存器空

/*============================================================================
 * FLASH 寄存器结构 (AXI interface)
 *============================================================================*/

#define FLASH_REG_BASE      (AHB1PERIPH_BASE + 0x3C00UL)

typedef struct {
    volatile uint32_t ACR;          // 访问控制寄存器
    volatile uint32_t KEYR;         // 密钥寄存器
    volatile uint32_t OPTKEYR;      // 选项密钥寄存器
    volatile uint32_t SR;           // 状态寄存器
    volatile uint32_t CR;           // 控制寄存器
    volatile uint32_t OPTCR;        // 选项控制寄存器
    volatile uint32_t OPTCR1;       // 选项控制寄存器1
} flash_t;

#define FLASH               ((flash_t *)FLASH_REG_BASE)

// FLASH ACR 位定义
#define FLASH_ACR_LATENCY_0 (0U << 0)
#define FLASH_ACR_LATENCY_1 (1U << 0)   /* 1 WS: 30-60 MHz */
#define FLASH_ACR_LATENCY_2 (2U << 0)
#define FLASH_ACR_LATENCY_7 (7U << 0)
#define FLASH_ACR_PRFTEN    (1U << 8)
#define FLASH_ACR_ARTEN     (1U << 9)

/*============================================================================
 * RCC 寄存器结构
 *============================================================================*/

typedef struct {
    volatile uint32_t CR;           // 时钟控制寄存器
    volatile uint32_t PLLCFGR;      // PLL配置寄存器
    volatile uint32_t CFGR;         // 时钟配置寄存器
    volatile uint32_t CIR;          // 时钟中断寄存器
    volatile uint32_t AHB1RSTR;     // AHB1外设复位寄存器
    volatile uint32_t AHB2RSTR;     // AHB2外设复位寄存器
    volatile uint32_t AHB3RSTR;     // AHB3外设复位寄存器
    uint32_t reserved0;
    volatile uint32_t APB1RSTR;     // APB1外设复位寄存器
    volatile uint32_t APB2RSTR;     // APB2外设复位寄存器
    uint32_t reserved1[2];
    volatile uint32_t AHB1ENR;      // AHB1外设时钟使能寄存器
    volatile uint32_t AHB2ENR;      // AHB2外设时钟使能寄存器
    volatile uint32_t AHB3ENR;      // AHB3外设时钟使能寄存器
    uint32_t reserved2;
    volatile uint32_t APB1ENR;      // APB1外设时钟使能寄存器
    volatile uint32_t APB2ENR;      // APB2外设时钟使能寄存器
    uint32_t reserved3[2];
    volatile uint32_t AHB1LPENR;    // AHB1外设低功耗时钟使能寄存器
    volatile uint32_t AHB2LPENR;    // AHB2外设低功耗时钟使能寄存器
    volatile uint32_t AHB3LPENR;    // AHB3外设低功耗时钟使能寄存器
    uint32_t reserved4;
    volatile uint32_t APB1LPENR;    // APB1外设低功耗时钟使能寄存器
    volatile uint32_t APB2LPENR;    // APB2外设低功耗时钟使能寄存器
    uint32_t reserved5[2];
    volatile uint32_t BDCR;         // 备份域控制寄存器
    volatile uint32_t CSR;          // 控制/状态寄存器
    uint32_t reserved6[2];
    volatile uint32_t SSCGR;        // 扩频时钟发生器寄存器
    volatile uint32_t PLLI2SCFGR;   // PLLI2S配置寄存器
    volatile uint32_t PLLSAICFGR;   // PLLSAI配置寄存器
    volatile uint32_t DCKCFGR1;     // 专用时钟配置寄存器1
    volatile uint32_t DCKCFGR2;     // 专用时钟配置寄存器2
} rcc_t;

#define RCC                 ((rcc_t *)RCC_BASE)

// RCC CR 位定义
#define RCC_CR_HSION        (1U << 0)
#define RCC_CR_HSIRDY       (1U << 1)
#define RCC_CR_HSEON        (1U << 16)
#define RCC_CR_HSERDY       (1U << 17)
#define RCC_CR_PLLON        (1U << 24)
#define RCC_CR_PLLRDY       (1U << 25)

// RCC CFGR 位定义
#define RCC_CFGR_SW_HSI     0
#define RCC_CFGR_SW_HSE     1
#define RCC_CFGR_SW_PLL     2
#define RCC_CFGR_SWS_SHIFT  2
#define RCC_CFGR_HPRE_SHIFT 4
#define RCC_CFGR_HPRE_DIV1  (0U << RCC_CFGR_HPRE_SHIFT)
#define RCC_CFGR_PPRE1_SHIFT 10
#define RCC_CFGR_PPRE1_DIV4 (0b101U << RCC_CFGR_PPRE1_SHIFT)
#define RCC_CFGR_PPRE2_SHIFT 13
#define RCC_CFGR_PPRE2_DIV2 (0b100U << RCC_CFGR_PPRE2_SHIFT)

// RCC PLLCFGR 位定义
#define RCC_PLLCFGR_PLLM_SHIFT  0
#define RCC_PLLCFGR_PLLN_SHIFT  6
#define RCC_PLLCFGR_PLLP_SHIFT  16
#define RCC_PLLCFGR_PLLP_DIV2   (0U << RCC_PLLCFGR_PLLP_SHIFT)
#define RCC_PLLCFGR_PLLSRC_HSE  (1U << 22)
#define RCC_PLLCFGR_PLLQ_SHIFT  24
#define RCC_PLLCFGR_PLLQ_9      (9U << RCC_PLLCFGR_PLLQ_SHIFT)
#define RCC_PLLCFGR_PLLR_SHIFT  28
#define RCC_PLLCFGR_PLLR_DIV2   (0U << RCC_PLLCFGR_PLLR_SHIFT)

// RCC AHB1ENR 位定义
#define RCC_AHB1ENR_GPIOAEN (1U << 0)
#define RCC_AHB1ENR_GPIOBEN (1U << 1)
#define RCC_AHB1ENR_GPIOCEN (1U << 2)
#define RCC_AHB1ENR_GPIODEN (1U << 3)
#define RCC_AHB1ENR_GPIOEEN (1U << 4)
#define RCC_AHB1ENR_GPIOFEN (1U << 5)
#define RCC_AHB1ENR_GPIOGEN (1U << 6)
#define RCC_AHB1ENR_GPIOHEN (1U << 7)
#define RCC_AHB1ENR_GPIOIEN (1U << 8)

// RCC APB1ENR 位定义
#define RCC_APB1ENR_USART2EN    (1U << 17)
#define RCC_APB1ENR_USART3EN    (1U << 18)
#define RCC_APB1ENR_UART4EN     (1U << 19)
#define RCC_APB1ENR_UART5EN     (1U << 20)

// RCC CSR 位定义
#define RCC_CSR_LSION          (1U << 0)
#define RCC_CSR_LSIRDY         (1U << 1)

// RCC APB2ENR 位定义
#define RCC_APB2ENR_USART1EN    (1U << 4)
#define RCC_APB2ENR_USART6EN    (1U << 5)
#define RCC_APB2ENR_SYSCFGEN    (1U << 14)

/*============================================================================
 * PWR 寄存器结构
 *============================================================================*/

typedef struct {
    volatile uint32_t CR1;          // 电源控制寄存器1
    volatile uint32_t CSR1;         // 电源控制/状态寄存器1
    volatile uint32_t CR2;          // 电源控制寄存器2
    volatile uint32_t CSR2;         // 电源控制/状态寄存器2
} pwr_t;

#define PWR                 ((pwr_t *)PWR_BASE)

// PWR CR1 位定义
#define PWR_CR1_VOS_SHIFT   14
#define PWR_CR1_VOS_SCALE1  (3U << PWR_CR1_VOS_SHIFT)
#define PWR_CR1_VOS_SCALE2  (2U << PWR_CR1_VOS_SHIFT)
#define PWR_CR1_VOS_SCALE3  (1U << PWR_CR1_VOS_SHIFT)
#define PWR_CR1_ODEN        (1U << 16)
#define PWR_CR1_ODSWEN      (1U << 17)
#define PWR_CR1_UDEN        (1U << 18)

// PWR CSR1 位定义
#define PWR_CSR1_ODRDY      (1U << 16)
#define PWR_CSR1_ODSWRDY    (1U << 17)
#define PWR_CSR1_UDRDY      (1U << 18)
#define PWR_CSR1_VOSRDY     (1U << 14)

/*============================================================================
 * SCB (System Control Block) - Cortex-M7
 *============================================================================*/

#define SCB_BASE            0xE000ED00UL

typedef struct {
    volatile uint32_t CPUID;        // 0x00: CPUID 基址寄存器
    volatile uint32_t ICSR;         // 0x04: 中断控制状态寄存器
    volatile uint32_t VTOR;         // 0x08: 向量表偏移寄存器
    volatile uint32_t AIRCR;        // 0x0C: 应用中断复位控制寄存器
    volatile uint32_t SCR;          // 0x10: 系统控制寄存器
    volatile uint32_t CCR;          // 0x14: 配置控制寄存器
    volatile uint32_t SHPR[3];      // 0x18-0x20: 系统处理优先级寄存器
    volatile uint32_t SHCSR;        // 0x24: 系统处理控制状态寄存器
    volatile uint32_t CFSR;         // 0x28: 配置故障状态寄存器
    volatile uint32_t HFSR;         // 0x2C: 硬件故障状态寄存器
    volatile uint32_t DFSR;         // 0x30: 调试故障状态寄存器
    volatile uint32_t MMFAR;        // 0x34: MemManage 故障地址寄存器
    volatile uint32_t BFAR;         // 0x38: 总线故障地址寄存器
    volatile uint32_t AFSR;         // 0x3C: 辅助故障状态寄存器
} scb_t;

#define SCB                 ((scb_t *)SCB_BASE)

// SCB AIRCR
#define SCB_AIRCR_VECTKEY   (0x5FAUL << 16)

/*============================================================================
 * SysTick
 *============================================================================*/

#define SYSTICK_BASE        0xE000E010UL

typedef struct {
    volatile uint32_t CSR;          // 控制和状态寄存器
    volatile uint32_t RVR;          // 重载值寄存器
    volatile uint32_t CVR;          // 当前值寄存器
    volatile uint32_t CALIB;        // 校准值寄存器
} systick_t;

#define SYSTICK             ((systick_t *)SYSTICK_BASE)

#define SYSTICK_CSR_ENABLE  (1U << 0)
#define SYSTICK_CSR_TICKINT (1U << 1)
#define SYSTICK_CSR_CLKSRC  (1U << 2)

/*============================================================================
 * NVIC (Nested Vectored Interrupt Controller)
 *============================================================================*/

#define NVIC_BASE           0xE000E100UL

typedef struct {
    volatile uint32_t ISER[8];      // 中断使能设置
    uint32_t reserved0[24];
    volatile uint32_t ICER[8];      // 中断使能清除
    uint32_t reserved1[24];
    volatile uint32_t ISPR[8];      // 中断挂起设置
    uint32_t reserved2[24];
    volatile uint32_t ICPR[8];      // 中断挂起清除
    uint32_t reserved3[24];
    volatile uint32_t IABR[8];      // 中断活跃
    uint32_t reserved4[56];
    volatile uint8_t  IP[240];      // 中断优先级
} nvic_t;

#define NVIC                ((nvic_t *)NVIC_BASE)

/*============================================================================
 * 工具宏
 *============================================================================*/

// 使用 kernel_types.h 中的 BIT 宏
#ifndef BIT
#define BIT(x)              (1U << (x))
#endif
#define SETBIT(x, b)        ((x) |= BIT(b))
#define CLRBIT(x, b)        ((x) &= ~BIT(b))
#define TGLBIT(x, b)        ((x) ^= BIT(b))

#define REG32(addr)         (*(volatile uint32_t *)(addr))

#define NOP()               __asm volatile("nop")
#define DMB()               __asm volatile("dmb")
#define DSB()               __asm volatile("dsb")
#define ISB()               __asm volatile("isb")

#define ENABLE_IRQ()        __asm volatile("cpsie i")
#define DISABLE_IRQ()       __asm volatile("cpsid i")

#endif // STM32F767_H
