/**
 * @file wait_queue.c
 * @brief 公共等待队列实现
 */

#include "wait_queue.h"
#include "kernel_config.h"
#include "hal.h"

#if KERN_DEBUG_ENABLE
extern void kern_panic(const char *msg);

static void wait_queue_check(const wait_queue_t *wq, const char *where) {
    if (!wait_queue_validate(wq)) {
        hal_debug_puts("\r\n[WAITQ] invariant failed at ");
        hal_debug_puts(where);
        hal_debug_puts("\r\n");
        kern_panic("wait queue invariant");
    }
}
#else
#define wait_queue_check(wq, where) do { (void)(wq); } while (0)
#endif

void wait_queue_init(wait_queue_t *wq) {
    wq->head = NULL;
    wq->tail = NULL;
    wq->count = 0;
}

void wait_queue_add(wait_queue_t *wq, tcb_t *tcb) {
    tcb->wait_next = NULL;
    tcb->wait_prev = wq->tail;

    if (wq->tail) {
        wq->tail->wait_next = tcb;
    } else {
        wq->head = tcb;
    }
    wq->tail = tcb;
    wq->count++;
    wait_queue_check(wq, "add");
}

void wait_queue_remove(wait_queue_t *wq, tcb_t *tcb) {
    if (!wait_queue_contains(wq, tcb)) {
#if KERN_DEBUG_ENABLE
        hal_debug_puts("\r\n[WAITQ] remove ignored: task not in queue\r\n");
#endif
        return;
    }

    if (tcb->wait_prev) {
        tcb->wait_prev->wait_next = tcb->wait_next;
    } else {
        wq->head = tcb->wait_next;
    }

    if (tcb->wait_next) {
        tcb->wait_next->wait_prev = tcb->wait_prev;
    } else {
        wq->tail = tcb->wait_prev;
    }

    tcb->wait_next = NULL;
    tcb->wait_prev = NULL;
    wq->count--;
    wait_queue_check(wq, "remove");
}

int wait_queue_contains(const wait_queue_t *wq, const tcb_t *tcb) {
    const tcb_t *curr = wq ? wq->head : NULL;

    while (curr) {
        if (curr == tcb) {
            return 1;
        }
        curr = curr->wait_next;
    }

    return 0;
}

int wait_queue_remove_safe(wait_queue_t *wq, tcb_t *tcb) {
    uint16_t before = wq ? wq->count : 0;

    wait_queue_remove(wq, tcb);
    if (wq == NULL || before == wq->count) {
        return 0;
    }

    return 1;
}

int wait_queue_validate(const wait_queue_t *wq) {
    if (wq == NULL) {
        return 0;
    }

    if ((wq->head == NULL) != (wq->tail == NULL)) {
        return 0;
    }

    if (wq->head && wq->head->wait_prev != NULL) {
        return 0;
    }

    if (wq->tail && wq->tail->wait_next != NULL) {
        return 0;
    }

    uint16_t count = 0;
    const tcb_t *prev = NULL;
    const tcb_t *curr = wq->head;

    while (curr) {
        if (curr->wait_prev != prev) {
            return 0;
        }

        prev = curr;
        curr = curr->wait_next;
        count++;

        if (count > KERNEL_MAX_TASKS) {
            return 0;
        }
    }

    if (prev != wq->tail) {
        return 0;
    }

    return count == wq->count;
}

tcb_t *wait_queue_get_highest(wait_queue_t *wq) {
    if (wq->head == NULL) {
        return NULL;
    }

    tcb_t *highest = wq->head;
    tcb_t *curr = wq->head->wait_next;

    while (curr) {
        if (curr->priority < highest->priority) {
            highest = curr;
        }
        curr = curr->wait_next;
    }

    return highest;
}

uint16_t wait_queue_count(const wait_queue_t *wq) {
    return wq->count;
}
