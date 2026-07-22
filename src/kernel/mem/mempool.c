/**
 * @file mempool.c
 * @brief 内存池实现 (固定大小块分配，无碎片)
 */

#include "mempool.h"
#include "mem.h"
#include "kernel_config.h"
#include "trace.h"
#include "stats.h"
#include "scheduler.h"
#include "hal.h"
#include "spinlock.h"
#include <string.h>

#ifndef TRACE_MEM_ALLOC
#define TRACE_MEM_ALLOC       1
#define TRACE_MEM_FREE        2
#define TRACE_MEM_FAIL        3
#endif

#ifndef STATS_COUNTER_OK
#define STATS_COUNTER_OK         0
#define STATS_COUNTER_ERROR      1
#define STATS_COUNTER_QUEUE_FULL 2
#define STATS_COUNTER_DELETE     4
#define STATS_COUNTER_NOEXIST    7
#endif

#define POOL_MAX_COUNT      8

typedef struct mem_pool {
    uint8_t *buffer;
    uint8_t *free_list;
    size_t block_size;
    uint32_t block_count;
    uint32_t free_count;
    uint8_t in_use;
} mem_pool_t;

static mem_pool_t pools[POOL_MAX_COUNT];
static uint32_t pool_used_bitmap = 0;
static irq_spinlock_t mempool_lock;

static uint8_t mempool_current_task_id(void) {
    tcb_t *current = sched_get_current();
    return current ? (uint8_t)current->id : 0xFFU;
}

static void mempool_record_event(pool_id_t pool_id, uint8_t action,
                                 kern_err_t err, uint8_t counter) {
#if TRACE_ENABLE
    uint8_t object_id = (pool_id >= 0) ? (uint8_t)pool_id : 0xFFU;
    uint8_t result = (err == KERN_OK) ? TRACE_RESULT_OK :
                     (err == KERN_ERR_NOEXIST ? TRACE_RESULT_NOEXIST :
                      (err == KERN_ERR_RESOURCE ? TRACE_RESULT_FULL :
                       TRACE_RESULT_ERR));
    trace_mem(mempool_current_task_id(), object_id, action, result);
#else
    (void)pool_id;
    (void)action;
#endif

#if KERN_TASK_STATS
    if (err != KERN_OK) {
        if (err == KERN_ERR_NOEXIST) {
            counter = STATS_COUNTER_NOEXIST;
        } else if (err == KERN_ERR_RESOURCE) {
            counter = STATS_COUNTER_QUEUE_FULL;
        } else {
            counter = STATS_COUNTER_ERROR;
        }
    }
    (void)stats_record_event(STATS_SUBSYS_MEM, counter);
#else
    (void)counter;
#endif
    (void)err;
}

static uint32_t crit_enter(void) {
    return irq_spin_lock(&mempool_lock);
}

static void crit_exit(uint32_t primask) {
    irq_spin_unlock(&mempool_lock, primask);
}

static pool_id_t alloc_pool_id(void) {
    for (int i = 0; i < POOL_MAX_COUNT; i++) {
        if (!(pool_used_bitmap & (1U << i))) {
            pool_used_bitmap |= (1U << i);
            return (pool_id_t)i;
        }
    }
    return POOL_INVALID_ID;
}

static void free_pool_id(pool_id_t id) {
    if (id >= 0 && id < POOL_MAX_COUNT) {
        pool_used_bitmap &= ~(1U << id);
    }
}

void mempool_init(void) {
    irq_spin_init_rank(&mempool_lock, LOCKDEP_RANK_REGISTRY);
    memset(pools, 0, sizeof(pools));
    pool_used_bitmap = 0;
}

pool_id_t mempool_create(size_t block_size, uint32_t block_count) {
    if (block_size == 0 || block_count == 0) {
        mempool_record_event(POOL_INVALID_ID, TRACE_MEM_ALLOC, KERN_ERR_PARAM,
                             STATS_COUNTER_ERROR);
        return POOL_INVALID_ID;
    }
    
    block_size = (block_size + 7) & ~7;
    
    uint32_t crit = crit_enter();
    
    pool_id_t id = alloc_pool_id();
    if (id == POOL_INVALID_ID) {
        crit_exit(crit);
        mempool_record_event(POOL_INVALID_ID, TRACE_MEM_FAIL, KERN_ERR_RESOURCE,
                             STATS_COUNTER_QUEUE_FULL);
        return POOL_INVALID_ID;
    }
    
    mem_pool_t *pool = &pools[id];
    
    size_t total_size = block_size * block_count;
    pool->buffer = kmalloc(total_size);
    if (!pool->buffer) {
        free_pool_id(id);
        crit_exit(crit);
        mempool_record_event(id, TRACE_MEM_FAIL, KERN_ERR_RESOURCE,
                             STATS_COUNTER_QUEUE_FULL);
        return POOL_INVALID_ID;
    }
    
    pool->block_size = block_size;
    pool->block_count = block_count;
    pool->free_count = block_count;
    pool->in_use = 1;
    
    for (uint32_t i = 0; i < block_count; i++) {
        uint8_t *block = pool->buffer + i * block_size;
        *(uint8_t **)block = (i < block_count - 1) ? (block + block_size) : NULL;
    }
    pool->free_list = pool->buffer;
    
    crit_exit(crit);
    mempool_record_event(id, TRACE_MEM_ALLOC, KERN_OK, STATS_COUNTER_OK);
    return id;
}

