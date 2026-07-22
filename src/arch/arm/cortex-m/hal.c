/**
 * @file hal.c
 * @brief STM32F7 系列硬件抽象层（HAL）实现
 *
 * ============================================================================
 * 模块概述
 * ============================================================================
 *
 * 硬件抽象层（Hardware Abstraction Layer）是 RTOS 与硬件之间的桥梁。
 * 它将硬件相关的操作封装成统一的接口，使得内核代码可以跨平台运行。
 *
 * 本文件实现了 STM32F767ZI（Cortex-M7）的 HAL 接口，包括：
 *
 * 1. CPU 抽象
 *    - 中断使能/禁用
 *    - 临界区保护
 *    - 低功耗模式
 *
 * 2. 上下文切换支持
 *    - 栈初始化
 *    - PendSV/SVC 触发
 *
 * 3. 系统时钟
 *    - SysTick 初始化
 *    - 滴答计数
 *
 * 4. 中断控制器
 *    - NVIC 操作
 *    - 优先级设置
 *
 * 5. 调试支持
 *    - 串口输出
 *    - 时间戳
 *
 * ============================================================================
 * Cortex-M7 架构说明
 * ============================================================================
 *
 * 特殊寄存器：
 *   PRIMASK : 中断屏蔽寄存器（0=使能中断，1=禁用所有中断）
 *   BASEPRI : 基础优先级寄存器（屏蔽低于指定优先级的中断）
 *   CONTROL : 控制寄存器（SPSEL 位控制使用 MSP/PSP）
 *   IPSR    : 中断程序状态寄存器（当前异常号）
 *
 * 优先级规则：
 *   - 数值越小，优先级越高
 *   - STM32F7 使用 4 位优先级（0-15）
 *   - 优先级分组：抢占优先级 + 子优先级
 *
 * ============================================================================
 */

#include "hal.h"
#include "board_config.h"
#include "uart.h"
#include "kernel_config.h"

#if MPU_ENABLE
#include "mpu.h"
#endif

#if TARGET_BOARD == BOARD_RP2350_PICO2
extern void watchdog_reboot(uint32_t pc, uint32_t sp, uint32_t delay_ms);
extern void multicore_reset_core1(void);
#endif

/*============================================================================
 * 内部变量
 *============================================================================*/

/**
 * @brief SysTick 滴答计数器
 *
 * 由 SysTick_Handler() 递增，用于提供系统时间基准。
 */
static volatile uint32_t systick_count = 0;

/* Cortex-M Floating-Point Context Control Register. */
#define FPU_FPCCR_ADDR          0xE000EF34UL
#define FPU_FPCCR_ASPEN         (1UL << 31)
#define FPU_FPCCR_LSPEN         (1UL << 30)

#if TARGET_BOARD == BOARD_RP2350_PICO2
static uint32_t ram_vectors[128] __attribute__((aligned(256)));
extern uint32_t __vectors[];

void _default_handler(void) {
    while (1) {
        __asm volatile("wfi");
    }
}
#endif

/*============================================================================
 * CPU 抽象实现
 *============================================================================*/

/**
 * @brief 初始化 CPU
 *
 * 执行 CPU 相关的初始化：
 * 1. 设置向量表位置（Flash 起始地址）
 * 2. 初始化中断优先级
 */
void hal_cpu_init(void) {
#if TARGET_BOARD == BOARD_RP2350_PICO2
#if !SMP
    /*
     * A debugger reset (and some flash workflows) can reset core0 without
     * stopping core1.  If the previous image was SMP-enabled, that stale
     * core1 continues consuming the new UP kernel's shared ready queue and
     * indexes its single-entry per-CPU arrays with cpu_id == 1.  Reset it
     * into the SDK's boot-ROM launch wait before any kernel state exists.
     * SMP builds deliberately leave this to smp_init_core1().
     */
    if (sio_hw->cpuid == 0U) {
        multicore_reset_core1();
    }
#endif

    const uint32_t *flash_vectors = __vectors;
    for (uint32_t i = 0; i < (16U + BOARD_IRQ_COUNT); i++) {
        ram_vectors[i] = flash_vectors[i];
    }
    SCB->VTOR = (uint32_t)(uintptr_t)ram_vectors;
    __asm volatile("dsb");
    __asm volatile("isb");
#endif

    /* Configure deterministic FP exception frames before kernel C code runs. */
    hal_fpu_context_init();

    /* 初始化中断优先级 */
    hal_interrupt_priority_init();

#if MPU_ENABLE
    /* 使能 MPU 内存保护 (微内核安全基础) */
    mpu_init();
#endif
}

