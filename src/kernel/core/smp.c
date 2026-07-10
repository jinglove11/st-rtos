/**
 * @file smp.c
 * @brief Core completion #7 S3 — SMP core1 launch + entry
 *
 * Launches the RP2350's second Cortex-M33 core via the SDK multicore API.
 * Core1 gets its own SysTick, its own PendSV/SVC (per-core hardware), and
 * shares the ready_list + task pool with core0 via the scheduler spinlock.
 */

#include "kernel_config.h"

#if SMP

#include "smp.h"
#include "hal.h"
#include "scheduler.h"
#include "task.h"
#include "spinlock.h"
#include <string.h>

static volatile uint32_t core1_ready;

/* The SDK's multicore launch provides a stack for core1 (core1_stack in
 * pico_multicore). We just need an entry function. */

/**
 * core1_entry — the C entry point for core1 after the SDK trampoline.
 *
 * Core1 inherits the same vector table (VTOR) as core0 (the SDK passes it).
 * It needs to: set up its own exception priorities (per-core SCB), enable
 * its own SysTick, and enter the scheduler's idle loop (picking tasks from
 * the shared ready_list).
 */
static void core1_entry(void) {
    /* Set up core1's exception priorities (per-core SCB SHPR).
     * Same config as core0: SVC=0, PendSV=15, SysTick=15. */
    hal_interrupt_priority_init();

    /* Enable core1's SysTick at the same rate as core0. Each core's SysTick
     * is independent hardware (banked at 0xE000E010). */
    hal_systick_init(KERNEL_TICK_RATE);

    /* Mark core1 as "started" — it will pick tasks from the shared ready_list
     * via kern_pendsv_handler. We set _current_task[1] = NULL and trigger
     * a PendSV to select the first task for this core. */
    _current_task[1] = NULL;
    _next_task[1] = NULL;
    core1_ready = 1;
    __asm volatile("dmb" ::: "memory");

    /* Enable interrupts on core1. */
    hal_irq_enable();

    /* Trigger the first context switch on core1. PendSV will select a task
     * from the shared ready_list. If no task is available, the idle task
     * will run. We use a software-triggered PendSV. */
    hal_trigger_pendsv();

    /* Should never reach here — PendSV switches to a task. */
    while (1) {
        __asm volatile("wfi");
    }
}

void smp_init_core1(void) {
    /* Launch core1 via the SDK multicore API. This:
     * 1. Resets core1 (PSM force-off)
     * 2. Sends VTOR + SP + entry over the SIO FIFO
     * 3. Core1 bootrom applies them and jumps to core1_entry
    */
    extern void multicore_launch_core1(void (*entry)(void));
    core1_ready = 0;
    __asm volatile("dmb" ::: "memory");
    multicore_launch_core1(core1_entry);

    for (uint32_t spin = 0; spin < 1000000U; spin++) {
        __asm volatile("dmb" ::: "memory");
        if (core1_ready && _current_task[1] != NULL) {
            break;
        }
        __asm volatile("nop");
    }
}

#endif /* SMP */
