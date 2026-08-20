/**
 * @file mpu.h
 * @brief Cortex-M7 MPU 内存保护接口
 */

#ifndef MPU_H
#define MPU_H

#include "kernel_types.h"

#if MPU_ENABLE

/*============================================================================
 * MPU 寄存器宏 (供外部使用)
 *============================================================================*/

/* RBAR */
#define RBAR_VALID            (1UL << 4)

/* RASR — Cortex-M7 (PMSAv7) MPU_RASR bit layout:
 *   [0]     ENABLE
 *   [5:1]   SIZE
 *   [7:6]   Reserved
 *   [15:8]  SRD  (Sub-Region Disable)
 *   [17:16] TEX[2:1]
 *   [18]    TEX[0]
 *   [19]    S    (Shareable)
 *   [20]    C    (Cacheable)
 *   [21]    B    (Bufferable)
 *   [23:22] Reserved
 *   [26:24] AP   (Access Permission, 3-bit)
 *   [27]    Reserved
 *   [28]    XN   (Execute Never)
 *   [31:29] Reserved
 */
#define RASR_ENABLE           (1UL << 0)
#define RASR_SRD_SHIFT        8

/* AP (Access Permission) — bits [26:24] */
#define AP_NOACCESS     (0x0UL << 24)
#define AP_PRW          (0x1UL << 24)
#define AP_PRW_URO      (0x2UL << 24)
#define AP_FULL         (0x3UL << 24)
#define AP_PRO          (0x5UL << 24)

/* Memory type attributes — bits [21:16] */
/* Strongly-ordered: TEX=000, S=1, C=0, B=0 */
#define ATTR_STRONGLY_ORDERED   (1UL << 19)
/* Device: TEX=000, S=0, C=0, B=1 */
#define ATTR_DEVICE             (1UL << 21)
/* Normal, Write-Back, Write-Allocate: TEX=001, S=1, C=1, B=1 */
#define ATTR_NORMAL_WBWA        ((1UL << 18) | (1UL << 19) | (1UL << 20) | (1UL << 21))

#define XN_ENABLE               (1UL << 28)

/*
 * PMSAv8 (Cortex-M33 / RP2350) MAIR attr indices — slots we populate in
 * mpu_init(). The ARMv8-M encoder in mpu_region_encode() picks one of these
 * via the high bits of `attr` (ATTR_NORMAL_WBWA etc.). The legacy PMSAv7
 * ATTR_* values are kept as "intent enums": same bits, repurposed as a tag
 * the encoder translates into an index.
 *
 * On PMSAv7 (Cortex-M7) these ATTR_IDX_* are unused.
 */
#define ATTR_IDX_DEVICE_NGNRNE   0U   /* MAIR[0]=0x00 */
#define ATTR_IDX_NORMAL_WBWA     1U   /* MAIR[1]=0xFF */
#define ATTR_IDX_NORMAL_WT       2U   /* MAIR[2]=0x44 */
#define ATTR_IDX_NORMAL_NC       3U   /* MAIR[3]=0x40 */
#define ATTR_IDX_DEVICE_NGNRE    4U   /* MAIR[4]=0x04 */

/*============================================================================
 * P1-2 (C2): address_space — 从 TCB 分离的 mapping policy 对象
 *
 * slice 1(行为保持):每用户任务 1:1 持有一个池化 address_space,
 * 上下文切换经 TCB 指针加载。后续切片:cap 化/共享(M4 进程)、
 * 动态区分配器(P1-3)。
 *============================================================================*/
#if MPU_ENABLE
typedef struct address_space {
    uint32_t regions[MPU_REGION_COUNT][2];  /* [RBAR, RASR/RLAR] */
    uint8_t  in_use;
} address_space_t;

/* 用户任务创建时获取;失败返回 NULL(池耗尽) */
address_space_t *mpu_aspace_acquire(void);
/* 任务资源清理时归还(幂等,同时清 TCB 指针) */
void mpu_aspace_release_task(tcb_t *tcb);
#endif /* MPU_ENABLE */

void mpu_init(void);
void mpu_region_set(uint32_t region, uint32_t base, uint32_t size, uint32_t attr);
void mpu_region_disable(uint32_t region);
void mpu_region_encode(uint32_t region, uint32_t base, uint32_t size,
                       uint32_t attr, uint32_t *out_rbar,
                       uint32_t *out_limit_attr);
void mpu_stack_region_encode(uint32_t region, uint32_t base, uint32_t size,
                             uint32_t *out_rbar,
                             uint32_t *out_limit_attr);
int mpu_region_allows_read(uint32_t rbar, uint32_t limit_attr);
int mpu_region_allows_write(uint32_t rbar, uint32_t limit_attr);
int mpu_region_is_execute_never(uint32_t rbar, uint32_t limit_attr);
int mpu_region_get_bounds(uint32_t rbar, uint32_t limit_attr,
                          uintptr_t *base, uint32_t *size);
uint32_t mpu_calc_rasr_size(uint32_t size);
uint32_t mpu_stack_guard_rasr(uint32_t base, uint32_t size, uint32_t subregion_disable);
void mpu_load_task_regions(tcb_t *tcb);
void mpu_enable_default_map(void);

#else

void mpu_init(void);
void mpu_load_task_regions(tcb_t *tcb);

#endif /* MPU_ENABLE */

#endif /* MPU_H */
