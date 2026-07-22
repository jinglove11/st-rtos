/**
 * @file trace.h
 * @brief 内核事件追踪 — 环形 buffer + record API
 */

#ifndef TRACE_H
#define TRACE_H

#include "kernel_config.h"

#if TRACE_ENABLE

#include <stdint.h>

/*============================================================================
 * 事件类型
 *
 * data 字段含义 (per event):
 *   TASK_SWITCH:  0
 *   ISR_ENTER:    irq_num
 *   ISR_EXIT:     irq_num
 *   SYSCALL:      syscall_num
 *   IPC_SEND:     endpoint/channel id
 *   IPC_RECV:     endpoint/channel id
 *   BH_SCHEDULE:  bh_id
 *   FAULT:        fault_type (0=Hard, 1=MemManage, 2=Bus, 3=Usage)
 *   *_P2 events:  high byte = object id, low byte = result/sub-event
 *============================================================================*/

#define TRACE_TASK_SWITCH   0
#define TRACE_ISR_ENTER     1
#define TRACE_ISR_EXIT      2
#define TRACE_SYSCALL       3
#define TRACE_IPC_SEND      4
#define TRACE_IPC_RECV      5
#define TRACE_BH_SCHEDULE   6
#define TRACE_FAULT         7
#define TRACE_TIMER         8
#define TRACE_IRQ           9
#define TRACE_BH            10
#define TRACE_DEV           11
#define TRACE_MEM           12
#define TRACE_IPC_EVENT     13
#define TRACE_CAP_EVENT     14
#define TRACE_VFS_EVENT     15

#define TRACE_RESULT_OK       0
#define TRACE_RESULT_ERR      1
#define TRACE_RESULT_FULL     2
#define TRACE_RESULT_TIMEOUT  3
#define TRACE_RESULT_DELETE   4
#define TRACE_RESULT_CANCEL   5
#define TRACE_RESULT_BUSY     6
#define TRACE_RESULT_NOEXIST  7

#define TRACE_TIMER_CREATE    1
#define TRACE_TIMER_START     2
#define TRACE_TIMER_STOP      3
#define TRACE_TIMER_FIRE      4
#define TRACE_TIMER_DELETE    5
#define TRACE_TIMER_QUEUE_FULL 6
#define TRACE_TIMER_RESET     7
#define TRACE_TIMER_CHANGE    8

#define TRACE_IRQ_REGISTER    1
#define TRACE_IRQ_FIRE        2
#define TRACE_IRQ_RELEASE     3
#define TRACE_IRQ_SPURIOUS    4
#define TRACE_IRQ_MASK        5
#define TRACE_IRQ_UNMASK      6

#define TRACE_BH_CREATE       1
#define TRACE_BH_SCHED        2
#define TRACE_BH_RUN          3
#define TRACE_BH_CANCEL       4
#define TRACE_BH_DELETE       5

#define TRACE_DEV_OPEN        1
#define TRACE_DEV_READ        2
#define TRACE_DEV_WRITE       3
#define TRACE_DEV_IOCTL       4
#define TRACE_DEV_REMOVE      5

#define TRACE_MEM_ALLOC       1
#define TRACE_MEM_FREE        2
#define TRACE_MEM_FAIL        3

#define TRACE_IPC_TIMEOUT     1
#define TRACE_IPC_DELETE      2
#define TRACE_IPC_DEATH       3
#define TRACE_IPC_CAP_FAIL    4

/*============================================================================
 * Trace 条目
 *============================================================================*/

typedef struct {
    uint32_t    tick;
    uint8_t     event;
    uint8_t     task_id;
    uint16_t    data;
} trace_entry_t;

/*============================================================================
 * API
 *============================================================================*/

/**
 * @brief 记录一条 trace 事件
 * @param event   事件类型 (TRACE_TASK_SWITCH 等)
 * @param task_id 相关任务 ID
 * @param data    附加数据 (如 syscall 号)
 */
void trace_record(uint8_t event, uint8_t task_id, uint16_t data);

uint16_t trace_pack(uint8_t object_id, uint8_t result);
void trace_timer(uint8_t task_id, uint8_t timer_id, uint8_t action, uint8_t result);
void trace_irq(uint8_t task_id, uint8_t irq_num, uint8_t action, uint8_t result);
void trace_bh(uint8_t task_id, uint8_t bh_id, uint8_t action, uint8_t result);
void trace_dev(uint8_t task_id, uint8_t dev_id, uint8_t action, uint8_t result);
void trace_mem(uint8_t task_id, uint8_t mem_id, uint8_t action, uint8_t result);
void trace_ipc_event(uint8_t task_id, uint8_t ipc_id, uint8_t action, uint8_t result);

/**
 * @brief 获取 trace buffer 条目数
 */
uint16_t trace_get_count(void);

/** Per-CPU diagnostic/test view; avoids unrelated events from the peer CPU. */
uint16_t trace_get_local_count(void);

/**
 * @brief 获取指定索引的 trace 条目
 * @param index 索引 (0 = 最旧)
 * @return 条目指针，越界返回 NULL
 */
const trace_entry_t *trace_get_entry(uint16_t index);
const trace_entry_t *trace_get_local_entry(uint16_t index);

/**
 * @brief 清空 trace buffer
 */
void trace_clear(void);

/**
 * @brief 按事件类型遍历 trace buffer
 * @param event    事件类型过滤器 (0-7)
 * @param callback 回调函数 (每匹配一条调用一次)
 * @param ctx      回调上下文 (透传)
 * @return 匹配的条目数
 */
uint16_t trace_filter(uint8_t event,
                      void (*callback)(const trace_entry_t *e, void *ctx),
                      void *ctx);
uint16_t trace_filter_local(uint8_t event,
                            void (*callback)(const trace_entry_t *e, void *ctx),
                            void *ctx);

/**
 * @brief 输出最近的 trace 事件，用于 panic/fault 诊断
 * @param max_entries 最多输出条目数，0 表示输出当前全部缓冲
 */
void trace_dump_recent(uint16_t max_entries);

#endif /* TRACE_ENABLE */
#endif /* TRACE_H */
