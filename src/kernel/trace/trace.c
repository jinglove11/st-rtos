/**
 * @file trace.c
 * @brief 内核事件追踪实现 — 环形 buffer
 */

#include "trace.h"

#if TRACE_ENABLE

#include "scheduler.h"
#include "hal.h"
#include "kernel_config.h"

typedef struct {
    trace_entry_t buf[TRACE_BUFFER_SIZE];
    volatile uint16_t head;
    volatile uint16_t count;
    volatile uint32_t epoch;
} trace_cpu_t;

static trace_cpu_t trace_cpu[SMP_MAX_CPUS];
static volatile uint32_t trace_epoch = 1U;

static uint32_t trace_cpu_id(void) {
    uint32_t cpu = hal_get_cpu_id();
    return (cpu < SMP_MAX_CPUS) ? cpu : 0U;
}

static void trace_put_hex16(uint16_t value) {
    static const char hex[] = "0123456789ABCDEF";
    char buf[5];

    for (int i = 3; i >= 0; i--) {
        buf[i] = hex[value & 0xF];
        value >>= 4;
    }
    buf[4] = '\0';
    hal_debug_puts(buf);
}

static void trace_put_u32(uint32_t value) {
    char buf[10];
    int i = 0;

    if (value == 0) {
        hal_debug_puts("0");
        return;
    }
    while (value > 0 && i < (int)sizeof(buf)) {
        buf[i++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (i > 0) {
        char c[2] = { buf[--i], '\0' };
        hal_debug_puts(c);
    }
}

void trace_record(uint8_t event, uint8_t task_id, uint16_t data) {
    uint32_t irq_state = hal_irq_save();
    trace_cpu_t *state = &trace_cpu[trace_cpu_id()];
    uint32_t epoch = __atomic_load_n(&trace_epoch, __ATOMIC_ACQUIRE);
    if (state->epoch != epoch) {
        state->head = 0U;
        state->count = 0U;
        state->epoch = epoch;
    }

    trace_entry_t *e = &state->buf[state->head];
    e->tick     = sched_get_tick_count();
    e->event    = event;
    e->task_id  = task_id;
    e->data     = data;

    state->head = (uint16_t)((state->head + 1U) % TRACE_BUFFER_SIZE);
    if (state->count < TRACE_BUFFER_SIZE) {
        state->count++;
    }
    hal_irq_restore(irq_state);
}

uint16_t trace_pack(uint8_t object_id, uint8_t result) {
    return (uint16_t)(((uint16_t)object_id << 8) | result);
}

void trace_timer(uint8_t task_id, uint8_t timer_id, uint8_t action, uint8_t result) {
    trace_record(TRACE_TIMER, task_id, trace_pack(timer_id, (uint8_t)((action << 4) | (result & 0x0F))));
}

void trace_irq(uint8_t task_id, uint8_t irq_num, uint8_t action, uint8_t result) {
    trace_record(TRACE_IRQ, task_id, trace_pack(irq_num, (uint8_t)((action << 4) | (result & 0x0F))));
}

void trace_bh(uint8_t task_id, uint8_t bh_id, uint8_t action, uint8_t result) {
    trace_record(TRACE_BH, task_id, trace_pack(bh_id, (uint8_t)((action << 4) | (result & 0x0F))));
}

void trace_dev(uint8_t task_id, uint8_t dev_id, uint8_t action, uint8_t result) {
    trace_record(TRACE_DEV, task_id, trace_pack(dev_id, (uint8_t)((action << 4) | (result & 0x0F))));
}

void trace_mem(uint8_t task_id, uint8_t mem_id, uint8_t action, uint8_t result) {
    trace_record(TRACE_MEM, task_id, trace_pack(mem_id, (uint8_t)((action << 4) | (result & 0x0F))));
}

void trace_ipc_event(uint8_t task_id, uint8_t ipc_id, uint8_t action, uint8_t result) {
    trace_record(TRACE_IPC_EVENT, task_id, trace_pack(ipc_id, (uint8_t)((action << 4) | (result & 0x0F))));
}

uint16_t trace_get_count(void) {
    uint32_t epoch = __atomic_load_n(&trace_epoch, __ATOMIC_ACQUIRE);
    uint32_t total = 0U;
    for (uint32_t cpu = 0; cpu < SMP_MAX_CPUS; cpu++) {
        if (trace_cpu[cpu].epoch == epoch) {
            total += trace_cpu[cpu].count;
        }
    }
    return (total > UINT16_MAX) ? UINT16_MAX : (uint16_t)total;
}

uint16_t trace_get_local_count(void) {
    uint32_t epoch = __atomic_load_n(&trace_epoch, __ATOMIC_ACQUIRE);
    trace_cpu_t *state = &trace_cpu[trace_cpu_id()];
    return (state->epoch == epoch) ? state->count : 0U;
}

const trace_entry_t *trace_get_entry(uint16_t index) {
    uint32_t epoch = __atomic_load_n(&trace_epoch, __ATOMIC_ACQUIRE);
    for (uint32_t cpu = 0; cpu < SMP_MAX_CPUS; cpu++) {
        trace_cpu_t *state = &trace_cpu[cpu];
        if (state->epoch != epoch) {
            continue;
        }
        uint16_t count = state->count;
        if (index >= count) {
            index = (uint16_t)(index - count);
            continue;
        }

        uint16_t oldest = (count < TRACE_BUFFER_SIZE) ? 0U : state->head;
        uint16_t pos = (uint16_t)((oldest + index) % TRACE_BUFFER_SIZE);
        return &state->buf[pos];
    }
    return NULL;
}

const trace_entry_t *trace_get_local_entry(uint16_t index) {
    uint32_t epoch = __atomic_load_n(&trace_epoch, __ATOMIC_ACQUIRE);
    trace_cpu_t *state = &trace_cpu[trace_cpu_id()];
    if (state->epoch != epoch || index >= state->count) {
        return NULL;
    }

    uint16_t oldest = (state->count < TRACE_BUFFER_SIZE) ? 0U : state->head;
    return &state->buf[(oldest + index) % TRACE_BUFFER_SIZE];
}

void trace_clear(void) {
    uint32_t epoch = __atomic_add_fetch(&trace_epoch, 1U, __ATOMIC_ACQ_REL);
    uint32_t irq_state = hal_irq_save();
    trace_cpu_t *state = &trace_cpu[trace_cpu_id()];
    state->head = 0U;
    state->count = 0U;
    state->epoch = epoch;
    hal_irq_restore(irq_state);
}

uint16_t trace_filter(uint8_t event,
                      void (*callback)(const trace_entry_t *e, void *ctx),
                      void *ctx) {
    uint16_t matched = 0;
    uint16_t count = trace_get_count();
    for (uint16_t i = 0; i < count; i++) {
        const trace_entry_t *e = trace_get_entry(i);
        if (e && e->event == event) {
            callback(e, ctx);
            matched++;
        }
    }
    return matched;
}

uint16_t trace_filter_local(uint8_t event,
                            void (*callback)(const trace_entry_t *e, void *ctx),
                            void *ctx) {
    uint16_t matched = 0;
    uint16_t count = trace_get_local_count();
    for (uint16_t i = 0; i < count; i++) {
        const trace_entry_t *e = trace_get_local_entry(i);
        if (e && e->event == event) {
            callback(e, ctx);
            matched++;
        }
    }
    return matched;
}

void trace_dump_recent(uint16_t max_entries) {
    uint16_t count = trace_get_count();
    uint16_t start = 0;

    if (max_entries != 0 && count > max_entries) {
        start = count - max_entries;
    }

    hal_debug_puts("Recent trace:\r\n");
    for (uint16_t i = start; i < count; i++) {
        const trace_entry_t *e = trace_get_entry(i);
        if (e == NULL) {
            continue;
        }

        hal_debug_puts("  t=");
        trace_put_u32(e->tick);
        hal_debug_puts(" ev=");
        trace_put_u32(e->event);
        hal_debug_puts(" task=");
        trace_put_u32(e->task_id);
        hal_debug_puts(" data=0x");
        trace_put_hex16(e->data);
        hal_debug_puts("\r\n");
    }
}

#endif /* TRACE_ENABLE */