void hal_fpu_context_init(void) {
#if defined(__ARM_FP) && (__ARM_FP != 0)
    volatile uint32_t *fpccr = (volatile uint32_t *)FPU_FPCCR_ADDR;

    /*
     * Keep ASPEN enabled, but disable lazy preservation.  A context switch
     * must see either a complete basic frame or a complete extended frame;
     * it must never carry a deferred frame that still points at a discarded
     * bootstrap/other-task stack.  Eager stacking costs 72 bytes only for
     * tasks that actually own an FP context and makes the layout deterministic
     * on both RP2350 cores.
     */
    *fpccr = (*fpccr | FPU_FPCCR_ASPEN) & ~FPU_FPCCR_LSPEN;
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");

    /* The bootstrap thread's FP registers are not part of any RTOS task. */
    __asm volatile(
        "mrs r0, control\n"
        "bic r0, r0, #4\n"
        "msr control, r0\n"
        "dsb\n"
        "isb\n"
        ::: "r0", "memory");
#endif
}

/**
 * @brief 使能全局中断
 *
 * 使用 CPSIE I 指令清除 PRIMASK，使能所有可配置优先级中断。
 *
 * @note 此函数不返回之前的中断状态
 */
void hal_irq_enable(void) {
    __asm volatile("cpsie i");
}

/**
 * @brief 禁用全局中断
 *
 * 使用 CPSID I 指令设置 PRIMASK，禁用所有可配置优先级中断。
 *
 * @note 此函数不保存之前的中断状态
 */
void hal_irq_disable(void) {
    __asm volatile("cpsid i");
}

/**
 * @brief 保存并禁用中断
 *
 * 读取当前 PRIMASK 值，然后禁用中断。
 * 返回值用于 hal_irq_restore() 恢复之前的状态。
 *
 * @return 之前的 PRIMASK 值
 *
 * @code
 * uint32_t state = hal_irq_save();
 * // 临界区代码
 * hal_irq_restore(state);
 * @endcode
 */
uint32_t hal_irq_save(void) {
    uint32_t result;
    __asm volatile("mrs %0, primask" : "=r"(result));
    __asm volatile("cpsid i");
    return result;
}

/**
 * @brief 恢复中断状态
 *
 * 恢复之前保存的 PRIMASK 值。
 *
 * @param primask 之前由 hal_irq_save() 返回的值
 */
void hal_irq_restore(uint32_t primask) {
    __asm volatile("msr primask, %0" :: "r"(primask));
}

/*============================================================================
 * 临界区实现
 * ============================================================================
 *
 * 使用 PRIMASK 方式实现临界区:
 * - PRIMASK = 1: 禁用所有可配置优先级中断
 * - PRIMASK = 0: 使能所有中断
 *
 * 为什么使用 PRIMASK 而不是 BASEPRI？
 *
 * BASEPRI 方式的问题:
 * - BASEPRI = N 屏蔽优先级 >= N 的中断
 * - PendSV 优先级 = 15 (最低)
 * - 要让 PendSV 在临界区外执行, 需要 BASEPRI > 15, 但最大值是 15
 * - 设置 BASEPRI = 16 写入 0x100, Cortex-M7 只实现 bits[7:4], 截断为 0x00
 * - 这意味着临界区不屏蔽任何中断 - 完全错误!
 *
 * PRIMASK 方式:
 * - 完全禁用所有中断
 * - PendSV 被挂起但延迟执行
 * - 退出临界区后 PendSV 立即执行
 * - 这是 RTOS 临界区的正确实现方式
 *============================================================================*/

/**
 * @brief 进入临界区
 *
 * 使用 PRIMASK 实现临界区保护:
 * 1. 保存当前 PRIMASK 值
 * 2. 设置 PRIMASK = 1 (禁用所有中断)
 *
 * @return 之前的 PRIMASK 值，用于 hal_exit_critical() 恢复
 *
 * @note 临界区内所有中断被禁用
 * @note 支持临界区嵌套
 */
