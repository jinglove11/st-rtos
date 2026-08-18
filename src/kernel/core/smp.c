/**
 * @file smp.c
 * @brief Core completion #7 S3 — SMP core1 launch + entry
 *
 * Launches the RP2350's second Cortex-M33 core and owns the SIO FIFO IPI
 * transport used by the per-CPU scheduler.
 */

#include "kernel_config.h"

#if SMP

#include "smp.h"
#include "hal.h"
#include "scheduler.h"
#include "task.h"
#include "mpu.h"
#include "hardware/structs/sio.h"
#include "hardware/regs/intctrl.h"
#include "pico/platform.h"
#include <string.h>

static volatile uint32_t core1_ready;
static volatile uint32_t ipi_ready[SMP_MAX_CPUS];
static volatile uint32_t ipi_pending[SMP_MAX_CPUS];
static volatile uint32_t flash_lockout_requested;
static volatile uint32_t flash_lockout_ack[SMP_MAX_CPUS];

/* DBG: core1 生命周期探针 (纯 RAM 写,零时序扰动;openocd AHB-AP 免挂起读)。
 * stage 单调递增 (max-write),idle 的 5 不会覆盖 IRQ 路径的 6-10。 */
volatile uint32_t core1_stage;
volatile uint32_t core1_hb;
volatile uint32_t core1_hb_irq;
volatile uint32_t core1_hb_tick;

void smp_debug_mark1(uint32_t v) {
    if (v > core1_stage) {
        core1_stage = v;
    }
}

#define SMP_FIFO_KICK 0x4D315049UL /* "M1PI" */

/* This loop must remain executable while QSPI XIP is disabled by a flash
 * erase/program operation.  The FIFO handler enters it before acknowledging
 * the requesting CPU, so the requester cannot disable XIP too early. */
static void __attribute__((noinline))
__not_in_flash_func(smp_flash_lockout_park)(uint32_t cpu) {
    uint32_t primask;
    __asm volatile("mrs %0, primask" : "=r"(primask));
    if (cpu == 1U) {
        smp_debug_mark1(8);
    }
    __asm volatile("cpsid i" ::: "memory");

    __atomic_store_n(&flash_lockout_ack[cpu], 1U, __ATOMIC_RELEASE);
    __asm volatile("sev");

    while (__atomic_load_n(&flash_lockout_requested,
                            __ATOMIC_ACQUIRE) != 0U) {
        __asm volatile("wfe");
    }

    __atomic_store_n(&flash_lockout_ack[cpu], 0U, __ATOMIC_RELEASE);
    __asm volatile("sev");
    __asm volatile("msr primask, %0" :: "r"(primask) : "memory");
}

static inline int smp_fifo_rvalid(void) {
    return (sio_hw->fifo_st & SIO_FIFO_ST_VLD_BITS) != 0U;
}

static inline int smp_fifo_wready(void) {
    return (sio_hw->fifo_st & SIO_FIFO_ST_RDY_BITS) != 0U;
}

static inline void smp_fifo_drain(void) {
    while (smp_fifo_rvalid()) {
        (void)sio_hw->fifo_rd;
    }
}

static inline void smp_fifo_clear_irq(void) {
    sio_hw->fifo_st = 0xFFU;
}

/* 必须驻留 SRAM: flash 擦写窗口内 (XIP 不可用) 到达的 FIFO 中断若从
 * flash 取指,会执行 QMI 死区模式 (0xbbbbbbbb) → 寄存器/PSP 被垃圾
 * 污染 → 后续异常压栈 MemManage → lockup → core1 整核复位。 */
static void __attribute__((noinline))
__not_in_flash_func(smp_fifo_irq_handler)(void) {
    uint32_t cpu = hal_get_cpu_id();

    if (cpu == 1U) {
        core1_hb_irq++;
        smp_debug_mark1(core1_hb_irq >= 2U ? 15U : 6U);
    }

    /* Drain every token before clearing sticky FIFO error/IRQ state. */
    while (smp_fifo_rvalid()) {
        (void)sio_hw->fifo_rd;
    }
    smp_fifo_clear_irq();

    /* Atomic exchange closes the race with the remote OR.  If another token
     * arrives after this point, hardware will pend the IRQ again. */
    uint32_t reasons = __atomic_exchange_n(&ipi_pending[cpu], 0U,
                                            __ATOMIC_ACQ_REL);
    if ((reasons & SMP_IPI_FLASH_LOCKOUT) != 0U) {
        if (cpu == 1U) {
            smp_debug_mark1(7);
        }
        smp_flash_lockout_park(cpu);
        if (cpu == 1U) {
            smp_debug_mark1(9);
        }
        reasons &= ~SMP_IPI_FLASH_LOCKOUT;
    }
    if (reasons != 0U) {
        sched_handle_ipi(reasons);
    }
}

