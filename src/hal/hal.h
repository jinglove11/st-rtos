#ifndef HAL_H
#define HAL_H

#include <stdint.h>

void hal_cpu_init(void);
void hal_debug_putc(char c);
void hal_debug_puts(const char *s);

void hal_systick_init(uint32_t rate_hz);
uint32_t hal_systick_get(void);
void hal_systick_enable(void);
void hal_systick_disable(void);

void hal_irq_enable(void);
void hal_irq_disable(void);
uint32_t hal_irq_save(void);
void hal_irq_restore(uint32_t primask);

// BasePri-based critical section (preserves high-priority interrupts)
uint32_t hal_enter_critical(void);
void hal_exit_critical(uint32_t basepri);

// Interrupt priority initialization
void hal_interrupt_priority_init(void);

void hal_irq_set_priority(uint32_t irq, uint32_t priority);
void hal_irq_enable_irq(uint32_t irq);
void hal_irq_disable_irq(uint32_t irq);
void hal_irq_clear_pending(uint32_t irq);

void hal_enter_lowpower(void);
void hal_exit_lowpower(void);

void *hal_stack_init(void *stack_top, uint32_t stack_size, void *entry, void *arg, void *exit);

void hal_trigger_pendsv(void);
void hal_trigger_svc(uint32_t svc_num);
void hal_trigger_first_switch(void);

void hal_watchdog_init(void);
void hal_watchdog_feed(void);

uint32_t hal_get_tick_count(void);
void hal_set_tick_count(uint32_t count);

#endif