uint32_t hal_enter_critical(void) {
    return hal_irq_save();
}

/**
 * @brief 退出临界区
 *
 * 恢复之前保存的 PRIMASK 值。
 *
 * @param primask 之前由 hal_enter_critical() 返回的值
 */
void hal_exit_critical(uint32_t primask) {
    hal_irq_restore(primask);
}

/**
 * @brief 初始化中断优先级
 *
 * 设置系统异常的优先级：
 * - SVC：最高优先级（0），用于首次任务切换
 * - PendSV：最低优先级（15），用于上下文切换
 * - SysTick：最低优先级（15），用于系统滴答
 *
 * 为什么 PendSV 和 SysTick 要设为最低优先级？
 * - PendSV 用于上下文切换，不应打断其他中断
 * - SysTick 定期触发，不应影响高优先级中断
 * - 这样可以保证高优先级中断的实时响应
 */
void hal_interrupt_priority_init(void) {
    /*
     * Cortex-M7 SHPR 寄存器（32 位访问）：
     *
     * SHPR[0] = SHPR1 (0xE000ED18): MemManage, BusFault, UsageFault
     * SHPR[1] = SHPR2 (0xE000ED1C): bits 31:24 = SVCall priority
     * SHPR[2] = SHPR3 (0xE000ED20): bits 31:24 = SysTick, bits 23:16 = PendSV
     *
     * 优先级值：0 = 最高，15 = 最低（4 位优先级）
     */

    /* SVC 优先级为最高（0）- 用于首次切换 */
    SCB->SHPR[1] = (0 << 24);

    /* PendSV 和 SysTick 优先级为最低 */
    SCB->SHPR[2] = (PENDSV_PRIORITY << 20) | (SYSTICK_PRIORITY << 28);
}

/**
 * @brief 触发 PendSV 异常
 *
 * 设置 ICSR 寄存器的 PENDSVSET 位（bit 28），触发 PendSV 异常。
 * PendSV 异常会在当前中断处理完成后执行（因为优先级最低）。
 *
 * @note PendSV 优先级最低，不会打断其他中断
 */
void hal_trigger_pendsv(void) {
    /* 设置 PENDSVSET 位触发本核 PendSV */
    SCB->ICSR = (1 << 28);

    /* SMP-B (IPI):暂不实现核间中断。
     * RP2350 Cortex-M33 的 SGIR 可发 SGI 到另一核,但需要注册
     * SGI handler (NVIC IRQ 0-15),且 RP2350 的 SGI 行为需要验证。
     * 当前方案:无 IPI,靠 tick handler 的 idle 旁路检查 ready_bitmap
     * 触发 PendSV。跨核唤醒延迟最多 1ms (一个 tick)。 */

    __asm volatile("dsb");
    __asm volatile("isb");
}

/**
 * @brief 触发 SVC 异常
 *
 * @param svc_num SVC 号（暂未使用）
 *
 * @note SVC 用于首次任务切换
 */
void hal_trigger_svc(uint32_t svc_num) {
    (void)svc_num;
    __asm volatile("svc #0");
}

/**
 * @brief 进入低功耗模式
 *
 * 执行 WFI（Wait For Interrupt）指令，进入睡眠模式。
 * 当有中断发生时，CPU 自动唤醒。
 */
void hal_enter_lowpower(void) {
    __asm volatile("wfi");
}

/**
 * @brief 退出低功耗模式
 *
 * WFI 指令会在中断发生时自动退出，无需额外操作。
 */
void hal_exit_lowpower(void) {
    /* WFI 自动退出 */
}

/**
 * @brief 获取 CPU ID
 *
 * @return 0（单核 CPU）
 */
uint32_t hal_get_cpu_id(void) {
#if TARGET_BOARD == BOARD_RP2350_PICO2 && SMP
    return sio_hw->cpuid;
#else
    /* A UP kernel has exactly one per-CPU slot, regardless of the SoC. */
    return 0;
#endif
}

/*============================================================================
 * SysTick 抽象实现
 * ============================================================================
 *
 * SysTick 是 Cortex-M 内置的系统定时器：
 * - 24 位递减计数器
 * - 计数到 0 时产生中断
 * - 常用于 RTOS 的系统滴答
 *============================================================================*/

