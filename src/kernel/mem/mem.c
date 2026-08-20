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
#include "spinlock.h"
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

/* SMP spinlock protecting the heap free-list. In single-core mode
 * uncontended → IRQ disable only. */
static irq_spinlock_t mem_lock;

static uint32_t crit_enter(void) {
    return irq_spin_lock(&mem_lock);
}

static void crit_exit(uint32_t primask) {
    irq_spin_unlock(&mem_lock, primask);
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
    irq_spin_init_rank(&mem_lock, LOCKDEP_RANK_RESOURCE);
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
    if (size > SIZE_MAX - align - sizeof(void *)) return NULL;
    
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
    kobject_header_t hdr;
    void  *base;
    size_t size;
    uint8_t in_use;
    uint8_t aligned;
} kframe_object_t;

typedef struct {
    kobject_header_t hdr;   /* M2-Step3d */
    uintptr_t base;
    size_t    size;
    uint8_t   width;
    uint8_t   in_use;
} kmmio_object_t;

typedef struct {
    kobject_header_t hdr;   /* M2-Step3d */
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
static kframe_object_t kframe_objects[KFRAME_OBJECT_MAX];
static irq_spinlock_t kframe_lock;

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
        if (!kmmio_objects[i].in_use &&
            !kobj_generation_is_retired(kmmio_objects[i].hdr.generation)) {
            /* M2-Step3d: 跨 memset 保留 generation */
            uint32_t saved_gen = kmmio_objects[i].hdr.generation;
            memset(&kmmio_objects[i], 0, sizeof(kmmio_objects[i]));
            kobj_header_init(&kmmio_objects[i].hdr, CAP_OBJ_MMIO);
            if (saved_gen != 0) {
                kmmio_objects[i].hdr.generation = saved_gen;
            }
            kmmio_objects[i].in_use = 1U;
            return &kmmio_objects[i];
        }
    }
    return NULL;
}

static void kmmio_free_object(kmmio_object_t *mmio) {
    if (mmio != NULL) {
        uint32_t next_gen = kobj_header_prepare_reuse(&mmio->hdr);
        memset(mmio, 0, sizeof(*mmio));
        mmio->hdr.obj_type   = CAP_OBJ_MMIO;
        mmio->hdr.generation = next_gen;
    }
}

/* 前向声明 (kmem_map_to_task 在前,kshm_is_mpu_compliant 定义在后) */
static int kshm_is_mpu_compliant(size_t size);

static kshm_object_t *kshm_alloc_object(void) {
    for (uint32_t i = 0; i < KSHM_OBJECT_MAX; i++) {
        if (!kshm_objects[i].in_use &&
            !kobj_generation_is_retired(kshm_objects[i].hdr.generation)) {
            /* M2-Step3d: 跨 memset 保留 generation */
            uint32_t saved_gen = kshm_objects[i].hdr.generation;
            memset(&kshm_objects[i], 0, sizeof(kshm_objects[i]));
            kobj_header_init(&kshm_objects[i].hdr, CAP_OBJ_SHM);
            if (saved_gen != 0) {
                kshm_objects[i].hdr.generation = saved_gen;
            }
            kshm_objects[i].in_use = 1U;
            return &kshm_objects[i];
        }
    }
    return NULL;
}

static void kshm_free_object(kshm_object_t *shm) {
    if (shm != NULL) {
        uint32_t next_gen = kobj_header_prepare_reuse(&shm->hdr);
        memset(shm, 0, sizeof(*shm));
        shm->hdr.obj_type   = CAP_OBJ_SHM;
        shm->hdr.generation = next_gen;
    }
}

