/**
 * @file mem.c
 * @brief 动态内存管理实现
 * 
 * 使用首次适应算法
 * 空闲块链表按地址排序
 */

#include "mem.h"
#include "kernel_config.h"
#include "trace.h"
#include "stats.h"
#include "scheduler.h"
#include "capability.h"
#include "hal.h"
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
#endif

#define MEM_ALIGN_SIZE      8
#define MEM_ALIGN_MASK      (MEM_ALIGN_SIZE - 1)
#define MEM_ALIGN_UP(x)     (((x) + MEM_ALIGN_MASK) & ~MEM_ALIGN_MASK)
#define MEM_ALIGN_DOWN(x,a) ((x) & ~((a) - 1))

#define MEM_BLOCK_MAGIC     0x4D454D42
#define MEM_BLOCK_FREE      0x01
#define MEM_BLOCK_USED      0x00

typedef struct mem_block {
    uint32_t magic;
    uint32_t flags;
    size_t size;
    struct mem_block *next;
    struct mem_block *prev;
} mem_block_t;

#define BLOCK_HEADER_SIZE   sizeof(mem_block_t)
#define BLOCK_MIN_SIZE      (MEM_ALIGN_UP(BLOCK_HEADER_SIZE) + MEM_ALIGN_SIZE)

#if MEM_DYNAMIC
static uint8_t mem_heap[MEM_HEAP_SIZE] __attribute__((aligned(MEM_ALIGN_SIZE)));
#endif

static mem_block_t *free_list = NULL;
static mem_stats_t mem_stats;

static uint8_t mem_current_task_id(void) {
    tcb_t *current = sched_get_current();
    return current ? (uint8_t)current->id : 0xFFU;
}

static uint8_t mem_object_id(const void *ptr) {
    return (uint8_t)(((uintptr_t)ptr >> 3) & 0xFFU);
}

static void mem_record_event(const void *ptr, uint8_t action,
                             kern_err_t err, uint8_t counter) {
#if TRACE_ENABLE
    uint8_t object_id = ptr ? mem_object_id(ptr) : 0xFFU;
    uint8_t result = (err == KERN_OK) ? TRACE_RESULT_OK :
                     (err == KERN_ERR_RESOURCE ? TRACE_RESULT_FULL :
                      TRACE_RESULT_ERR);
    trace_mem(mem_current_task_id(), object_id, action, result);
#else
    (void)ptr;
    (void)action;
#endif

#if KERN_TASK_STATS
    if (err != KERN_OK) {
        counter = (err == KERN_ERR_RESOURCE) ? STATS_COUNTER_QUEUE_FULL :
                                               STATS_COUNTER_ERROR;
    }
    (void)stats_record_event(STATS_SUBSYS_MEM, counter);
#else
    (void)counter;
#endif
    (void)err;
}

static uint32_t crit_enter(void) {
    return hal_irq_save();
}

static void crit_exit(uint32_t primask) {
    hal_irq_restore(primask);
}

static void block_init(mem_block_t *block, size_t size, uint32_t flags) {
    block->magic = MEM_BLOCK_MAGIC;
    block->flags = flags;
    block->size = size;
    block->next = NULL;
    block->prev = NULL;
}

static int block_is_valid(mem_block_t *block) {
    return block && block->magic == MEM_BLOCK_MAGIC;
}

static void *block_to_ptr(mem_block_t *block) {
    return (void *)((uint8_t *)block + BLOCK_HEADER_SIZE);
}

static mem_block_t *ptr_to_block(void *ptr) {
    return (mem_block_t *)((uint8_t *)ptr - BLOCK_HEADER_SIZE);
}

static size_t block_total_size(mem_block_t *block) {
    return block->size + BLOCK_HEADER_SIZE;
}

static void free_list_insert(mem_block_t *block) {
    if (free_list == NULL) {
        free_list = block;
        block->next = NULL;
        block->prev = NULL;
        return;
    }

    if (block < free_list) {
        block->next = free_list;
        block->prev = NULL;
        free_list->prev = block;
        free_list = block;
        return;
    }

    mem_block_t *curr = free_list;
    while (curr->next && curr->next < block) {
        curr = curr->next;
    }

    block->next = curr->next;
    block->prev = curr;
    if (curr->next) {
        curr->next->prev = block;
    }
    curr->next = block;
}