/**
 * @brief 初始化 SysTick 定时器
 *
 * @param rate_hz 滴答频率（Hz）
 *
 * 配置 SysTick 以指定频率产生中断。
 * 使用 CPU 时钟（SYSCLK）作为时钟源。
 */
void hal_systick_init(uint32_t rate_hz) {
    extern uint32_t hal_get_sysclk(void);
    uint32_t reload = (hal_get_sysclk() / rate_hz) - 1;

    /* 设置重载值 */
    SYSTICK->RVR = reload;

    /* 清除当前值（写入任意值都会清零） */
    SYSTICK->CVR = 0;

    /* 使能 SysTick
     *
     * CSR 寄存器位定义：
     * - bit 0 (ENABLE): 使能计数器
     * - bit 1 (TICKINT): 使能中断
     * - bit 2 (CLKSOURCE): 时钟源（1=CPU时钟，0=外部时钟）
     */
    SYSTICK->CSR = SYSTICK_CSR_ENABLE | SYSTICK_CSR_TICKINT | SYSTICK_CSR_CLKSRC;
}

/**
 * @brief 获取 SysTick 计数
 * @return 当前滴答计数
 */
uint32_t hal_systick_get(void) {
    return systick_count;
}

/**
 * @brief 获取系统滴答计数
 * @return 当前滴答计数
 */
uint32_t hal_get_tick_count(void) {
    return systick_count;
}

/**
 * @brief 设置系统滴答计数
 * @param count 新的计数值
 */
void hal_set_tick_count(uint32_t count) {
    systick_count = count;
}

/**
 * @brief 使能 SysTick
 */
void hal_systick_enable(void) {
    SYSTICK->CSR |= SYSTICK_CSR_ENABLE | SYSTICK_CSR_TICKINT;
}

/**
 * @brief 禁用 SysTick
 */
void hal_systick_disable(void) {
    SYSTICK->CSR &= ~(SYSTICK_CSR_ENABLE | SYSTICK_CSR_TICKINT);
}

/*============================================================================
 * SysTick 中断处理
 *============================================================================*/

/**
 * @brief SysTick 中断处理程序
 *
 * 由硬件自动调用，执行以下操作：
 * 1. 递增滴答计数
 * 2. 调用内核滴答处理函数
 *
 * @note 此函数在中断上下文中执行
 */
void SysTick_Handler(void) {
    systick_count++;

    /* 调用内核滴答处理
     *
     * kern_tick_handler() 会：
     * - 更新调度器滴答计数
     * - 处理时间片轮转
     * - 检查超时唤醒
     */
    extern void kern_tick_handler(void);
    kern_tick_handler();
}

#if TARGET_BOARD == BOARD_RP2350_PICO2
void isr_systick(void) __attribute__((alias("SysTick_Handler")));
#endif

/*============================================================================
 * 上下文切换实现
 *============================================================================*/

/* 汇编函数声明（在 context.S 中实现） */
extern void hal_context_switch(void **from, void *to);
extern void hal_context_switch_first(void *to);

