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

#define SMP_FIFO_KICK 0x4D315049UL /* "M1PI" */

/* This loop must remain executable while QSPI XIP is disabled by a flash
 * erase/program operation.  The FIFO handler enters it before acknowledging
 * the requesting CPU, so the requester cannot disable XIP too early. */
static void __attribute__((noinline))
__not_in_flash_func(smp_flash_lockout_park)(uint32_t cpu) {
    uint32_t primask;
    __asm volatile("mrs %0, primask" : "=r"(primask));
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

static void smp_fifo_irq_handler(void) {
    uint32_t cpu = hal_get_cpu_id();

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
        smp_flash_lockout_park(cpu);
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
static void core1_entry(void) {
    /* Core1 must not accept SysTick/FIFO work until all banked architectural
     * state and the scheduler ownership handshake are ready. */
    hal_irq_disable();

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
    core1_ready = 0;
    memset((void *)ipi_ready, 0, sizeof(ipi_ready));
    memset((void *)ipi_pending, 0, sizeof(ipi_pending));
    memset((void *)flash_lockout_ack, 0, sizeof(flash_lockout_ack));
    flash_lockout_requested = 0U;
    __asm volatile("dmb" ::: "memory");
    extern void multicore_launch_core1(void (*entry)(void));
    multicore_launch_core1(core1_entry);

    /* multicore_launch_core1() returns after the boot FIFO handshake. */
    smp_ipi_init_cpu();

    for (uint32_t spin = 0; spin < 1000000U; spin++) {
        __asm volatile("dmb" ::: "memory");
        if (core1_ready && _current_task[1] != NULL) {
            return KERN_OK;
        }
        __asm volatile("nop");
    }

    return KERN_ERR_TIMEOUT;
}

#endif /* SMP */
