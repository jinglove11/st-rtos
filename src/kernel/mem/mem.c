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
#include "task.h"
#include "capability.h"
#include "mpu.h"
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

typedef struct {
    void  *base;
    size_t size;
} kmem_object_t;

typedef struct {
    uintptr_t base;
    size_t    size;
    uint8_t   width;
    uint8_t   in_use;
} kmmio_object_t;

typedef struct {
    void  *base;
    size_t size;
    uint8_t in_use;
    uint8_t aligned;
} kshm_object_t;

#define KMMIO_PERIPH_BASE 0x40000000UL
#define KMMIO_PERIPH_SIZE 0x20000000UL
#define KMMIO_OBJECT_MAX  8
#define KSHM_OBJECT_MAX   8

static kmmio_object_t kmmio_objects[KMMIO_OBJECT_MAX];
static kshm_object_t kshm_objects[KSHM_OBJECT_MAX];

static int kmem_range_in_region(uintptr_t base, size_t size,
                                uintptr_t region_base, size_t region_size) {
    if (size == 0) {
        return 0;
    }

    uintptr_t end = base + (uintptr_t)size - 1U;
    uintptr_t region_end = region_base + (uintptr_t)region_size - 1U;
    if (end < base || region_end < region_base) {
        return 0;
    }

    return base >= region_base && end <= region_end;
}

static int kmmio_width_valid(uint8_t width) {
    return width == 1U || width == 2U || width == 4U;
}

static kmmio_object_t *kmmio_alloc_object(void) {
    for (uint32_t i = 0; i < KMMIO_OBJECT_MAX; i++) {
        if (!kmmio_objects[i].in_use) {
            memset(&kmmio_objects[i], 0, sizeof(kmmio_objects[i]));
            kmmio_objects[i].in_use = 1U;
            return &kmmio_objects[i];
        }
    }
    return NULL;
}

static void kmmio_free_object(kmmio_object_t *mmio) {
    if (mmio != NULL) {
        memset(mmio, 0, sizeof(*mmio));
    }
}

static kshm_object_t *kshm_alloc_object(void) {
    for (uint32_t i = 0; i < KSHM_OBJECT_MAX; i++) {
        if (!kshm_objects[i].in_use) {
            memset(&kshm_objects[i], 0, sizeof(kshm_objects[i]));
            kshm_objects[i].in_use = 1U;
            return &kshm_objects[i];
        }
    }
    return NULL;
}

static void kshm_free_object(kshm_object_t *shm) {
    if (shm != NULL) {
        memset(shm, 0, sizeof(*shm));
    }
}

static void kmem_cap_cleanup(void *object, uint8_t obj_type) {
    if (obj_type == CAP_OBJ_MEMBLOCK && object != NULL) {
        kmem_object_t *mem = (kmem_object_t *)object;
        if (mem->base != NULL) {
            kfree(mem->base);
            mem->base = NULL;
        }
        kfree(mem);
    } else if (obj_type == CAP_OBJ_MMIO && object != NULL) {
        kmmio_free_object((kmmio_object_t *)object);
    } else if (obj_type == CAP_OBJ_SHM && object != NULL) {
        kshm_object_t *shm = (kshm_object_t *)object;
        if (shm->base != NULL) {
            if (shm->aligned) {
                kfree_aligned(shm->base);
            } else {
                kfree(shm->base);
            }
            shm->base = NULL;
        }
        kshm_free_object(shm);
    }
}

static void kmem_cap_revoke_hook(cap_id_t cap, void *object, uint8_t obj_type) {
    (void)object;
    if (obj_type == CAP_OBJ_SHM) {
        kshm_unmap_cap_from_all_tasks(cap);
    }
}

cap_id_t kmem_alloc_cap(size_t size, uint8_t rights) {
    if (rights == 0) {
        rights = CAP_READ | CAP_WRITE | CAP_MANAGE;
    }

    kmem_object_t *mem = (kmem_object_t *)kmalloc(sizeof(kmem_object_t));
    if (!mem) {
        return KERN_INVALID_ID;
    }
    memset(mem, 0, sizeof(*mem));

    mem->base = kmalloc(size);
    if (!mem->base) {
        kfree(mem);
        return KERN_INVALID_ID;
    }
    mem->size = size;

    (void)cap_register_cleanup(CAP_OBJ_MEMBLOCK, kmem_cap_cleanup);
    cap_id_t cap = cap_create(mem, CAP_OBJ_MEMBLOCK, rights, 0);
    if (cap == KERN_INVALID_ID) {
        kmem_cap_cleanup(mem, CAP_OBJ_MEMBLOCK);
        return KERN_INVALID_ID;
    }
    return cap;
}