/**
 * @brief 初始化任务栈
 *
 * 为新任务创建初始栈帧，使得任务第一次运行时能够正确恢复上下文。
 *
 * @param stack_top  栈顶地址（高地址）
 * @param stack_size 栈大小（字节）
 * @param entry      任务入口函数
 * @param arg        任务参数
 * @param exit       任务退出处理函数
 *
 * @return 初始化后的栈指针（SP）
 *
 * ============================================================================
 * 栈帧布局（从高地址到低地址）
 * ============================================================================
 *
 *   高地址 ─────────────────────────
 *          │    xPSR     │ 0x01000000 (Thumb bit)
 *          ├─────────────┤
 *          │     PC      │ 任务入口函数
 *          ├─────────────┤
 *          │     LR      │ 任务退出处理
 *          ├─────────────┤
 *          │     R12     │ 0
 *          ├─────────────┤
 *          │     R3      │ 0
 *          ├─────────────┤
 *          │     R2      │ 0
 *          ├─────────────┤
 *          │     R1      │ 0
 *          ├─────────────┤
 *          │     R0      │ 任务参数
 *          ├─────────────┤  <- 硬件帧结束
 *          │     R11     │ 0
 *          ├─────────────┤
 *          │     R10     │ 0
 *          ├─────────────┤
 *          │     R9      │ 0
 *          ├─────────────┤
 *          │     R8      │ 0
 *          ├─────────────┤
 *          │     R7      │ 0
 *          ├─────────────┤
 *          │     R6      │ 0
 *          ├─────────────┤
 *          │     R5      │ 0
 *          ├─────────────┤
 *          │     R4      │ 0
 *          ├─────────────┤  <- 软件帧结束
 *          │   (padding) │ 可选的对齐填充
 *   低地址 ─────────────────────────
 *                    ↑
 *                   SP
 *
 * ============================================================================
 * 异常返回流程
 * ============================================================================
 *
 * 当 PendSV 恢复上下文时：
 * 1. 从 SP 恢复 R4-R11（软件帧）
 * 2. 异常返回时硬件自动恢复 xPSR, PC, LR, R12, R3-R0（硬件帧）
 * 3. PC 加载任务入口函数地址，开始执行任务
 * 4. R0 包含任务参数
 * 5. LR 包含任务退出处理函数地址（任务返回时调用）
 */
void *hal_stack_init(void    *stack_top,
                     uint32_t stack_size,
                     void    *entry,
                     void    *arg,
                     void    *exit)
{
    uint8_t *stack_base = (uint8_t *)stack_top - stack_size;

    /*
     * 栈水位/溢出检测：整栈填充魔数。
     *
     * 任务运行后，栈从高地址向低地址增长并覆盖魔数。
     * shell 的 ps/mem/top 命令依赖这个模式估算真实栈水位；
     * 栈底前 16 字节仍作为溢出 guard 检查。
     */
    for (uint32_t i = 0; i < stack_size; i++) {
        stack_base[i] = STACK_MAGIC_BYTE;
    }

    uint32_t *sp = (uint32_t *)((uint8_t *)stack_top);

    /*
     * 初始化硬件帧（异常返回时硬件自动恢复）
     *
     * Cortex-M 异常栈帧格式：
     * xPSR, PC, LR, R12, R3, R2, R1, R0
     */
    sp--; *sp = 0x01000000UL;   /* xPSR: Thumb bit 必须置 1 */
    sp--; *sp = (uint32_t)entry; /* PC: 任务入口函数 */
    sp--; *sp = (uint32_t)exit;  /* LR: 任务退出处理 */
    sp--; *sp = 0;               /* R12 */
    sp--; *sp = 0;               /* R3 */
    sp--; *sp = 0;               /* R2 */
    sp--; *sp = 0;               /* R1 */
    sp--; *sp = (uint32_t)arg;   /* R0: 任务参数 */

    /*
     * 初始化软件帧（PendSV 恢复）
     *
     * R4-R11 是被调用者保存寄存器，需要手动保存/恢复。
     */
    sp--; *sp = 0;  /* R11 */
    sp--; *sp = 0;  /* R10 */
    sp--; *sp = 0;  /* R9 */
    sp--; *sp = 0;  /* R8 */
    sp--; *sp = 0;  /* R7 */
    sp--; *sp = 0;  /* R6 */
    sp--; *sp = 0;  /* R5 */
    sp--; *sp = 0;  /* R4 */

    /*
     * 8 字节对齐
     *
     * AAPCS（ARM Architecture Procedure Call Standard）要求
     * 栈指针在函数调用时必须 8 字节对齐。
     */
    if ((uint32_t)sp & 4) {
        sp--;
        *sp = 0;
    }

    return sp;
}

/*============================================================================
 * 中断控制器抽象实现
 * ============================================================================
 *
 * NVIC（Nested Vectored Interrupt Controller）是 Cortex-M 的中断控制器：
 * - 支持嵌套中断
 * - 支持优先级
 * - 向量化中断（每个中断有独立的处理函数）
 *============================================================================*/

/**
 * @brief 初始化中断控制器
 *
 * STM32 使用 NVIC，无需额外初始化。
 */
void hal_irq_controller_init(void) {
    /* STM32 使用 NVIC, 无需额外初始化 */
}

/**
 * @brief 使能指定中断
 * @param irq 中断号
 */
