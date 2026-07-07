/**
 * @file mem.h
 * @brief 内存管理接口
 */

#ifndef MEM_H
#define MEM_H

#include <stdint.h>
#include <stddef.h>
#include "kernel_types.h"
#include "kernel_config.h"

typedef struct {
    size_t total_size;
    size_t used_size;
    size_t free_size;
    size_t max_used;
    uint32_t alloc_count;
    uint32_t free_count;
    uint32_t fail_count;
    uint32_t outstanding_allocs;
    uint32_t invalid_free_count;
} mem_stats_t;

void mem_init(void);

void *kmalloc(size_t size);
void *krealloc(void *ptr, size_t size);
void kfree(void *ptr);

void *kmalloc_aligned(size_t size, size_t align);
void kfree_aligned(void *ptr);

mem_stats_t mem_get_stats(void);
size_t mem_get_free(void);
size_t mem_get_used(void);
uint32_t mem_get_outstanding_allocs(void);
uint32_t mem_get_fail_count(void);

#if CAP_ENABLE
cap_id_t kmem_alloc_cap(size_t size, uint8_t rights);
void    *kmem_resolve_cap(cap_id_t cap, uint8_t required_rights);
kern_err_t kmem_free_cap(cap_id_t cap);
kern_err_t kmem_get_bounds(cap_id_t cap, void **base, size_t *size);
kern_err_t kmem_get_range(cap_id_t cap, uint8_t required_rights,
                          size_t offset, size_t len, void **ptr);
kern_err_t kmmio_create_cap(uintptr_t base, size_t size, uint8_t width,
                            uint8_t rights, cap_id_t *out_cap);
kern_err_t kmmio_create_cap_for(tcb_t *owner, uintptr_t base, size_t size,
                                uint8_t width, uint8_t rights,
                                cap_id_t *out_cap);
kern_err_t kmmio_delete_cap(cap_id_t cap);
kern_err_t kmmio_get_bounds(cap_id_t cap, uintptr_t *base, size_t *size,
                            uint8_t *width);
cap_id_t kshm_create_cap(size_t size, uint8_t rights);
cap_id_t kshm_create_aligned_cap(size_t size, uint8_t rights);
kern_err_t kshm_delete_cap(cap_id_t cap);
kern_err_t kshm_get_bounds(cap_id_t cap, void **base, size_t *size);
kern_err_t kshm_get_range(cap_id_t cap, uint8_t required_rights,
                          size_t offset, size_t len, void **ptr);
kern_err_t kshm_map_to_task(tcb_t *task, cap_id_t cap,
                            uint8_t rights, void **out_addr);
kern_err_t kshm_unmap_from_task(tcb_t *task, cap_id_t cap);
void kshm_unmap_cap_from_all_tasks(cap_id_t cap);
void kshm_unmap_all_for_task(tcb_t *task);

/* MMIO mapping: map a CAP_OBJ_MMIO region into a task's MPU as device memory
 * (ATTR_DEVICE), so a user-mode driver can touch peripheral registers. The
 * task must already hold the cap with the requested rights. Mirrors
 * kshm_map_to_task but uses ATTR_DEVICE and CAP_OBJ_MMIO. */
kern_err_t kmmio_map_to_task(tcb_t *task, cap_id_t cap,
                             uint8_t rights, void **out_addr);
kern_err_t kmmio_unmap_from_task(tcb_t *task, cap_id_t cap);
void kmmio_unmap_all_for_task(tcb_t *task);
#endif

#endif