void *kmem_resolve_cap(cap_id_t cap, uint8_t required_rights) {
    kmem_object_t *mem = cap_resolve(cap, CAP_OBJ_MEMBLOCK, required_rights);
    return mem ? mem->base : NULL;
}

kern_err_t kmem_free_cap(cap_id_t cap) {
    kmem_object_t *mem = cap_resolve(cap, CAP_OBJ_MEMBLOCK, CAP_MANAGE);
    if (!mem) {
        return KERN_ERR_CAP;
    }

    (void)cap_register_cleanup(CAP_OBJ_MEMBLOCK, kmem_cap_cleanup);
    return cap_revoke(cap);
}

kern_err_t kmem_get_bounds(cap_id_t cap, void **base, size_t *size) {
    if (base == NULL || size == NULL) {
        return KERN_ERR_PARAM;
    }

    kmem_object_t *mem = cap_resolve(cap, CAP_OBJ_MEMBLOCK, CAP_READ);
    if (!mem) {
        return KERN_ERR_CAP;
    }

    *base = mem->base;
    *size = mem->size;
    return KERN_OK;
}

kern_err_t kmem_get_range(cap_id_t cap, uint8_t required_rights,
                          size_t offset, size_t len, void **ptr) {
    if (ptr == NULL) {
        return KERN_ERR_PARAM;
    }
    if (len == 0) {
        return KERN_ERR_PARAM;
    }

    kmem_object_t *mem = cap_resolve(cap, CAP_OBJ_MEMBLOCK, required_rights);
    if (!mem) {
        return KERN_ERR_CAP;
    }
    if (offset > mem->size || len > (mem->size - offset)) {
        return KERN_ERR_PARAM;
    }

    *ptr = (void *)((uint8_t *)mem->base + offset);
    return KERN_OK;
}

kern_err_t kmmio_create_cap(uintptr_t base, size_t size, uint8_t width,
                            uint8_t rights, cap_id_t *out_cap) {
    if (out_cap == NULL || size == 0 || !kmmio_width_valid(width)) {
        return KERN_ERR_PARAM;
    }
    *out_cap = KERN_INVALID_ID;

    if ((base & ((uintptr_t)width - 1U)) != 0U) {
        return KERN_ERR_PARAM;
    }
    if (!kmem_range_in_region(base, size, KMMIO_PERIPH_BASE, KMMIO_PERIPH_SIZE)) {
        return KERN_ERR_PERM;
    }

    if (rights == 0) {
        rights = CAP_READ | CAP_WRITE | CAP_MANAGE;
    }

    kmmio_object_t *mmio = kmmio_alloc_object();
    if (!mmio) {
        return KERN_ERR_RESOURCE;
    }
    mmio->base = base;
    mmio->size = size;
    mmio->width = width;

    (void)cap_register_cleanup(CAP_OBJ_MMIO, kmem_cap_cleanup);
    cap_id_t cap = cap_create_for(NULL, mmio, CAP_OBJ_MMIO, rights);
    if (cap == KERN_INVALID_ID) {
        kmmio_free_object(mmio);
        return KERN_ERR_RESOURCE;
    }

    *out_cap = cap;
    return KERN_OK;
}

kern_err_t kmmio_delete_cap(cap_id_t cap) {
    kmmio_object_t *mmio = cap_resolve(cap, CAP_OBJ_MMIO, CAP_MANAGE);
    if (!mmio) {
        return KERN_ERR_CAP;
    }

    (void)cap_register_cleanup(CAP_OBJ_MMIO, kmem_cap_cleanup);
    return cap_revoke(cap);
}

kern_err_t kmmio_get_bounds(cap_id_t cap, uintptr_t *base, size_t *size,
                            uint8_t *width) {
    if (base == NULL || size == NULL || width == NULL) {
        return KERN_ERR_PARAM;
    }

    kmmio_object_t *mmio = cap_resolve(cap, CAP_OBJ_MMIO, CAP_READ);
    if (!mmio) {
        return KERN_ERR_CAP;
    }

    *base = mmio->base;
    *size = mmio->size;
    *width = mmio->width;
    return KERN_OK;
}