void hal_irq_enable_irq(uint32_t irq) {
    NVIC->ISER[irq / 32] = (1 << (irq % 32));
}

/**
 * @brief 禁用指定中断
 * @param irq 中断号
 */
void hal_irq_disable_irq(uint32_t irq) {
    NVIC->ICER[irq / 32] = (1 << (irq % 32));
}

/**
 * @brief 设置中断优先级
 *
 * @param irq      中断号
 * @param priority 优先级（0-15，数值越小优先级越高）
 */
void hal_irq_set_priority(uint32_t irq, uint32_t priority) {
    NVIC->IP[irq] = (uint8_t)(priority << 4);
}

/**
 * @brief 获取当前活动的中断号
 *
 * @return 当前中断号，如果没有中断活动则返回负值
 */
int32_t hal_irq_get_active(void) {
    uint32_t ipsr;
    __asm volatile("mrs %0, ipsr" : "=r"(ipsr));
    return (ipsr & 0x1FF) - 16;  /* 减去异常号偏移 */
}

/**
 * @brief 清除中断挂起状态
 * @param irq 中断号
 */
void hal_irq_clear_pending(uint32_t irq) {
    NVIC->ICPR[irq / 32] = (1 << (irq % 32));
}

/**
 * @brief 设置中断向量
 *
 * @param irq     中断号
 * @param handler 中断处理函数
 *
 * @note STM32F767 的向量表在 Flash 中，通常不动态修改
 */
void hal_irq_set_vector(uint32_t irq, void (*handler)(void)) {
#if TARGET_BOARD == BOARD_RP2350_PICO2
    uint32_t *vectors = ram_vectors;
#else
    extern uint32_t __ram_vector_start[];
    uint32_t *vectors = __ram_vector_start;
#endif

    /* IRQ 号映射到向量表索引: 条目 = irq + 16 (跳过系统异常) */
    uint32_t index = (uint32_t)irq + 16;

    if (irq < BOARD_IRQ_COUNT) {
        vectors[index] = (uint32_t)(uintptr_t)handler | 1;  /* Thumb bit */
        __asm volatile("dmb");
        __asm volatile("isb");
    }
}

/*============================================================================
 * 内存抽象实现
 *============================================================================*/

/**
 * @brief 获取 SRAM 基地址
 * @return SRAM 起始地址
 */
void *hal_get_sram_base(void) {
    return (void *)BOARD_SRAM_BASE;
}

/**
 * @brief 获取 SRAM 大小
 * @return SRAM 大小（字节）
 */
uint32_t hal_get_sram_size(void) {
    return BOARD_SRAM_SIZE;
}

/**
 * @brief 获取 Flash 基地址
 * @return Flash 起始地址
 */
void *hal_get_flash_base(void) {
    return (void *)BOARD_FLASH_BASE;
}

/**
 * @brief 获取 Flash 大小
 * @return Flash 大小（字节）
 */
uint32_t hal_get_flash_size(void) {
    return BOARD_FLASH_SIZE;
}

/*============================================================================
 * 调试抽象实现
 *============================================================================*/

/**
 * @brief 输出单个字符到调试串口
 * @param c 要输出的字符
 *
 * 使用 USART3 输出（Nucleo 板的虚拟串口）。
 */
void hal_debug_putc(char c) {
#if TARGET_BOARD == BOARD_RP2350_PICO2
    uart_putc(BOARD_DEFAULT_UART, c);
#else
    /* 等待发送缓冲区为空 */
    while (!(USART3->ISR & USART_ISR_TXE));
    /* 写入数据寄存器 */
    USART3->TDR = c;
    /* 等待发送完成，防止后续输出覆盖 */
    while (!(USART3->ISR & USART_ISR_TC));
#endif
}

/**
 * @brief 输出字符串到调试串口
 * @param s 要输出的字符串
 */
void hal_debug_puts(const char *s) {
    while (*s) {
        hal_debug_putc(*s++);
    }
}

/**
 * @brief 获取高精度时间戳
 *
 * 使用 DWT（Data Watchpoint and Trace）的周期计数器。
 * 计数器每个 CPU 时钟周期递增一次。
 *
 * @return 周期计数值
 *
 * @note 用于性能测量和精确定时
 */
