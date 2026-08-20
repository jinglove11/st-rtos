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
/* genconfig 不物化 .config 缺失符号的 Kconfig 默认值(P0-8 缺口):
 * 回退与 Kconfig default 16 保持一致。 */
#ifndef MPU_MAP_MAX
#define MPU_MAP_MAX 16
#endif

/* P1-3: 软映射表项 —— 映射的持久记录;硬件槽(3..)是它的 LRU 驻留缓存 */
typedef struct {
    uintptr_t base;
    uint32_t  size;
    uint32_t  attr;      /* AP_*|ATTR_*|XN 意图(不含 RASR_ENABLE) */
    int8_t    slot;      /* 当前驻留硬件槽(3..),-1 = 未驻留 */
    uint32_t  lru_seq;   /* 驻留换序 */
    uint8_t   in_use;
} mpu_map_t;

typedef struct address_space {
    uint32_t regions[MPU_REGION_COUNT][2];  /* 硬件镜像(SVC/PendSV 重载源) */
    mpu_map_t maps[MPU_MAP_MAX];            /* P1-3: 软映射表 */
    int8_t   slot_owner[MPU_REGION_COUNT];  /* 槽 → 软表项 idx,-1 空 */
    uint32_t lru_tick;
    /* P1-4: 静态 region 1 的私有 data/heap 域(base==0 = 未附加;
     * frame cap 由任务持有,生命周期随 cap 吊销) */
    uintptr_t domain_base;
    uint32_t  domain_size;
    cap_id_t  domain_cap;
    uint8_t  in_use;
} address_space_t;

/* 用户任务创建时获取;失败返回 NULL(池耗尽) */
address_space_t *mpu_aspace_acquire(void);
/* 任务资源清理时归还(幂等,同时清 TCB 指针) */
void mpu_aspace_release_task(tcb_t *tcb);

/* P1-3: 动态映射接口(内核内部;mem.c 的 kshm/kmmio 走这里)
 * attr = AP_*|ATTR_*|XN_ENABLE(不含 RASR_ENABLE)。
 * 硬件运行时槽满时不失败 —— 新表项暂不驻留,首次访问经 MemManage
 * 按需换入(mpu_map_demand_load)。 */
int mpu_map_add(tcb_t *task, uintptr_t base, uint32_t size, uint32_t attr);
kern_err_t mpu_map_remove(tcb_t *task, uintptr_t base);
/* 诊断/测试:base 覆盖的软表项当前驻留槽,-1 = 未驻留/无映射 */
int mpu_map_slot_of(tcb_t *task, uintptr_t base);
/* MemManage 按需换入(fault.c 调,作用域=故障核):
 *   1  = 已换入(写硬件+镜像),异常返回重试即恢复
 *   0  = 命中映射且已驻留(真实权限/执行违例,按故障处理)
 *  -1  = 无覆盖映射 */
int mpu_map_demand_load(tcb_t *task, uint32_t fault_addr);
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
