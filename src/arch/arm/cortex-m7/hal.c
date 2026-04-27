/**
 * @file hal_stm32f7.c
 * @brief STM32F7 系列 HAL 实现
 */

#include "hal.h"
#include "stm32f767.h"
#include "nucleo_f767.h"
#include "kernel_config.h"

/*============================================================================
 * 内部变量
 *============================================================================*/

static volatile uint32_t systick_count = 0;

/*============================================================================
 * CPU 抽象实现
 *============================================================================*/

void hal_cpu_init(void) {
    // 设置向量表位置
    SCB->VTOR = 0x08000000UL;

    // 初始化中断优先级
    hal_interrupt_priority_init();
}

void hal_irq_enable(void) {
    __asm volatile("cpsie i");
}

void hal_irq_disable(void) {
    __asm volatile("cpsid i");
}

uint32_t hal_irq_save(void) {
    uint32_t result;
    __asm volatile("mrs %0, primask" : "=r"(result));
    __asm volatile("cpsid i");
    return result;
}

void hal_irq_restore(uint32_t primask) {
    __asm volatile("msr primask, %0" :: "r"(primask));
}

/*============================================================================
 * BasePri 临界区实现
 *============================================================================*/

uint32_t hal_enter_critical(void) {
    uint32_t basepri;
    __asm volatile("mrs %0, basepri" : "=r"(basepri));
    __asm volatile("msr basepri, %0" :: "r"(SCHED_CRITICAL_PRIORITY << 4));
    __asm volatile("isb");
    return basepri;
}

void hal_exit_critical(uint32_t basepri) {
    __asm volatile("msr basepri, %0" :: "r"(basepri));
}

void hal_interrupt_priority_init(void) {
    // Cortex-M7 SHPR registers (32-bit access):
    // SHPR[0] = SHPR1 (0xE000ED18): MemManage, BusFault, UsageFault
    // SHPR[1] = SHPR2 (0xE000ED1C): bits 31:24 = SVCall priority
    // SHPR[2] = SHPR3 (0xE000ED20): bits 31:24 = SysTick, bits 23:16 = PendSV

    // 设置 SVC 优先级为最高（用于首次切换）- SHPR2 bits 31:24
    SCB->SHPR[1] = (0 << 24);

    // 设置 PendSV 优先级为最低 - SHPR3 bits 23:16
    // 设置 SysTick 优先级为最低 - SHPR3 bits 31:24
    SCB->SHPR[2] = (PENDSV_PRIORITY << 16) | (SYSTICK_PRIORITY << 24);
}

void hal_trigger_pendsv(void) {
    // 触发 PendSV (优先级已在初始化时设置为最低)
    SCB->ICSR = (1 << 28);
    // 内存屏障确保写入完成
    __asm volatile("dsb");
    __asm volatile("isb");
}

void hal_trigger_svc(uint32_t svc_num) {
    (void)svc_num;
    __asm volatile("svc #0");
}

void hal_enter_lowpower(void) {
    __asm volatile("wfi");
}

void hal_exit_lowpower(void) {
    // WFI 自动退出
}

uint32_t hal_get_cpu_id(void) {
    return 0;  // 单核
}

/*============================================================================
 * SysTick 抽象实现
 *============================================================================*/

void hal_systick_init(uint32_t rate_hz) {
    // 计算重载值 (使用 HSI 16MHz)
    uint32_t reload = (16000000UL / rate_hz) - 1;

    // 设置重载值
    SYSTICK->RVR = reload;

    // 清除当前值
    SYSTICK->CVR = 0;

    // 使能 SysTick, 使能中断, 使用处理器时钟
    SYSTICK->CSR = SYSTICK_CSR_ENABLE | SYSTICK_CSR_TICKINT | SYSTICK_CSR_CLKSRC;
}

uint32_t hal_systick_get(void) {
    return systick_count;
}

uint32_t hal_get_tick_count(void) {
    return systick_count;
}

void hal_set_tick_count(uint32_t count) {
    systick_count = count;
}

void hal_systick_enable(void) {
    SYSTICK->CSR |= SYSTICK_CSR_ENABLE | SYSTICK_CSR_TICKINT;
}

