/**
 * @file trace.c
 * @brief 内核事件追踪实现 — 环形 buffer
 */

#include "trace.h"

#if TRACE_ENABLE

#include "scheduler.h"
#include "hal.h"

#ifndef TRACE_BUFFER_SIZE
#define TRACE_BUFFER_SIZE 256
#endif

static trace_entry_t trace_buf[TRACE_BUFFER_SIZE];
static uint16_t trace_head;
static uint16_t trace_count;

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
    trace_entry_t *e = &trace_buf[trace_head];
    e->tick     = sched_get_tick_count();
    e->event    = event;
    e->task_id  = task_id;
    e->data     = data;

    trace_head = (trace_head + 1) % TRACE_BUFFER_SIZE;
    if (trace_count < TRACE_BUFFER_SIZE) {
        trace_count++;
    }
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
    return trace_count;
}

const trace_entry_t *trace_get_entry(uint16_t index) {
    if (index >= trace_count) return NULL;

    /* 计算实际位置: 最旧的条目在 (head - count) 处 */
    uint16_t pos;
    if (trace_count < TRACE_BUFFER_SIZE) {
        pos = index;
    } else {
        pos = (trace_head + index) % TRACE_BUFFER_SIZE;
    }
    return &trace_buf[pos];
}

void trace_clear(void) {
    trace_head  = 0;
    trace_count = 0;
}

uint16_t trace_filter(uint8_t event,
                      void (*callback)(const trace_entry_t *e, void *ctx),
                      void *ctx) {
    uint16_t matched = 0;
    for (uint16_t i = 0; i < trace_count; i++) {
        const trace_entry_t *e = trace_get_entry(i);
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