void mempool_delete(pool_id_t pool_id) {
    if (pool_id < 0 || pool_id >= POOL_MAX_COUNT) {
        mempool_record_event(pool_id, TRACE_MEM_FREE, KERN_ERR_PARAM,
                             STATS_COUNTER_ERROR);
        return;
    }
    
    uint32_t crit = crit_enter();
    
    mem_pool_t *pool = &pools[pool_id];
    if (!pool->in_use) {
        crit_exit(crit);
        mempool_record_event(pool_id, TRACE_MEM_FREE, KERN_ERR_NOEXIST,
                             STATS_COUNTER_NOEXIST);
        return;
    }
    
    kfree(pool->buffer);
    memset(pool, 0, sizeof(mem_pool_t));
    free_pool_id(pool_id);
    
    crit_exit(crit);
    mempool_record_event(pool_id, TRACE_MEM_FREE, KERN_OK,
                         STATS_COUNTER_DELETE);
}

void *mempool_alloc(pool_id_t pool_id) {
    if (pool_id < 0 || pool_id >= POOL_MAX_COUNT) {
        mempool_record_event(pool_id, TRACE_MEM_ALLOC, KERN_ERR_PARAM,
                             STATS_COUNTER_ERROR);
        return NULL;
    }
    
    uint32_t crit = crit_enter();
    
    mem_pool_t *pool = &pools[pool_id];
    if (!pool->in_use || !pool->free_list) {
        crit_exit(crit);
        mempool_record_event(pool_id, TRACE_MEM_FAIL,
                             pool->in_use ? KERN_ERR_RESOURCE : KERN_ERR_NOEXIST,
                             pool->in_use ? STATS_COUNTER_QUEUE_FULL :
                                            STATS_COUNTER_NOEXIST);
        return NULL;
    }
    
    void *block = pool->free_list;
    pool->free_list = *(uint8_t **)block;
    pool->free_count--;
    
    crit_exit(crit);
    mempool_record_event(pool_id, TRACE_MEM_ALLOC, KERN_OK, STATS_COUNTER_OK);
    return block;
}

void mempool_free(pool_id_t pool_id, void *block) {
    if (pool_id < 0 || pool_id >= POOL_MAX_COUNT || !block) {
        mempool_record_event(pool_id, TRACE_MEM_FREE, KERN_ERR_PARAM,
                             STATS_COUNTER_ERROR);
        return;
    }
    
    uint32_t crit = crit_enter();
    
    mem_pool_t *pool = &pools[pool_id];
    if (!pool->in_use) {
        crit_exit(crit);
        mempool_record_event(pool_id, TRACE_MEM_FREE, KERN_ERR_NOEXIST,
                             STATS_COUNTER_NOEXIST);
        return;
    }
    
    *(uint8_t **)block = pool->free_list;
    pool->free_list = (uint8_t *)block;
    pool->free_count++;
    
    crit_exit(crit);
    mempool_record_event(pool_id, TRACE_MEM_FREE, KERN_OK, STATS_COUNTER_OK);
}

uint32_t mempool_get_free_count(pool_id_t pool_id) {
    if (pool_id < 0 || pool_id >= POOL_MAX_COUNT) {
        return 0;
    }
    
    uint32_t crit = crit_enter();
    uint32_t count = pools[pool_id].free_count;
    crit_exit(crit);
    
    return count;
}

uint32_t mempool_get_used_count(pool_id_t pool_id) {
    if (pool_id < 0 || pool_id >= POOL_MAX_COUNT) {
        return 0;
    }
    
    uint32_t crit = crit_enter();
    mem_pool_t *pool = &pools[pool_id];
    uint32_t count = pool->block_count - pool->free_count;
    crit_exit(crit);
    
    return count;
}