static void smp_ipi_init_cpu(void) {
    uint32_t cpu = hal_get_cpu_id();
    uint32_t irq = (uint32_t)SIO_IRQ_FIFO;

    smp_fifo_drain();
    smp_fifo_clear_irq();
    hal_irq_set_vector(irq, smp_fifo_irq_handler);
    hal_irq_set_priority(irq, 1U);
    hal_irq_clear_pending(irq);
    hal_irq_enable_irq(irq);
    ipi_ready[cpu] = 1U;
    __asm volatile("dmb" ::: "memory");
    __asm volatile("sev");
}

void smp_send_ipi(uint32_t target_cpu, uint32_t reasons) {
    uint32_t cpu = hal_get_cpu_id();

    if (target_cpu >= SMP_MAX_CPUS || target_cpu == cpu || reasons == 0U ||
        ipi_ready[target_cpu] == 0U) {
        return;
    }

    __atomic_fetch_or(&ipi_pending[target_cpu], reasons, __ATOMIC_RELEASE);

    /* A full FIFO already guarantees an interrupt at the receiver.  Reasons
     * are coalesced in shared memory, so a token can be safely omitted. */
    if (smp_fifo_wready()) {
        sio_hw->fifo_wr = SMP_FIFO_KICK;
        __asm volatile("sev");
    }
}

kern_err_t smp_flash_lockout_start(void) {
    uint32_t cpu = hal_get_cpu_id();
    uint32_t peer = cpu ^ 1U;

    if (cpu >= SMP_MAX_CPUS || peer >= SMP_MAX_CPUS) {
        return KERN_ERR_PARAM;
    }

    /* Before the second CPU installs its FIFO handler it cannot be executing
     * an RTOS task from XIP, so there is nothing to park. */
    if (__atomic_load_n(&ipi_ready[peer], __ATOMIC_ACQUIRE) == 0U) {
        return KERN_OK;
    }

    __atomic_store_n(&flash_lockout_ack[peer], 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&flash_lockout_requested, 1U, __ATOMIC_RELEASE);
    smp_send_ipi(peer, SMP_IPI_FLASH_LOCKOUT);

    /* A bounded wait converts a broken IPI path into an ordinary I/O error
     * instead of disabling XIP while the peer is still running from flash. */
    for (uint32_t spin = 0; spin < 10000000U; spin++) {
        if (__atomic_load_n(&flash_lockout_ack[peer],
                            __ATOMIC_ACQUIRE) != 0U) {
            return KERN_OK;
        }
        __asm volatile("nop");
    }

    __atomic_store_n(&flash_lockout_requested, 0U, __ATOMIC_RELEASE);
    __asm volatile("sev");
    return KERN_ERR_TIMEOUT;
}

void smp_flash_lockout_end(void) {
    uint32_t cpu = hal_get_cpu_id();
    uint32_t peer = cpu ^ 1U;

    __atomic_store_n(&flash_lockout_requested, 0U, __ATOMIC_RELEASE);
    __asm volatile("sev");

    if (peer < SMP_MAX_CPUS) {
        while (__atomic_load_n(&flash_lockout_ack[peer],
                               __ATOMIC_ACQUIRE) != 0U) {
            __asm volatile("wfe");
        }
    }
}

/* The SDK's multicore launch provides a stack for core1 (core1_stack in
 * pico_multicore). We just need an entry function. */

/**
 * core1_entry — the C entry point for core1 after the SDK trampoline.
 *
 * Core1 inherits the same vector table (VTOR) as core0 (the SDK passes it).
 * It needs to: set up its own exception priorities (per-core SCB), enable
 * its own SysTick, and enter the scheduler through its private runqueue.
 */
/* core1 启动栈 (raw launch 实验预留;当前 trampoline launch 未用) */


/* 本函数驻留 SRAM: core1 失效自身 XIP cache (RP2350 每核独立) 前,
 * 从 SRAM 执行失效调用,避免命中刷写后残留的脏缓存行。 */