cap_id_t kshm_create_cap(size_t size, uint8_t rights) {
    if (size == 0) {
        return KERN_INVALID_ID;
    }
    if (rights == 0) {
        rights = CAP_READ | CAP_WRITE | CAP_MANAGE | CAP_TRANSFER | CAP_GRANT;
    }

    kshm_object_t *shm = kshm_alloc_object();
    if (!shm) {
        return KERN_INVALID_ID;
    }

    shm->base = kmalloc(size);
    if (!shm->base) {
        kshm_free_object(shm);
        return KERN_INVALID_ID;
    }
    shm->size = size;

    (void)cap_register_cleanup(CAP_OBJ_SHM, kmem_cap_cleanup);
    (void)cap_register_revoke_hook(CAP_OBJ_SHM, kmem_cap_revoke_hook);
    cap_id_t cap = cap_create_for(NULL, shm, CAP_OBJ_SHM, rights);
    if (cap == KERN_INVALID_ID) {
        kmem_cap_cleanup(shm, CAP_OBJ_SHM);
        return KERN_INVALID_ID;
    }
    return cap;
}

static int kshm_is_power_of_two(size_t size) {
    return size != 0 && (size & (size - 1U)) == 0;
}

cap_id_t kshm_create_aligned_cap(size_t size, uint8_t rights) {
    if (size < 32 || !kshm_is_power_of_two(size)) {
        return KERN_INVALID_ID;
    }
    if (rights == 0) {
        rights = CAP_READ | CAP_WRITE | CAP_MANAGE | CAP_TRANSFER | CAP_GRANT;
    }

    kshm_object_t *shm = kshm_alloc_object();
    if (!shm) {
        return KERN_INVALID_ID;
    }

    shm->base = kmalloc_aligned(size, size);
    if (!shm->base) {
        kshm_free_object(shm);
        return KERN_INVALID_ID;
    }
    shm->size = size;
    shm->aligned = 1U;

    (void)cap_register_cleanup(CAP_OBJ_SHM, kmem_cap_cleanup);
    (void)cap_register_revoke_hook(CAP_OBJ_SHM, kmem_cap_revoke_hook);
    cap_id_t cap = cap_create_for(NULL, shm, CAP_OBJ_SHM, rights);
    if (cap == KERN_INVALID_ID) {
        kmem_cap_cleanup(shm, CAP_OBJ_SHM);
        return KERN_INVALID_ID;
    }
    return cap;
}

kern_err_t kshm_delete_cap(cap_id_t cap) {
    kshm_object_t *shm = cap_resolve(cap, CAP_OBJ_SHM, CAP_MANAGE);
    if (!shm) {
        return KERN_ERR_CAP;
    }

    (void)cap_register_cleanup(CAP_OBJ_SHM, kmem_cap_cleanup);
    (void)cap_register_revoke_hook(CAP_OBJ_SHM, kmem_cap_revoke_hook);
    return cap_revoke(cap);
}

kern_err_t kshm_get_bounds(cap_id_t cap, void **base, size_t *size) {
    if (base == NULL || size == NULL) {
        return KERN_ERR_PARAM;
    }

    kshm_object_t *shm = cap_resolve(cap, CAP_OBJ_SHM, CAP_READ);
    if (!shm) {
        return KERN_ERR_CAP;
    }

    *base = shm->base;
    *size = shm->size;
    return KERN_OK;
}

kern_err_t kshm_get_range(cap_id_t cap, uint8_t required_rights,
                          size_t offset, size_t len, void **ptr) {
    if (ptr == NULL || len == 0) {
        return KERN_ERR_PARAM;
    }

    kshm_object_t *shm = cap_resolve(cap, CAP_OBJ_SHM, required_rights);
    if (!shm) {
        return KERN_ERR_CAP;
    }
    if (offset > shm->size || len > (shm->size - offset)) {
        return KERN_ERR_PARAM;
    }

    *ptr = (void *)((uint8_t *)shm->base + offset);
    return KERN_OK;
}