static kframe_object_t *kframe_alloc_object(size_t size) {
    if (size == 0U) {
        return NULL;
    }

    uint32_t crit = irq_spin_lock(&kframe_lock);
    kframe_object_t *frame = NULL;
    for (uint32_t i = 0; i < KFRAME_OBJECT_MAX; i++) {
        if (!kframe_objects[i].in_use &&
            !kobj_generation_is_retired(
                kframe_objects[i].hdr.generation)) {
            frame = &kframe_objects[i];
            break;
        }
    }
    if (frame == NULL) {
        irq_spin_unlock(&kframe_lock, crit);
        return NULL;
    }

    uint32_t generation = frame->hdr.generation;
    memset(frame, 0, sizeof(*frame));
    kobj_header_init(&frame->hdr, CAP_OBJ_FRAME);
    if (generation != 0U) {
        frame->hdr.generation = generation;
    }
    frame->in_use = 1U;
    frame->base = kmalloc_aligned(size, 32U);
    if (frame->base == NULL) {
        frame->in_use = 0U;
        irq_spin_unlock(&kframe_lock, crit);
        return NULL;
    }
    frame->size = size;
    frame->aligned = 1U;
    irq_spin_unlock(&kframe_lock, crit);
    return frame;
}

static void kframe_free_object(kframe_object_t *frame) {
    if (frame == NULL) {
        return;
    }

    uint32_t crit = irq_spin_lock(&kframe_lock);
    if (frame < &kframe_objects[0] ||
        frame >= &kframe_objects[KFRAME_OBJECT_MAX] ||
        !frame->in_use) {
        irq_spin_unlock(&kframe_lock, crit);
        return;
    }

    if (frame->base != NULL) {
        if (frame->aligned) {
            kfree_aligned(frame->base);
        } else {
            kfree(frame->base);
        }
    }
    uint32_t next_generation = kobj_header_prepare_reuse(&frame->hdr);
    memset(frame, 0, sizeof(*frame));
    frame->hdr.obj_type = CAP_OBJ_FRAME;
    frame->hdr.generation = next_generation;
    irq_spin_unlock(&kframe_lock, crit);
}