static void free_list_remove(mem_block_t *block) {
    if (block->prev) {
        block->prev->next = block->next;
    } else {
        free_list = block->next;
    }

    if (block->next) {
        block->next->prev = block->prev;
    }

    block->next = NULL;
    block->prev = NULL;
}

static void try_merge_with_next(mem_block_t *block) {
    if (!block->next) return;

    uint8_t *block_end = (uint8_t *)block + block_total_size(block);
    
    if (block_end == (uint8_t *)block->next) {
        mem_block_t *next = block->next;
        
        free_list_remove(next);
        
        block->size += block_total_size(next);
    }
}

void mem_init(void) {
#if MEM_DYNAMIC
    size_t heap_size = MEM_HEAP_SIZE;

    heap_size = MEM_ALIGN_DOWN(heap_size, MEM_ALIGN_SIZE);

    mem_block_t *initial = (mem_block_t *)mem_heap;
    block_init(initial, heap_size - BLOCK_HEADER_SIZE, MEM_BLOCK_FREE);

    free_list = initial;

    memset(&mem_stats, 0, sizeof(mem_stats));
    mem_stats.total_size = heap_size;
    mem_stats.free_size = heap_size - BLOCK_HEADER_SIZE;
#else
    free_list = NULL;
    memset(&mem_stats, 0, sizeof(mem_stats));
#endif
}

void *kmalloc(size_t size) {
    if (size == 0) {
        mem_record_event(NULL, TRACE_MEM_ALLOC, KERN_ERR_PARAM,
                         STATS_COUNTER_ERROR);
        return NULL;
    }
    
    size = MEM_ALIGN_UP(size);
    if (size < MEM_ALIGN_SIZE) {
        size = MEM_ALIGN_SIZE;
    }
    
    uint32_t crit = crit_enter();
    
    mem_block_t *block = free_list;
    while (block) {
        if (block->size >= size) {
            break;
        }
        block = block->next;
    }
    
    if (!block) {
        mem_stats.fail_count++;
        crit_exit(crit);
        mem_record_event(NULL, TRACE_MEM_FAIL, KERN_ERR_RESOURCE,
                         STATS_COUNTER_QUEUE_FULL);
        return NULL;
    }
    
    free_list_remove(block);
    
    size_t remaining = block->size - size;
    
    if (remaining >= BLOCK_MIN_SIZE) {
        mem_block_t *new_block = (mem_block_t *)((uint8_t *)block + BLOCK_HEADER_SIZE + size);
        block_init(new_block, remaining - BLOCK_HEADER_SIZE, MEM_BLOCK_FREE);
        free_list_insert(new_block);
        
        block->size = size;
        mem_stats.free_size -= BLOCK_HEADER_SIZE;
    }
    
    block->flags = MEM_BLOCK_USED;
    
    mem_stats.used_size += block->size;
    mem_stats.free_size -= block->size;
    mem_stats.alloc_count++;
    mem_stats.outstanding_allocs++;
    
    if (mem_stats.used_size > mem_stats.max_used) {
        mem_stats.max_used = mem_stats.used_size;
    }
    
    crit_exit(crit);
    
    void *ptr = block_to_ptr(block);
    mem_record_event(ptr, TRACE_MEM_ALLOC, KERN_OK, STATS_COUNTER_OK);
    return ptr;
}

