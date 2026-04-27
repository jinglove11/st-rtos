/**
 * @file mem.c
 * @brief 动态内存管理实现
 * 
 * 使用首次适应算法
 * 空闲块链表按地址排序
 */

#include "mem.h"
#include "kernel_config.h"
#include "hal.h"
#include <string.h>

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

static uint8_t mem_heap[KERN_HEAP_SIZE] __attribute__((aligned(MEM_ALIGN_SIZE)));

static mem_block_t *free_list = NULL;
static mem_stats_t mem_stats;

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
    size_t heap_size = KERN_HEAP_SIZE;
    
    heap_size = MEM_ALIGN_DOWN(heap_size, MEM_ALIGN_SIZE);
    
    mem_block_t *initial = (mem_block_t *)mem_heap;
    block_init(initial, heap_size - BLOCK_HEADER_SIZE, MEM_BLOCK_FREE);
    
    free_list = initial;
    
    memset(&mem_stats, 0, sizeof(mem_stats));
    mem_stats.total_size = heap_size;
    mem_stats.free_size = heap_size - BLOCK_HEADER_SIZE;
}

void *kmalloc(size_t size) {
    if (size == 0) return NULL;
    
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
    
    if (mem_stats.used_size > mem_stats.max_used) {
        mem_stats.max_used = mem_stats.used_size;
    }
    
    crit_exit(crit);
    
    return block_to_ptr(block);
}

void kfree(void *ptr) {
    if (!ptr) return;
    
    uint32_t crit = crit_enter();
    
    mem_block_t *block = ptr_to_block(ptr);
    
    if (!block_is_valid(block) || block->flags == MEM_BLOCK_FREE) {
        crit_exit(crit);
        return;
    }
    
    block->flags = MEM_BLOCK_FREE;
    
    mem_stats.used_size -= block->size;
    mem_stats.free_size += block->size;
    mem_stats.free_count++;
    
    free_list_insert(block);
    
    try_merge_with_next(block);
    
    if (block->prev && block->prev->flags == MEM_BLOCK_FREE) {
        mem_block_t *prev = block->prev;
        try_merge_with_next(prev);
    }
    
    crit_exit(crit);
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