void hal_systick_disable(void) {
    SYSTICK->CSR &= ~(SYSTICK_CSR_ENABLE | SYSTICK_CSR_TICKINT);
}

/*============================================================================
 * SysTick 中断处理
 *============================================================================*/

void SysTick_Handler(void) {
    systick_count++;

    // 调用内核滴答处理
    extern void kern_tick_handler(void);
    kern_tick_handler();
}

/*============================================================================
 * 上下文切换实现
 *============================================================================*/

// 在 hal_context_stm32f7.S 中实现
extern void hal_context_switch(void **from, void *to);
extern void hal_context_switch_first(void *to);

void *hal_stack_init(void    *stack_top,
                     uint32_t stack_size,
                     void    *entry,
                     void    *arg,
                     void    *exit)
{
    uint8_t *stack_base = (uint8_t *)stack_top - stack_size;

    // 填充魔数用于栈溢出检测
    for (int i = 0; i < 16; i++) {
        stack_base[i] = STACK_MAGIC_BYTE;
    }

    uint32_t *sp = (uint32_t *)((uint8_t *)stack_top);

    // Cortex-M 硬件异常栈帧格式 (从高地址到低地址):
    // xPSR, PC, LR, R12, R3, R2, R1, R0
    // PendSV 软件帧: R4-R11 (在硬件帧之前)

    // 1. 初始化硬件帧 (异常返回时硬件自动恢复)
    sp--; *sp = 0x01000000UL;   // xPSR (Thumb bit set)
    sp--; *sp = (uint32_t)entry; // PC (任务入口)
    sp--; *sp = (uint32_t)exit;  // LR (任务退出处理)
    sp--; *sp = 0;               // R12
    sp--; *sp = 0;               // R3
    sp--; *sp = 0;               // R2
    sp--; *sp = 0;               // R1
    sp--; *sp = (uint32_t)arg;   // R0 (任务参数)

    // 2. 初始化软件帧 (PendSV 恢复)
    sp--; *sp = 0;  // R11
    sp--; *sp = 0;  // R10
    sp--; *sp = 0;  // R9
    sp--; *sp = 0;  // R8
    sp--; *sp = 0;  // R7
    sp--; *sp = 0;  // R6
    sp--; *sp = 0;  // R5
    sp--; *sp = 0;  // R4

    // 3. 确保 8 字节对齐 (AAPCS 要求)
    if ((uint32_t)sp & 4) {
        sp--;
        *sp = 0;
    }

    return sp;
}

/*============================================================================
 * 中断抽象实现
 *============================================================================*/

void hal_irq_controller_init(void) {
    // STM32 使用 NVIC, 无需额外初始化
}

void hal_irq_enable_irq(uint32_t irq) {
    NVIC->ISER[irq / 32] = (1 << (irq % 32));
}

void hal_irq_disable_irq(uint32_t irq) {
    NVIC->ICER[irq / 32] = (1 << (irq % 32));
}

void hal_irq_set_priority(uint32_t irq, uint32_t priority) {
    NVIC->IP[irq] = (uint8_t)(priority << 4);
}

int32_t hal_irq_get_active(void) {
    uint32_t ipsr;
    __asm volatile("mrs %0, ipsr" : "=r"(ipsr));
    return (ipsr & 0x1FF) - 16;  // 减去异常号偏移
}

void hal_irq_clear_pending(uint32_t irq) {
    NVIC->ICPR[irq / 32] = (1 << (irq % 32));
}

void hal_irq_set_vector(uint32_t irq, void (*handler)(void)) {
    // 向量表在 Flash 中, 通常不动态修改
    (void)irq;
    (void)handler;
}

/*============================================================================
 * 内存抽象实现
 *============================================================================*/

void *hal_get_sram_base(void) {
    return (void *)SRAM_BASE;
}

uint32_t hal_get_sram_size(void) {
    return SRAM_SIZE;
}

void *hal_get_flash_base(void) {
    return (void *)FLASH_BASE;
}

uint32_t hal_get_flash_size(void) {
    return FLASH_SIZE;
}

/*============================================================================
 * 调试抽象实现
 *============================================================================*/

void hal_debug_putc(char c) {
    // 使用 USART3 输出
    while (!(USART3->ISR & USART_ISR_TXE));
    USART3->TDR = c;
}