uint32_t hal_get_timestamp(void) {
    /* 使用 DWT 周期计数器 */
    static int dwt_init = 0;
    if (!dwt_init) {
        /* 使能 DWT
         *
         * DEMCR（Debug Exception and Monitor Control Register）
         * bit 24 (TRCENA): 使能 DWT
         */
        volatile uint32_t *DEMCR = (uint32_t *)0xE000EDFC;
        *DEMCR |= (1 << 24);

        /* 清零周期计数器 */
        volatile uint32_t *DWT_CYCCNT = (uint32_t *)0xE0001004;
        *DWT_CYCCNT = 0;

        /* 使能周期计数器
         *
         * DWT_CTRL bit 0 (CYCCNTENA): 使能周期计数器
         */
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

/*
 * 看门狗是板级外设。STM32 走 IWDG，RP2350 走 hardware_watchdog (SDK)。
 * 当前 RP2350 配置关掉了 KERNEL_WATCHDOG — 走 stub 分支。后续若开启，
 * 在 src/board/rp2350/ 下加一个 rp2350_watchdog.c 实现，不要在 hal.c 里
 * #include STM32 寄存器。
 */
#if TARGET_BOARD == BOARD_RP2350_PICO2 && KERN_WATCHDOG_ENABLE
#error "RP2350 watchdog not implemented here; provide src/board/rp2350/rp2350_watchdog.c and link it."
#endif

#if KERN_WATCHDOG_ENABLE && TARGET_BOARD == BOARD_STM32F767_NUCLEO

/**
 * @brief IWDG 寄存器定义
 *
 * IWDG（Independent Watchdog）是 STM32 的独立看门狗：
 * - 使用独立的 LSI 时钟（32kHz）
 * - 一旦启动无法停止
 * - 必须定期喂狗，否则复位系统
 */
#define IWDG_BASE       0x40003000UL
#define IWDG_KR         (*(volatile uint32_t *)(IWDG_BASE + 0x00))  /* Key register */
#define IWDG_PR         (*(volatile uint32_t *)(IWDG_BASE + 0x04))  /* Prescaler register */
#define IWDG_RLR        (*(volatile uint32_t *)(IWDG_BASE + 0x08))  /* Reload register */
#define IWDG_SR         (*(volatile uint32_t *)(IWDG_BASE + 0x0C))  /* Status register */

/**
 * @brief 初始化看门狗
 *
 * @param timeout_ms 超时时间（毫秒）
 *
 * 配置独立看门狗的超时时间。
 */
void hal_watchdog_init(uint32_t timeout_ms) {
    /* Enable PWR clock + backup domain access, then enable LSI */
    RCC->APB1ENR |= (1U << 28);  /* PWREN */
    DSB();
    PWR->CR1 |= (1U << 8);       /* DBP: disable backup protection */
    DSB();
    RCC->CSR |= RCC_CSR_LSION;
    while (!(RCC->CSR & RCC_CSR_LSIRDY));

    /* 计算预分频和重载值
     *
     * LSI = 32kHz
     * 超时时间 = (RLR + 1) * 预分频 / LSI
     */
    uint32_t ticks = (32000UL * timeout_ms) / 1000;

    uint32_t prescaler = 0;  /* 除以 4 */
    while (ticks > 0xFFF && prescaler < 6) {
        prescaler++;
        ticks /= 2;
    }

    /* 解锁写保护
     *
     * 写入 0x5555 到 KR 寄存器解锁 IWDG_PR 和 IWDG_RLR
     */
    IWDG_KR = 0x5555;

    /* 设置预分频 */
    IWDG_PR = prescaler;

    /* 设置重载值 */
    IWDG_RLR = ticks & 0xFFF;

    /* 等待更新完成 */
    while (IWDG_SR & 1);

    /* 启动看门狗 */
    IWDG_KR = 0xCCCC;
}

/**
 * @brief 喂狗
 *
 * 重置看门狗计数器，防止系统复位。
 */
void hal_watchdog_feed(void) {
    IWDG_KR = 0xAAAA;
}

/**
 * @brief 读取复位原因
 *
 * @return RCC_CSR 复位标志位
 */
uint32_t hal_watchdog_reset_cause(void) {
    return *(volatile uint32_t *)0x40023874;  /* RCC_CSR */
}

/**
 * @brief 停止看门狗
 *
 * @note IWDG 一旦启动无法停止
 */
void hal_watchdog_stop(void) {
    /* IWDG 一旦启动无法停止 */
}

#else

/* 看门狗未启用时的空实现 */
void hal_watchdog_init(void) {
}

void hal_watchdog_feed(void) {
}

uint32_t hal_watchdog_reset_cause(void) {
    return 0;
}

#endif /* KERN_WATCHDOG_ENABLE */

/*============================================================================
 * 电源管理抽象实现
 *============================================================================*/

#if KERN_IDLE_SLEEP

/**
 * @brief 进入空闲睡眠模式
 */
void hal_idle_sleep(void) {
    __asm volatile("wfi");
}

#endif /* KERN_IDLE_SLEEP */

/**
 * @brief 获取 CPU 时钟频率
 * @return 当前 SYSCLK 频率（Hz）
 */
uint32_t hal_get_clock_freq(void) {
    extern uint32_t hal_get_sysclk(void);
    return hal_get_sysclk();
}

/*============================================================================
 * 缓存抽象实现
 * ============================================================================
 *
 * Cortex-M7 内置指令缓存（I-Cache）和数据缓存（D-Cache）：
 * - 提高代码执行速度
 * - 减少总线访问延迟
 * - 需要在使用 DMA 时注意缓存一致性
 *============================================================================*/

/**
 * @brief 使能指令缓存
 */
void hal_icache_enable(void) {
#if TARGET_BOARD == BOARD_STM32F767_NUCLEO
    /* 使能指令缓存前，先无效化整个缓存 */
    volatile uint32_t *ICIALLU = (uint32_t *)0xE000EF50;
    (void)ICIALLU;
    *ICIALLU = 0;

    /* 设置 CCR 寄存器的 ICACHE 位（bit 17） */
    volatile uint32_t *CCR = (uint32_t *)0xE000ED14;
    *CCR |= (1 << 17);
#endif
}

/**
 * @brief 使能数据缓存
 */
void hal_dcache_enable(void) {
#if TARGET_BOARD == BOARD_STM32F767_NUCLEO
    volatile uint32_t *CSSELR = (uint32_t *)0xE000ED84;
    volatile uint32_t *CCR = (uint32_t *)0xE000ED14;

    /* 选择数据缓存 */
    *CSSELR = 0;

    /* 设置 CCR 寄存器的 DCACHE 位（bit 16） */
    *CCR |= (1 << 16);
#endif
}

/**
 * @brief 刷新数据缓存
 *
 * 将数据缓存中的脏数据写入内存。
 * 在使用 DMA 传输前调用，确保缓存一致性。
 */
void hal_dcache_flush(void) {
#if TARGET_BOARD == BOARD_STM32F767_NUCLEO
    volatile uint32_t *DCCISW = (uint32_t *)0xE000EF60;

    /* 简单实现: 清理所有缓存行 */
    for (int i = 0; i < 256; i++) {
        *DCCISW = (uint32_t)(i << 1);
    }
#endif
}

/*============================================================================
 * 平台信息
 *============================================================================*/

/**
 * @brief 获取平台名称
 * @return 平台名称字符串
 */
const char *hal_get_platform_name(void) {
    return MCU_NAME;
}

/**
 * @brief 获取 CPU 架构
 * @return 架构名称字符串
 */
const char *hal_get_cpu_arch(void) {
    return CPU_CORE;
}

/*============================================================================
 * 系统复位
 *============================================================================*/

void hal_system_reset(void) {
    hal_irq_disable();
    SYSTICK->CSR = 0;

#if TARGET_BOARD == BOARD_RP2350_PICO2
    /* RP2350:用 watchdog_reboot 完整重启两个核。
     * AIRCR SYSRESETREQ 只复位当前核 (core0),
     * reset 后 core1 状态不对,导致 smp_init_core1 失败。
     * watchdog_reboot(0,0,...) 走完整 boot path,两核都重新初始化。 */
    watchdog_reboot(0, 0, 1);
#else
    volatile uint32_t *aircr = (volatile uint32_t *)0xE000ED0C;
    *aircr = (0x05FAUL << 16) | (1U << 2);
#endif

    __asm volatile("dsb");
    while (1);
}
