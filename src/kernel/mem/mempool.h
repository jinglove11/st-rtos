/**
 * @file mempool.h
 * @brief 内存池接口 (固定大小块分配)
 */

#ifndef MEMPOOL_H
#define MEMPOOL_H

#include <stdint.h>
#include <stddef.h>
#include "kernel_types.h"

typedef int pool_id_t;

#define POOL_INVALID_ID    (-1)

void mempool_init(void);

pool_id_t mempool_create(size_t block_size, uint32_t block_count);
void mempool_delete(pool_id_t pool_id);

void *mempool_alloc(pool_id_t pool_id);
void mempool_free(pool_id_t pool_id, void *block);

uint32_t mempool_get_free_count(pool_id_t pool_id);
uint32_t mempool_get_used_count(pool_id_t pool_id);

#endif