static void kmem_cap_cleanup(void *object, uint8_t obj_type) {
    if (obj_type == CAP_OBJ_FRAME && object != NULL) {
        kframe_free_object((kframe_object_t *)object);
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
    if (obj_type == CAP_OBJ_FRAME || obj_type == CAP_OBJ_SHM) {
        kshm_unmap_cap_from_all_tasks(cap);
    }
}

void kframe_init(void) {
    irq_spin_init_rank(&kframe_lock, LOCKDEP_RANK_OBJECT);
    memset(kframe_objects, 0, sizeof(kframe_objects));
    for (uint32_t i = 0; i < KFRAME_OBJECT_MAX; i++) {
        kobj_header_init(&kframe_objects[i].hdr, CAP_OBJ_FRAME);
    }
    (void)cap_register_cleanup(CAP_OBJ_FRAME, kmem_cap_cleanup);
    (void)cap_register_revoke_hook(CAP_OBJ_FRAME,
                                   kmem_cap_revoke_hook);
}

cap_id_t kframe_create_cap_for(tcb_t *owner, size_t size, uint8_t rights) {
    if (rights == 0) {
        rights = CAP_READ | CAP_WRITE | CAP_MANAGE;
    }
    if ((rights & ~CAP_FULL) != 0U) {
        return KERN_INVALID_ID;
    }

    kframe_object_t *frame = kframe_alloc_object(size);
    if (frame == NULL) {
        return KERN_INVALID_ID;
    }

    cap_id_t cap = cap_create_for_gen(owner, frame, CAP_OBJ_FRAME, rights,
                                      frame->hdr.generation);
    if (cap == KERN_INVALID_ID) {
        kframe_free_object(frame);
        return KERN_INVALID_ID;
    }
    return cap;
}

cap_id_t kframe_create_cap(size_t size, uint8_t rights) {
    return kframe_create_cap_for(sched_get_current(), size, rights);
}

#if USER_DOMAIN
/*============================================================================
 * P1-4: 用户任务私有 data/heap 域(静态 region 1,Frame 后端)
 *============================================================================*/

kern_err_t kuser_domain_attach(tcb_t *task, size_t size, void **out_base) {
    if (out_base != NULL) {
        *out_base = NULL;
    }
    if (task == NULL || size == 0U) {
        return KERN_ERR_PARAM;
    }
    if (task->aspace == NULL) {
        return KERN_ERR_STATE;
    }
    /* region 1 已占用(重复附加) */
    if (task->aspace->domain_base != 0U ||
        (task->aspace->regions[1][1] & RASR_ENABLE) != 0U) {
        return KERN_ERR_BUSY;
    }

    cap_id_t cap = kframe_create_cap_for(task, size,
                                         CAP_READ | CAP_WRITE | CAP_MANAGE);
    if (cap < 0) {
        return KERN_ERR_RESOURCE;
    }
    kframe_object_t *frame =
        cap_lookup_for(task, cap, CAP_OBJ_FRAME, CAP_READ);
    if (frame == NULL) {
        (void)cap_delete(cap);
        return KERN_ERR_STATE;
    }
    if (!kshm_is_mpu_compliant(frame->size)) {
        (void)cap_delete(cap);
        return KERN_ERR_PARAM;
    }

    mpu_region_encode(1, (uint32_t)(uintptr_t)frame->base,
                      (uint32_t)frame->size,
                      RASR_ENABLE | AP_FULL | ATTR_NORMAL_WBWA | XN_ENABLE,
                      &task->aspace->regions[1][0],
                      &task->aspace->regions[1][1]);
    task->aspace->domain_base = (uintptr_t)frame->base;
    task->aspace->domain_size = (uint32_t)frame->size;
    task->aspace->domain_cap = cap;

    if (out_base != NULL) {
        *out_base = frame->base;
    }
    return KERN_OK;
}

kern_err_t kuser_domain_detach(tcb_t *task) {
    if (task == NULL || task->aspace == NULL) {
        return KERN_ERR_PARAM;
    }
    if (task->aspace->domain_base == 0U) {
        return KERN_ERR_NOEXIST;
    }

    task->aspace->regions[1][0] = 0;
    task->aspace->regions[1][1] = 0;
    cap_id_t cap = task->aspace->domain_cap;
    task->aspace->domain_base = 0;
    task->aspace->domain_size = 0;
    task->aspace->domain_cap = KERN_INVALID_ID;
    if (cap >= 0) {
        (void)cap_delete(cap);  /* 最后一引用 → frame 经吊销钩子回收 */
    }
    return KERN_OK;
}
#endif /* USER_DOMAIN */

cap_id_t kmem_alloc_cap(size_t size, uint8_t rights) {
    return kframe_create_cap(size, rights);
}

void *kmem_resolve_cap(cap_id_t cap, uint8_t required_rights) {
    kframe_object_t *frame =
        cap_resolve(cap, CAP_OBJ_FRAME, required_rights);
    return frame != NULL ? frame->base : NULL;
}

kern_err_t kmem_free_cap(cap_id_t cap) {
    kframe_object_t *frame =
        cap_resolve(cap, CAP_OBJ_FRAME, CAP_MANAGE);
    if (frame == NULL) {
        return KERN_ERR_CAP;
    }
    return cap_revoke(cap);
}

kern_err_t kmem_get_bounds(cap_id_t cap, void **base, size_t *size) {
    if (base == NULL || size == NULL) {
        return KERN_ERR_PARAM;
    }

    kframe_object_t *frame = cap_resolve(cap, CAP_OBJ_FRAME, CAP_READ);
    if (frame == NULL) {
        return KERN_ERR_CAP;
    }

    *base = frame->base;
    *size = frame->size;
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

    kframe_object_t *frame =
        cap_resolve(cap, CAP_OBJ_FRAME, required_rights);
    if (frame == NULL) {
        return KERN_ERR_CAP;
    }
    if (offset > frame->size || len > (frame->size - offset)) {
        return KERN_ERR_PARAM;
    }

    *ptr = (void *)((uint8_t *)frame->base + offset);
    return KERN_OK;
}

kern_err_t kmem_map_to_task(tcb_t *task, cap_id_t cap,
                            uint8_t rights, void **out_addr) {
#if MPU_ENABLE
    if (task == NULL || out_addr == NULL) {
        return KERN_ERR_PARAM;
    }
    *out_addr = NULL;
    if (rights != CAP_READ && rights != (CAP_READ | CAP_WRITE)) {
        return KERN_ERR_PARAM;
    }

    kframe_object_t *frame =
        cap_lookup_for(task, cap, CAP_OBJ_FRAME, rights);
    if (frame == NULL) {
        return KERN_ERR_CAP;
    }
    if (!kshm_is_mpu_compliant(frame->size) ||
        (((uintptr_t)frame->base & 0x1FU) != 0U)) {
        return KERN_ERR_PARAM;
    }

    /* 复用 shm_maps[] 记录映射 (Frame 与 SHM 在 MPU region 层面等价) */
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

    /* P1-3: 登记软映射表;硬件运行时槽满时不失败(MemManage 按需换入)。
     * SHM/Frame 数据区 */
    uint32_t ap = (rights & CAP_WRITE) ? AP_FULL : AP_PRW_URO;
    int map_id = mpu_map_add(task, (uintptr_t)frame->base,
                             (uint32_t)frame->size, ap | ATTR_NORMAL_WBWA | XN_ENABLE);
    if (map_id < 0) {
        return (kern_err_t)(-map_id);
    }

    task->shm_maps[map_slot].in_use = 1U;
    task->shm_maps[map_slot].region = 0xFFU;  /* P1-3: 驻留由软表管理,此字段为顾问值 */
    task->shm_maps[map_slot].rights = rights;
    task->shm_maps[map_slot].cap = cap;
    task->shm_maps[map_slot].addr = frame->base;
    task->shm_maps[map_slot].size = frame->size;

    *out_addr = frame->base;
    return KERN_OK;
#else
    (void)task; (void)cap; (void)rights; (void)out_addr;
    return KERN_ERR_STATE;
#endif
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
    cap_id_t cap = cap_create_for_gen(NULL, mmio, CAP_OBJ_MMIO, rights,
                                      mmio->hdr.generation);
    if (cap == KERN_INVALID_ID) {
        kmmio_free_object(mmio);
        return KERN_ERR_RESOURCE;
    }

    *out_cap = cap;
    return KERN_OK;
}

kern_err_t kmmio_create_cap_for(tcb_t *owner, uintptr_t base, size_t size,
                                uint8_t width, uint8_t rights,
                                cap_id_t *out_cap) {
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
    cap_id_t cap = cap_create_for_gen(owner, mmio, CAP_OBJ_MMIO, rights,
                                      mmio->hdr.generation);
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
    cap_id_t cap = cap_create_for_gen(NULL, shm, CAP_OBJ_SHM, rights, shm->hdr.generation);
    if (cap == KERN_INVALID_ID) {
        kmem_cap_cleanup(shm, CAP_OBJ_SHM);
        return KERN_INVALID_ID;
    }
    return cap;
}

/*
 * PMSAv8 MPU 区在 RLAR 里只需要 32 字节对齐(RBAR/RLAR 都按 0x1F 对齐)。
 * 老 strong-order PMSAv7 必须把区做成 2 的幂 + size 等于 align,这导致
 * 用户要 1KB 共享内存就得分配 2KB。RP2350 上放宽到 size 是 32 的倍数即可,
 * align 用 32 而不是 size。
 */
static int kshm_is_mpu_compliant(size_t size) {
    return size >= 32U && (size % 32U) == 0U;
}

cap_id_t kshm_create_aligned_cap(size_t size, uint8_t rights) {
    if (!kshm_is_mpu_compliant(size)) {
        return KERN_INVALID_ID;
    }
    if (rights == 0) {
        rights = CAP_READ | CAP_WRITE | CAP_MANAGE | CAP_TRANSFER | CAP_GRANT;
    }

    kshm_object_t *shm = kshm_alloc_object();
    if (!shm) {
        return KERN_INVALID_ID;
    }

    shm->base = kmalloc_aligned(size, 32);
    if (!shm->base) {
        kshm_free_object(shm);
        return KERN_INVALID_ID;
    }
    shm->size = size;
    shm->aligned = 1U;

    (void)cap_register_cleanup(CAP_OBJ_SHM, kmem_cap_cleanup);
    (void)cap_register_revoke_hook(CAP_OBJ_SHM, kmem_cap_revoke_hook);
    cap_id_t cap = cap_create_for_gen(NULL, shm, CAP_OBJ_SHM, rights, shm->hdr.generation);
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
    if (!kshm_is_mpu_compliant(shm->size) ||
        (((uintptr_t)shm->base & 0x1FU) != 0U)) {
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

    /* P1-3: 登记软映射表;硬件运行时槽满时不失败(MemManage 按需换入)。
     * SHM 数据区 */
    uint32_t ap = (rights & CAP_WRITE) ? AP_FULL : AP_PRW_URO;
    int map_id = mpu_map_add(task, (uintptr_t)shm->base,
                             (uint32_t)shm->size, ap | ATTR_NORMAL_WBWA | XN_ENABLE);
    if (map_id < 0) {
        return (kern_err_t)(-map_id);
    }

    task->shm_maps[map_slot].in_use = 1U;
    task->shm_maps[map_slot].region = 0xFFU;  /* P1-3: 驻留由软表管理,此字段为顾问值 */
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

            /* P1-3: 生命周期在软表(含驻留槽清除) */
            (void)mpu_map_remove(task, (uintptr_t)task->shm_maps[i].addr);
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

            /* P1-3: 生命周期在软表(含驻留槽清除) */
            (void)mpu_map_remove(task, (uintptr_t)task->shm_maps[i].addr);
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
            if (task->aspace != NULL) {
                task->aspace->regions[region][0] = 0;
                task->aspace->regions[region][1] = 0;
            }
        }
        memset(&task->shm_maps[i], 0, sizeof(task->shm_maps[i]));
    }
#else
    (void)task;
#endif
}