void hal_debug_puts(const char *s) {
    while (*s) {
        hal_debug_putc(*s++);
    }
}

uint32_t hal_get_timestamp(void) {
    // 使用 DWT 周期计数器
    static int dwt_init = 0;
    if (!dwt_init) {
        // 使能 DWT
        volatile uint32_t *DEMCR = (uint32_t *)0xE000EDFC;
        *DEMCR |= (1 << 24);
        volatile uint32_t *DWT_CYCCNT = (uint32_t *)0xE0001004;
        *DWT_CYCCNT = 0;
        volatile uint32_t *DWT_CTRL = (uint32_t *)0xE0001000;
        *DWT_CTRL |= 1;
        dwt_init = 1;
    }

    volatile uint32_t *DWT_CYCCNT = (uint32_t *)0xE0001004;
    return *DWT_CYCCNT;
}

/*============================================================================
 * 看门狗抽象实现
 *============================================================================*/

#if KERN_WATCHDOG_ENABLE

// IWDG 寄存器
#define IWDG_BASE       0x40003000UL
#define IWDG_KR         (*(volatile uint32_t *)(IWDG_BASE + 0x00))
#define IWDG_PR         (*(volatile uint32_t *)(IWDG_BASE + 0x04))
#define IWDG_RLR        (*(volatile uint32_t *)(IWDG_BASE + 0x08))
#define IWDG_SR         (*(volatile uint32_t *)(IWDG_BASE + 0x0C))

void hal_watchdog_init(uint32_t timeout_ms) {
    // 计算预分频和重载值
    // LSI = 32kHz
    uint32_t ticks = (32000UL * timeout_ms) / 1000;

    uint32_t prescaler = 0;  // 除以 4
    while (ticks > 0xFFF && prescaler < 6) {
        prescaler++;
        ticks /= 2;
    }

    // 解锁写保护
    IWDG_KR = 0x5555;

    // 设置预分频
    IWDG_PR = prescaler;

    // 设置重载值
    IWDG_RLR = ticks & 0xFFF;

    // 等待更新完成
    while (IWDG_SR & 1);

    // 启动看门狗
    IWDG_KR = 0xCCCC;
}

void hal_watchdog_feed(void) {
    IWDG_KR = 0xAAAA;
}

void hal_watchdog_stop(void) {
    // IWDG 一旦启动无法停止
}

#else

void hal_watchdog_init(void) {
}

void hal_watchdog_feed(void) {
}

#endif

/*============================================================================
 * 电源管理抽象实现
 *============================================================================*/

#if KERN_IDLE_SLEEP

void hal_idle_sleep(void) {
    __asm volatile("wfi");
}

void hal_set_clock_freq(uint32_t freq_hz) {
    (void)freq_hz;
    // TODO: 实现 PLL 重配置
}

uint32_t hal_get_clock_freq(void) {
    return 16000000UL;  // HSI 16MHz
}

#endif

/*============================================================================
 * 缓存抽象实现
 *============================================================================*/

void hal_icache_enable(void) {
    volatile uint32_t *ICIALLU = (uint32_t *)0xE000EF50;
    (void)ICIALLU;
    *ICIALLU = 0;

    volatile uint32_t *CCR = (uint32_t *)0xE000ED14;
    *CCR |= (1 << 17);  // ICACHE
}

void hal_dcache_enable(void) {
    volatile uint32_t *CSSELR = (uint32_t *)0xE000ED84;
    volatile uint32_t *CCR = (uint32_t *)0xE000ED14;

    // 选择数据缓存
    *CSSELR = 0;

    // 使能数据缓存
    *CCR |= (1 << 16);
}

void hal_dcache_flush(void) {
    volatile uint32_t *DCCISW = (uint32_t *)0xE000EF60;

    // 简单实现: 清理所有缓存
    for (int i = 0; i < 256; i++) {
        *DCCISW = (uint32_t)(i << 1);
    }
}

/*============================================================================
 * 平台信息
 *============================================================================*/

const char *hal_get_platform_name(void) {
    return "STM32F767ZI";
}

const char *hal_get_cpu_arch(void) {
    return "Cortex-M7";
}