kern_err_t kshm_map_to_task(tcb_t *task, cap_id_t cap,
                            uint8_t rights, void **out_addr) {
#if MPU_ENABLE
    if (task == NULL || out_addr == NULL) {
        return KERN_ERR_PARAM;
    }
    *out_addr = NULL;
    if (rights != CAP_READ && rights != (CAP_READ | CAP_WRITE)) {
        return KERN_ERR_PARAM;
    }

    kshm_object_t *shm = cap_lookup_for(task, cap, CAP_OBJ_SHM, rights);
    if (!shm) {
        return KERN_ERR_CAP;
    }
    if (!kshm_is_power_of_two(shm->size) || shm->size < 32 ||
        (((uintptr_t)shm->base & ((uintptr_t)shm->size - 1U)) != 0U)) {
        return KERN_ERR_PARAM;
    }

    for (uint32_t i = 0; i < TASK_SHM_MAP_MAX; i++) {
        if (task->shm_maps[i].in_use && task->shm_maps[i].cap == cap) {
            return KERN_ERR_BUSY;
        }
    }

    int map_slot = -1;
    for (uint32_t i = 0; i < TASK_SHM_MAP_MAX; i++) {
        if (!task->shm_maps[i].in_use) {
            map_slot = (int)i;
            break;
        }
    }
    if (map_slot < 0) {
        return KERN_ERR_RESOURCE;
    }

    int region = -1;
    for (uint32_t r = 3; r < 8; r++) {
        if ((task->mpu_regions[r][1] & RASR_ENABLE) == 0) {
            region = (int)r;
            break;
        }
    }
    if (region < 0) {
        return KERN_ERR_RESOURCE;
    }

    uint32_t ap = (rights & CAP_WRITE) ? AP_FULL : AP_PRW_URO;
    uint32_t rasr = RASR_ENABLE | ap | ATTR_NORMAL_WBWA | XN_ENABLE |
                    mpu_calc_rasr_size((uint32_t)shm->size);
    task->mpu_regions[region][0] = ((uint32_t)(uintptr_t)shm->base) |
                                   RBAR_VALID | (uint32_t)region;
    task->mpu_regions[region][1] = rasr;

    task->shm_maps[map_slot].in_use = 1U;
    task->shm_maps[map_slot].region = (uint8_t)region;
    task->shm_maps[map_slot].rights = rights;
    task->shm_maps[map_slot].cap = cap;
    task->shm_maps[map_slot].addr = shm->base;
    task->shm_maps[map_slot].size = shm->size;

    *out_addr = shm->base;
    return KERN_OK;
#else
    (void)task; (void)cap; (void)rights; (void)out_addr;
    return KERN_ERR_STATE;
#endif
}

kern_err_t kshm_unmap_from_task(tcb_t *task, cap_id_t cap) {
#if MPU_ENABLE
    if (task == NULL) {
        return KERN_ERR_PARAM;
    }

    for (uint32_t i = 0; i < TASK_SHM_MAP_MAX; i++) {
        if (task->shm_maps[i].in_use && task->shm_maps[i].cap == cap) {
            kern_err_t result = KERN_OK;
            if (cap_lookup_for(task, cap, CAP_OBJ_SHM, CAP_READ) == NULL) {
                result = KERN_ERR_CAP;
            }

            uint8_t region = task->shm_maps[i].region;
            if (region < 8) {
                task->mpu_regions[region][0] = 0;
                task->mpu_regions[region][1] = 0;
            }
            memset(&task->shm_maps[i], 0, sizeof(task->shm_maps[i]));
            return result;
        }
    }
    return KERN_ERR_NOEXIST;
#else
    (void)task; (void)cap;
    return KERN_ERR_STATE;
#endif
}

void kshm_unmap_cap_from_all_tasks(cap_id_t cap) {
#if MPU_ENABLE
    tcb_t *current = sched_get_current();
    int current_changed = 0;

    task_id_t id = KERN_INVALID_ID;
    while ((id = task_get_next(id)) != KERN_INVALID_ID) {
        tcb_t *task = task_get_tcb(id);
        if (task == NULL) {
            continue;
        }

        for (uint32_t i = 0; i < TASK_SHM_MAP_MAX; i++) {
            if (!task->shm_maps[i].in_use || task->shm_maps[i].cap != cap) {
                continue;
            }

            uint8_t region = task->shm_maps[i].region;
            if (region < 8) {
                task->mpu_regions[region][0] = 0;
                task->mpu_regions[region][1] = 0;
            }
            memset(&task->shm_maps[i], 0, sizeof(task->shm_maps[i]));
            if (task == current) {
                current_changed = 1;
            }
        }
    }

    if (current_changed && current != NULL) {
        mpu_load_task_regions(current);
    }
#else
    (void)cap;
#endif
}

void kshm_unmap_all_for_task(tcb_t *task) {
#if MPU_ENABLE
    if (task == NULL) {
        return;
    }

    for (uint32_t i = 0; i < TASK_SHM_MAP_MAX; i++) {
        if (!task->shm_maps[i].in_use) {
            continue;
        }
        uint8_t region = task->shm_maps[i].region;
        if (region < 8) {
            task->mpu_regions[region][0] = 0;
            task->mpu_regions[region][1] = 0;
        }
        memset(&task->shm_maps[i], 0, sizeof(task->shm_maps[i]));
    }
#else
    (void)task;
#endif
}

#endif