/*============================================================================
 * MMIO mapping — user-mode driver peripheral access
 *============================================================================*/

/* MMIO mappings don't have a per-task tracking table like shm_maps (TCB has no
 * mmio_maps field). We track the occupied region by cap id stored in a small
 * side table keyed by task id, so unmap can find which region to clear.
 * Region 3..7 are shared with shm — a task can mix shm + mmio mappings as long
 * as the total stays within the 5 free regions. */
#define TASK_MMIO_MAP_MAX 4
typedef struct {
    uint8_t  in_use;
    uint8_t  region;   /* P1-3 后为顾问值(驻留槽或 0xFF),生命周期在软表 */
    cap_id_t cap;
    void    *addr;     /* P1-3: unmap 定位软映射表项(cap 可能已被吊销) */
} mmio_map_t;
static mmio_map_t task_mmio_maps[KERNEL_MAX_TASKS][TASK_MMIO_MAP_MAX];

kern_err_t kmmio_map_to_task(tcb_t *task, cap_id_t cap,
                             uint8_t rights, void **out_addr) {
#if MPU_ENABLE
    if (task == NULL || out_addr == NULL) {
        return KERN_ERR_PARAM;
    }
    *out_addr = NULL;
    if (rights != CAP_READ && rights != (CAP_READ | CAP_WRITE)) {
        return KERN_ERR_PARAM;
    }
    if (task->id < 0 || task->id >= KERNEL_MAX_TASKS) {
        return KERN_ERR_PARAM;
    }

    kmmio_object_t *mmio = cap_lookup_for(task, cap, CAP_OBJ_MMIO, rights);
    if (!mmio) {
        return KERN_ERR_CAP;
    }
    if (!kshm_is_mpu_compliant(mmio->size) ||
        ((mmio->base & 0x1FU) != 0U)) {
        return KERN_ERR_PARAM;
    }

    /* Already mapped? */
    for (uint32_t i = 0; i < TASK_MMIO_MAP_MAX; i++) {
        if (task_mmio_maps[task->id][i].in_use &&
            task_mmio_maps[task->id][i].cap == cap) {
            return KERN_ERR_BUSY;
        }
    }

    int map_slot = -1;
    for (uint32_t i = 0; i < TASK_MMIO_MAP_MAX; i++) {
        if (!task_mmio_maps[task->id][i].in_use) {
            map_slot = (int)i;
            break;
        }
    }
    if (map_slot < 0) {
        return KERN_ERR_RESOURCE;
    }

    /* P1-3: 登记软映射表;硬件运行时槽满时不失败(MemManage 按需换入)。
     * Device 强序 */
    uint32_t ap = (rights & CAP_WRITE) ? AP_FULL : AP_PRW_URO;
    int map_id = mpu_map_add(task, (uintptr_t)mmio->base,
                             (uint32_t)mmio->size, ap | ATTR_DEVICE | XN_ENABLE);
    if (map_id < 0) {
        return (kern_err_t)(-map_id);
    }

    task_mmio_maps[task->id][map_slot].in_use = 1U;
    task_mmio_maps[task->id][map_slot].region = 0xFFU;  /* P1-3: 驻留由软表管理,此字段为顾问值 */
    task_mmio_maps[task->id][map_slot].cap = cap;
    task_mmio_maps[task->id][map_slot].addr = (void *)(uintptr_t)mmio->base;

    /* If the task is mapping into itself (the common case: a driver task
     * calls sys_mmio_map from its own context), the new MPU region won't take
     * effect until the next context switch. Reload immediately so the caller
     * can access the region right after map returns. */
    {
        extern tcb_t *sched_get_current(void);
        if (task == sched_get_current()) {
            mpu_load_task_regions(task);
        }
    }

    *out_addr = (void *)(uintptr_t)mmio->base;
    return KERN_OK;
#else
    (void)task; (void)cap; (void)rights; (void)out_addr;
    return KERN_ERR_STATE;
#endif
}