void kfree(void *ptr) {
    if (!ptr) return;
    
    uint32_t crit = crit_enter();
    
    mem_block_t *block = ptr_to_block(ptr);
    
    if (!block_is_valid(block) || block->flags == MEM_BLOCK_FREE) {
        mem_stats.invalid_free_count++;
        crit_exit(crit);
        mem_record_event(ptr, TRACE_MEM_FREE, KERN_ERR_PARAM,
                         STATS_COUNTER_ERROR);
        return;
    }
    
    block->flags = MEM_BLOCK_FREE;
    
    mem_stats.used_size -= block->size;
    mem_stats.free_size += block->size;
    mem_stats.free_count++;
    if (mem_stats.outstanding_allocs > 0) {
        mem_stats.outstanding_allocs--;
    }
    
    free_list_insert(block);
    
    try_merge_with_next(block);
    
    if (block->prev && block->prev->flags == MEM_BLOCK_FREE) {
        mem_block_t *prev = block->prev;
        try_merge_with_next(prev);
    }
    
    crit_exit(crit);
    mem_record_event(ptr, TRACE_MEM_FREE, KERN_OK, STATS_COUNTER_OK);
}

void *krealloc(void *ptr, size_t size) {
    if (ptr == NULL) {
        return kmalloc(size);
    }
    
    if (size == 0) {
        kfree(ptr);
        return NULL;
    }
    
    uint32_t crit = crit_enter();
    
    mem_block_t *block = ptr_to_block(ptr);
    
    if (!block_is_valid(block)) {
        crit_exit(crit);
        return NULL;
    }
    
    size_t old_size = block->size;
    size_t new_size = MEM_ALIGN_UP(size);
    
    if (new_size <= old_size) {
        crit_exit(crit);
        return ptr;
    }
    
    crit_exit(crit);
    
    void *new_ptr = kmalloc(size);
    if (!new_ptr) {
        return NULL;
    }
    
    memcpy(new_ptr, ptr, old_size);
    kfree(ptr);
    
    return new_ptr;
}

void *kmalloc_aligned(size_t size, size_t align) {
    if (size == 0) return NULL;
    if (align == 0 || (align & (align - 1)) != 0) return NULL;
    
    size_t total = size + align + sizeof(void *);
    void *ptr = kmalloc(total);
    if (!ptr) return NULL;
    
    uintptr_t addr = (uintptr_t)ptr + sizeof(void *);
    uintptr_t aligned = (addr + align - 1) & ~(align - 1);
    
    void **stored = (void **)(aligned - sizeof(void *));
    *stored = ptr;
    
    return (void *)aligned;
}

void kfree_aligned(void *ptr) {
    if (!ptr) return;
    
    void **stored = (void **)((uintptr_t)ptr - sizeof(void *));
    kfree(*stored);
}

mem_stats_t mem_get_stats(void) {
    return mem_stats;
}

size_t mem_get_free(void) {
    return mem_stats.free_size;
}

size_t mem_get_used(void) {
    return mem_stats.used_size;
}

uint32_t mem_get_outstanding_allocs(void) {
    return mem_stats.outstanding_allocs;
}

uint32_t mem_get_fail_count(void) {
    return mem_stats.fail_count;
}

#if CAP_ENABLE

static void kmem_cap_cleanup(void *object, uint8_t obj_type) {
    if (obj_type == CAP_OBJ_MEMBLOCK) {
        kfree(object);
    }
}

cap_id_t kmem_alloc_cap(size_t size, uint8_t rights) {
    if (rights == 0) {
        rights = CAP_READ | CAP_WRITE | CAP_MANAGE;
    }

    void *ptr = kmalloc(size);
    if (!ptr) {
        return KERN_INVALID_ID;
    }

    (void)cap_register_cleanup(CAP_OBJ_MEMBLOCK, kmem_cap_cleanup);
    cap_id_t cap = cap_create(ptr, CAP_OBJ_MEMBLOCK, rights, 0);
    if (cap == KERN_INVALID_ID) {
        kfree(ptr);
        return KERN_INVALID_ID;
    }
    return cap;
}

void *kmem_resolve_cap(cap_id_t cap, uint8_t required_rights) {
    return cap_resolve(cap, CAP_OBJ_MEMBLOCK, required_rights);
}

kern_err_t kmem_free_cap(cap_id_t cap) {
    void *ptr = cap_resolve(cap, CAP_OBJ_MEMBLOCK, CAP_MANAGE);
    if (!ptr) {
        return KERN_ERR_CAP;
    }

    (void)cap_register_cleanup(CAP_OBJ_MEMBLOCK, kmem_cap_cleanup);
    return cap_revoke(cap);
}

#endif
