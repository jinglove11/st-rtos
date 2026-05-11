/**
 * @file mempool.c
 * @brief 内存池实现 (固定大小块分配，无碎片)
 */

#include "mempool.h"
#include "mem.h"
#include "kernel_config.h"
#include "hal.h"
#include <string.h>

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

static uint32_t crit_enter(void) {
    return hal_irq_save();
}

static void crit_exit(uint32_t primask) {
    hal_irq_restore(primask);
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
    memset(pools, 0, sizeof(pools));
    pool_used_bitmap = 0;
}

pool_id_t mempool_create(size_t block_size, uint32_t block_count) {
    if (block_size == 0 || block_count == 0) {
        return POOL_INVALID_ID;
    }
    
    block_size = (block_size + 7) & ~7;
    
    uint32_t crit = crit_enter();
    
    pool_id_t id = alloc_pool_id();
    if (id == POOL_INVALID_ID) {
        crit_exit(crit);
        return POOL_INVALID_ID;
    }
    
    mem_pool_t *pool = &pools[id];
    
    size_t total_size = block_size * block_count;
    pool->buffer = kmalloc(total_size);
    if (!pool->buffer) {
        free_pool_id(id);
        crit_exit(crit);
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
    return id;
}

void mempool_delete(pool_id_t pool_id) {
    if (pool_id < 0 || pool_id >= POOL_MAX_COUNT) {
        return;
    }
    
    uint32_t crit = crit_enter();
    
    mem_pool_t *pool = &pools[pool_id];
    if (!pool->in_use) {
        crit_exit(crit);
        return;
    }
    
    kfree(pool->buffer);
    memset(pool, 0, sizeof(mem_pool_t));
    free_pool_id(pool_id);
    
    crit_exit(crit);
}

void *mempool_alloc(pool_id_t pool_id) {
    if (pool_id < 0 || pool_id >= POOL_MAX_COUNT) {
        return NULL;
    }
    
    uint32_t crit = crit_enter();
    
    mem_pool_t *pool = &pools[pool_id];
    if (!pool->in_use || !pool->free_list) {
        crit_exit(crit);
        return NULL;
    }
    
    void *block = pool->free_list;
    pool->free_list = *(uint8_t **)block;
    pool->free_count--;
    
    crit_exit(crit);
    return block;
}

void mempool_free(pool_id_t pool_id, void *block) {
    if (pool_id < 0 || pool_id >= POOL_MAX_COUNT || !block) {
        return;
    }
    
    uint32_t crit = crit_enter();
    
    mem_pool_t *pool = &pools[pool_id];
    if (!pool->in_use) {
        crit_exit(crit);
        return;
    }
    
    *(uint8_t **)block = pool->free_list;
    pool->free_list = (uint8_t *)block;
    pool->free_count++;
    
    crit_exit(crit);
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
