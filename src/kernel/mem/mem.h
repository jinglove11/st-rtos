/**
 * @file mem.h
 * @brief 内存管理接口
 */

#ifndef MEM_H
#define MEM_H

#include <stdint.h>
#include <stddef.h>
#include "kernel_types.h"

typedef struct {
    size_t total_size;
    size_t used_size;
    size_t free_size;
    size_t max_used;
    uint32_t alloc_count;
    uint32_t free_count;
    uint32_t fail_count;
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

#endif