static void __attribute__((noinline))
__not_in_flash_func(core1_entry)(void) {
    /* 不用 flash 驻留的 hal_irq_disable — 直接内联关中断 */
    __asm volatile("cpsid i" ::: "memory");
    core1_stage = 1;

    {
        extern void xip_cache_invalidate_all(void);
        xip_cache_invalidate_all();
    }

    /* trampoline 遗留的 PSP/CONTROL 未定义: 观测到 SPSEL=1 且 PSP 为
     * 无映射垃圾。首次异常的硬件压栈打到当前 SP → MemManage MSTKERR。
     * 统一切回 MSP (bootrom launch 栈) 并给 PSP 合法值。 */
    __asm volatile(
        "mrs  r0, control      \n"
        "bics r0, r0, #2       \n"  /* SPSEL := 0, 线程态用 MSP */
        "msr  control, r0      \n"
        "isb                   \n"
        "mrs  r0, msp          \n"
        "msr  psp, r0          \n"  /* PSP := 合法栈,防任何遗留 SPSEL 场景 */
        "isb                   \n"
        ::: "r0", "memory", "cc");

    /* Set up core1's exception priorities (per-core SCB SHPR).
     * Same config as core0: SVC=0, PendSV=15, SysTick=15. */
    hal_interrupt_priority_init();

#if MPU_ENABLE
    /* MPU registers are banked per core.  Core0 initialization does not
     * establish protection for tasks which later migrate to core1. */
    mpu_init();
#endif

    /* The launch handshake also uses the FIFO.  Install its steady-state IRQ
     * handler only after the SDK trampoline has consumed that protocol. */
    smp_ipi_init_cpu();
    core1_stage = 2;
    while (ipi_ready[0] == 0U) {
        __asm volatile("wfe");
    }

    /* Mark core1 as "started" — it will pick tasks from runqueue[1]
     * via kern_pendsv_handler. We set _current_task[1] = NULL and trigger
     * a PendSV to select the first task for this core. */
    _current_task[1] = NULL;
    _next_task[1] = NULL;
    sched_set_cpu_online(1U, 1);
    core1_ready = 1;
    core1_stage = 3;
    __asm volatile("dmb" ::: "memory");

    /* Runtime initializers execute before this function and may use VFP even
     * for integer copies.  Discard that non-task FP context and disable lazy
     * stacking on this CPU before its first PendSV switches to a task PSP. */
    hal_fpu_context_init();

    /* Each core has a private SysTick.  Core1 uses it only for its local time
     * slice; sched_tick_handler keeps wall-clock accounting on core0. */
    hal_systick_init(KERNEL_TICK_RATE);

    /* Enable interrupts only after the bootstrap FP context is gone. */
    hal_irq_enable();
    core1_stage = 4;

    /* Trigger the first context switch on core1. PendSV will select a task
     * from the local runqueue. If no task is available, the idle task
     * will run. We use a software-triggered PendSV. */
    hal_trigger_pendsv();

    /* Should never reach here — PendSV switches to a task. */
    while (1) {
        __asm volatile("wfi");
    }
}

kern_err_t smp_init_core1(void) {
    /* Launch core1 via the SDK multicore API. This:
     * 1. Resets core1 (PSM force-off)
     * 2. Sends VTOR + SP + entry over the SIO FIFO
     * 3. Core1 bootrom applies them and jumps to core1_entry
    */
    extern void multicore_launch_core1(void (*entry)(void));
    extern void multicore_reset_core1(void);

    for (uint32_t attempt = 0U; attempt < 3U; attempt++) {
        core1_ready = 0;
        memset((void *)ipi_ready, 0, sizeof(ipi_ready));
        memset((void *)ipi_pending, 0, sizeof(ipi_pending));
        memset((void *)flash_lockout_ack, 0, sizeof(flash_lockout_ack));
        flash_lockout_requested = 0U;
        __asm volatile("dmb" ::: "memory");
        multicore_launch_core1(core1_entry);

        /* multicore_launch_core1() returns after the boot FIFO handshake.
         * The handshake完成 ≠ core1 已进入 core1_entry: 复位路径残留
         * (SIO FIFO 陈旧数据/bootrom 状态) 会让 core1 困在 bootrom,
         * 间歇性发生 (flash 后首次启动、看门狗重启后)。 */
        smp_ipi_init_cpu();

        for (uint32_t spin = 0; spin < 20000000U; spin++) {
            __asm volatile("dmb" ::: "memory");
            if (core1_ready && _current_task[1] != NULL) {
#if KERN_DEBUG_ENABLE
                if (attempt != 0U) {
                    extern void hal_debug_puts(const char *s);
                    hal_debug_puts("[C0] core1 running (retry)\r\n");
                }
#endif
                return KERN_OK;
            }
            __asm volatile("nop");
        }

        /* core1 困死 (bootrom 握手后未达就绪,或短暂上线后被整核复位
         * 回 bootrom — flash-复位工作流下可复现)。core1_entry 到就绪
         * 之间的路径 (向量写/MPU 寄存器/RAM 变量) 不持有任何内核自旋
         * 锁,PSM 硬复位安全: 复位后 bootrom 排空 FIFO 并回报,下一轮
         * attempt 从干净状态重新 launch。 */
#if KERN_DEBUG_ENABLE
        {
            extern void hal_debug_puts(const char *s);
            hal_debug_puts("[C0] core1 relaunch (PSM reset)\r\n");
        }
#endif
        if (attempt + 1U < 3U) {
            multicore_reset_core1();
        }
    }

#if KERN_DEBUG_ENABLE
    {
        extern void hal_debug_puts(const char *s);
        hal_debug_puts("[C0] core1 LAUNCH TIMEOUT\r\n");
    }
#endif
    return KERN_ERR_TIMEOUT;
}

#endif /* SMP */
