/**
 * @file wait_queue.h
 * @brief 公共等待队列 — 所有 IPC 原语共享
 */

#ifndef WAIT_QUEUE_H
#define WAIT_QUEUE_H

#include "kernel_types.h"

void     wait_queue_init(wait_queue_t *wq);
void     wait_queue_add(wait_queue_t *wq, tcb_t *tcb);
void     wait_queue_remove(wait_queue_t *wq, tcb_t *tcb);
int      wait_queue_contains(const wait_queue_t *wq, const tcb_t *tcb);
int      wait_queue_remove_safe(wait_queue_t *wq, tcb_t *tcb);
int      wait_queue_validate(const wait_queue_t *wq);
tcb_t   *wait_queue_get_highest(wait_queue_t *wq);
uint16_t wait_queue_count(const wait_queue_t *wq);

#endif /* WAIT_QUEUE_H */