kern_err_t kmmio_unmap_from_task(tcb_t *task, cap_id_t cap) {
#if MPU_ENABLE
    if (task == NULL || task->id < 0 || task->id >= KERNEL_MAX_TASKS) {
        return KERN_ERR_PARAM;
    }
    for (uint32_t i = 0; i < TASK_MMIO_MAP_MAX; i++) {
        if (task_mmio_maps[task->id][i].in_use &&
            task_mmio_maps[task->id][i].cap == cap) {
            (void)mpu_map_remove(task,
                                 (uintptr_t)task_mmio_maps[task->id][i].addr);
            memset(&task_mmio_maps[task->id][i], 0,
                   sizeof(task_mmio_maps[task->id][i]));
            return KERN_OK;
        }
    }
    return KERN_ERR_NOEXIST;
#else
    (void)task; (void)cap;
    return KERN_ERR_STATE;
#endif
}

void kmmio_unmap_all_for_task(tcb_t *task) {
#if MPU_ENABLE
    if (task == NULL || task->id < 0 || task->id >= KERNEL_MAX_TASKS) {
        return;
    }
    for (uint32_t i = 0; i < TASK_MMIO_MAP_MAX; i++) {
        if (task_mmio_maps[task->id][i].in_use) {
            (void)mpu_map_remove(task,
                                 (uintptr_t)task_mmio_maps[task->id][i].addr);
            memset(&task_mmio_maps[task->id][i], 0,
                   sizeof(task_mmio_maps[task->id][i]));
        }
    }
#else
    (void)task;
#endif
}

#endif
