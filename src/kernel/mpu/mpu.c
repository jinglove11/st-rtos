/**
 * @file mpu.c
 * @brief Cortex-M7 MPU 实现
 */

#include "mpu.h"
#include <string.h>
#include "hal.h"
#include "kernel_config.h"
#include "board_config.h"
#include <stddef.h>

#if MPU_ENABLE

/*============================================================================
 * SCB / MPU 寄存器
 *============================================================================*/

#define SCB_BASE        0xE000ED00UL
#define SCB_SHCSR       (*(volatile uint32_t *)(SCB_BASE + 0x24))
#define SCB_SHCSR_MEMFAULTENA  (1 << 16)
#define SCB_SHCSR_BUSFAULTENA  (1 << 17)
#define SCB_SHCSR_USGFAULTENA  (1 << 18)

#define MPU_BASE        0xE000ED90UL
#define MPU_TYPE        (*(volatile uint32_t *)(MPU_BASE + 0x00))
#define MPU_CTRL        (*(volatile uint32_t *)(MPU_BASE + 0x04))
#define MPU_RNR         (*(volatile uint32_t *)(MPU_BASE + 0x08))
#define MPU_RBAR        (*(volatile uint32_t *)(MPU_BASE + 0x0C))
#if BOARD_MPU_ARMV8
#define MPU_RLAR        (*(volatile uint32_t *)(MPU_BASE + 0x10))
#define MPU_MAIR0       (*(volatile uint32_t *)(MPU_BASE + 0x30))
#define MPU_MAIR1       (*(volatile uint32_t *)(MPU_BASE + 0x34))
#else
#define MPU_RASR        (*(volatile uint32_t *)(MPU_BASE + 0x10))
#endif

/* MPU_CTRL bits */
#define MPU_CTRL_ENABLE       (1 << 0)
#define MPU_CTRL_HFNMIENA     (1 << 1)
#define MPU_CTRL_PRIVDEFENA   (1 << 2)

/*============================================================================
 * 内存区域常量
 *============================================================================*/

/* Flash: 0x08000000, 2MB */
#define FLASH_BASE_ADDR  0x08000000UL
#define FLASH_SIZE_BYTES (2UL * 1024 * 1024)

/* SRAM: 0x20000000, 384KB */
#define SRAM_BASE_ADDR   0x20000000UL
#define SRAM_SIZE_BYTES  (384UL * 1024)

/* 外设: 0x40000000, 512MB (region) */
#define PERIPH_BASE_ADDR 0x40000000UL

/*============================================================================
 * 初始化和使能
 *============================================================================*/

/*============================================================================
 * P1-2: address_space 池 — mapping policy 载体(每用户任务 1:1)
 *============================================================================*/
#if MPU_ENABLE
static address_space_t aspace_pool[KERNEL_MAX_TASKS];

address_space_t *mpu_aspace_acquire(void) {
    for (uint32_t i = 0; i < KERNEL_MAX_TASKS; i++) {
        if (!aspace_pool[i].in_use) {
            memset(&aspace_pool[i], 0, sizeof(aspace_pool[i]));
            aspace_pool[i].in_use = 1U;
            for (uint32_t r = 0; r < MPU_REGION_COUNT; r++) {
                aspace_pool[i].slot_owner[r] = -1;
            }
            return &aspace_pool[i];
        }
    }
    return NULL;
}

void mpu_aspace_release_task(tcb_t *tcb) {
    if (tcb == NULL || tcb->aspace == NULL) {
        return;
    }
    address_space_t *as = tcb->aspace;
    tcb->aspace = NULL;
    for (uint32_t i = 0; i < KERNEL_MAX_TASKS; i++) {
        if (&aspace_pool[i] == as) {
            aspace_pool[i].in_use = 0U;
            return;
        }
    }
}

/*============================================================================
 * P1-3: 动态映射 —— 软表 + 硬件槽 LRU 驻留缓存
 *============================================================================*/

/* 驻留:encode 进镜像 + 更新槽归属/LRU。不直接写硬件 —— 硬件在 SVC/
 * PendSV 返回路径统一从镜像重载;fault 换入路径额外立即写本核。 */
static void aspace_slot_program(address_space_t *as, int slot, int map_idx) {
    mpu_map_t *m = &as->maps[map_idx];
    mpu_region_encode((uint32_t)slot, (uint32_t)m->base, m->size,
                      RASR_ENABLE | m->attr,
                      &as->regions[slot][0], &as->regions[slot][1]);
    m->slot = (int8_t)slot;
    m->lru_seq = ++as->lru_tick;
    as->slot_owner[slot] = (int8_t)map_idx;
}

static void aspace_slot_evict(address_space_t *as, int slot) {
    int8_t owner = as->slot_owner[slot];
    if (owner >= 0 && owner < MPU_MAP_MAX) {
        as->maps[owner].slot = -1;
    }
    as->slot_owner[slot] = -1;
    as->regions[slot][0] = 0;
    as->regions[slot][1] = 0;
}

/* 运行时槽 = 3..MPU_REGION_COUNT-1(0/1/2 为静态布局) */
static int runtime_slot_first(void) { return 3; }

static int aspace_free_slot(address_space_t *as) {
    for (int r = runtime_slot_first(); r < MPU_REGION_COUNT; r++) {
        if (as->slot_owner[r] < 0) {
            return r;
        }
    }
    return -1;
}

static int aspace_lru_victim_slot(address_space_t *as) {
    int victim = -1;
    uint32_t oldest = 0;
    for (int r = runtime_slot_first(); r < MPU_REGION_COUNT; r++) {
        int8_t owner = as->slot_owner[r];
        if (owner < 0) {
            return r;  /* 空槽优先 */
        }
        if (victim < 0 || as->maps[owner].lru_seq < oldest) {
            victim = r;
            oldest = as->maps[owner].lru_seq;
        }
    }
    return victim;
}

int mpu_map_add(tcb_t *task, uintptr_t base, uint32_t size, uint32_t attr) {
    if (task == NULL || task->aspace == NULL) {
        return -(int)KERN_ERR_STATE;
    }
    if (size == 0U) {
        return -(int)KERN_ERR_PARAM;
    }
    address_space_t *as = task->aspace;

    for (uint32_t i = 0; i < MPU_MAP_MAX; i++) {
        if (as->maps[i].in_use && as->maps[i].base == base) {
            return -(int)KERN_ERR_BUSY;
        }
    }
    int mi = -1;
    for (uint32_t i = 0; i < MPU_MAP_MAX; i++) {
        if (!as->maps[i].in_use) {
            mi = (int)i;
            break;
        }
    }
    if (mi < 0) {
        return -(int)KERN_ERR_RESOURCE;  /* 只有软表满才拒映射 */
    }

    as->maps[mi].base = base;
    as->maps[mi].size = size;
    as->maps[mi].attr = attr;
    as->maps[mi].slot = -1;
    as->maps[mi].lru_seq = 0;
    as->maps[mi].in_use = 1;

    int slot = aspace_free_slot(as);
    if (slot >= 0) {
        aspace_slot_program(as, slot, mi);
    }
    return mi;
}

kern_err_t mpu_map_remove(tcb_t *task, uintptr_t base) {
    if (task == NULL || task->aspace == NULL) {
        return KERN_ERR_STATE;
    }
    address_space_t *as = task->aspace;
    for (uint32_t i = 0; i < MPU_MAP_MAX; i++) {
        if (as->maps[i].in_use && as->maps[i].base == base) {
            if (as->maps[i].slot >= 0) {
                aspace_slot_evict(as, as->maps[i].slot);
            }
            memset(&as->maps[i], 0, sizeof(as->maps[i]));
            return KERN_OK;
        }
    }
    return KERN_ERR_NOEXIST;
}

int mpu_map_slot_of(tcb_t *task, uintptr_t base) {
    if (task == NULL || task->aspace == NULL) {
        return -1;
    }
    address_space_t *as = task->aspace;
    for (uint32_t i = 0; i < MPU_MAP_MAX; i++) {
        if (as->maps[i].in_use && as->maps[i].base == base) {
            return as->maps[i].slot;
        }
    }
    return -1;
}

/* 单槽硬件装载(fault 换入用):本核立即生效,禁-置-启序防瞬时错覆盖 */
static void mpu_slot_hw_load(uint32_t slot, uint32_t rbar, uint32_t rlar) {
#if BOARD_MPU_ARMV8
    uint32_t saved_ctrl = MPU_CTRL;
    __asm volatile("dsb" ::: "memory");
    MPU_CTRL = 0;
    __asm volatile("dsb; isb" ::: "memory");
#endif
    MPU_RNR = slot;
    MPU_RBAR = rbar;
#if BOARD_MPU_ARMV8
    MPU_RLAR = rlar;
    __asm volatile("dsb; isb" ::: "memory");
    MPU_CTRL = saved_ctrl;
    __asm volatile("isb" ::: "memory");
#else
    MPU_RASR = rlar;
    __asm volatile("dsb; isb" ::: "memory");
#endif
}

int mpu_map_demand_load(tcb_t *task, uint32_t fault_addr) {
    if (task == NULL || task->aspace == NULL) {
        return -1;
    }
    address_space_t *as = task->aspace;

    int mi = -1;
    for (uint32_t i = 0; i < MPU_MAP_MAX; i++) {
        if (as->maps[i].in_use &&
            fault_addr >= (uint32_t)as->maps[i].base &&
            fault_addr < (uint32_t)as->maps[i].base + as->maps[i].size) {
            mi = (int)i;
            break;
        }
    }
    if (mi < 0) {
        return -1;  /* 无覆盖映射:走正常故障处理 */
    }
    if (as->maps[mi].slot >= 0) {
        return 0;   /* 已驻留仍违例:真实权限/执行故障 */
    }

    int slot = aspace_lru_victim_slot(as);
    if (slot < 0) {
        return -1;  /* 无运行时槽(配置异常),按故障处理 */
    }
    aspace_slot_evict(as, slot);
    aspace_slot_program(as, slot, mi);
    mpu_slot_hw_load((uint32_t)slot, as->regions[slot][0],
                     as->regions[slot][1]);
    return 1;
}
#endif /* MPU_ENABLE */

void mpu_init(void) {
#if MPU_ENABLE
    memset(aspace_pool, 0, sizeof(aspace_pool));
#endif
    /* 确保 MPU 关闭 */
    MPU_CTRL = 0;

#if BOARD_MPU_ARMV8
    /*
     * MAIR 颜色表 — 8 个 attr index。每个 byte 一个 Outer+Inner 内存属性编码,
     * 详见 ARMv8-M ARM (DDI0553) 的 MAIR_ELx 描述。RLAR.attr_idx 选其中之一。
     *
     *   Idx0  0x00  Device-nGnRnE    (强顺序外设)
     *   Idx1  0xFF  Normal WB/WA     (Flash/SRAM 主用,Cacheable)
     *   Idx2  0x44  Normal WT        (Write-Through,暂留)
     *   Idx3  0x40  Normal Non-cacheable
     *   Idx4  0x04  Device-nGnRE     (可缓存外设)
     *   Idx5-7  预留
     */
    MPU_MAIR0 = (0x00UL << 0)   /* idx0 */
              | (0xFFUL << 8)   /* idx1 */
              | (0x44UL << 16)  /* idx2 */
              | (0x40UL << 24); /* idx3 */
    MPU_MAIR1 = (0x04UL << 0);  /* idx4 */
#endif

    /* 使能所有可配置 Fault (MemManage/BusFault/UsageFault) */
    SCB_SHCSR |= SCB_SHCSR_MEMFAULTENA | SCB_SHCSR_BUSFAULTENA | SCB_SHCSR_USGFAULTENA;

    /* 使能 MPU + 背景区域 (特权模式默认全访问) */
    MPU_CTRL = MPU_CTRL_ENABLE | MPU_CTRL_PRIVDEFENA;

    __asm volatile("dsb");
    __asm volatile("isb");
}

/*============================================================================
 * Region 操作
 *============================================================================*/

void mpu_region_set(uint32_t region, uint32_t base, uint32_t size,
                    uint32_t attr) {
    if (region >= 8) return;

    uint32_t rbar = 0;
    uint32_t limit_attr = 0;
    mpu_region_encode(region, base, size, attr, &rbar, &limit_attr);
    MPU_RNR  = region;
    MPU_RBAR = rbar;
#if BOARD_MPU_ARMV8
    MPU_RLAR = limit_attr;
#else
    MPU_RASR = limit_attr;
#endif
    __asm volatile("dsb");
    __asm volatile("isb");
}

void mpu_region_disable(uint32_t region) {
    if (region >= 8) return;

    MPU_RNR  = region;
    MPU_RBAR = 0;
#if BOARD_MPU_ARMV8
    MPU_RLAR = 0;
#else
    MPU_RASR = 0;
#endif
    __asm volatile("dsb");
    __asm volatile("isb");
}

/*============================================================================
 * Region 大小计算
 *============================================================================*/

uint32_t mpu_calc_rasr_size(uint32_t size) {
    if (size == 0) return 0;

    /* SIZE = log2(actual) - 1, 最小值 4 (32 byte) */
    uint32_t log2 = 31;
    while (log2 > 1 && !(size & (1UL << log2))) {
        log2--;
    }

    /* 如果不是精确的 2 的幂，向上取整 */
    if (size & ((1UL << log2) - 1)) {
        log2++;
    }

    if (log2 < 5) log2 = 5;  /* 最小 32 bytes */
    if (log2 > 31) log2 = 31;

    return ((log2 - 1) & 0x1F) << 1;
}

void mpu_region_encode(uint32_t region, uint32_t base, uint32_t size,
                       uint32_t attr, uint32_t *out_rbar,
                       uint32_t *out_limit_attr) {
    if (out_rbar == NULL || out_limit_attr == NULL) {
        return;
    }
    *out_rbar = 0;
    *out_limit_attr = 0;
    if (size < 32U || base + size - 1U < base) return;

#if BOARD_MPU_ARMV8
    (void)region;
    uint32_t ap;
    uint32_t legacy_ap = attr & (0x7UL << 24);
    if (legacy_ap == AP_FULL) {
        ap = 1U; /* privileged RW, unprivileged RW */
    } else if (legacy_ap == AP_PRW_URO) {
        ap = 3U; /* privileged RO, unprivileged RO */
    } else if (legacy_ap == AP_PRO) {
        ap = 2U; /* privileged RO, unprivileged none */
    } else {
        ap = 0U; /* privileged RW, unprivileged none */
    }

    uint32_t xn = (attr & XN_ENABLE) != 0U ? 1U : 0U;
    uint32_t shareability = 2U; /* outer-shareable */
    uint32_t attr_index = (attr & ATTR_NORMAL_WBWA) != 0U ? 1U : 0U;
    uint32_t limit = base + size - 1U;

    *out_rbar = (base & ~0x1FU) | (shareability << 3) | (ap << 1) | xn;
    *out_limit_attr = (limit & ~0x1FU) | (attr_index << 1) | 1U;
#else
    *out_rbar = base | RBAR_VALID | region;
    *out_limit_attr = attr | mpu_calc_rasr_size(size);
#endif
}

void mpu_stack_region_encode(uint32_t region, uint32_t base, uint32_t size,
                             uint32_t *out_rbar,
                             uint32_t *out_limit_attr) {
#if BOARD_MPU_ARMV8
    /*
     * PMSAv8 + M33: 栈溢出由 PSPLIM 兜底(tcb->sp_limit 装载到 PSPLIM,触栈底
     * 即 MemManage fault)。这里 MPU 区只负责 RW + XN 的常规栈权限,不再缩小
     * 32 字节做下溢守卫 — PSPLIM 比 MPU 守卫更精确(0x1F 误差 vs 32B)且不耗
     * 额外区。任务栈底之下若需更强保护,加一个独立的 Device+XN 区盖在 base-32。
     */
    mpu_region_encode(region, base, size,
                      AP_FULL | ATTR_NORMAL_WBWA | XN_ENABLE,
                      out_rbar, out_limit_attr);
#else
    if (out_rbar != NULL) {
        *out_rbar = base | RBAR_VALID | region;
    }
    if (out_limit_attr != NULL) {
        *out_limit_attr = mpu_stack_guard_rasr(base, size, 0x01U);
    }
#endif
}

int mpu_region_allows_read(uint32_t rbar, uint32_t limit_attr) {
    (void)rbar;
    if ((limit_attr & RASR_ENABLE) == 0U) return 0;
#if BOARD_MPU_ARMV8
    uint32_t ap = (rbar >> 1) & 0x3U;
    return ap == 1U || ap == 3U;
#else
    uint32_t ap = limit_attr & (0x7UL << 24);
    return ap == AP_PRW_URO || ap == AP_FULL;
#endif
}

int mpu_region_allows_write(uint32_t rbar, uint32_t limit_attr) {
    (void)rbar;
    if ((limit_attr & RASR_ENABLE) == 0U) return 0;
#if BOARD_MPU_ARMV8
    return ((rbar >> 1) & 0x3U) == 1U;
#else
    return (limit_attr & (0x7UL << 24)) == AP_FULL;
#endif
}

int mpu_region_is_execute_never(uint32_t rbar, uint32_t limit_attr) {
    (void)rbar;
    (void)limit_attr;
#if BOARD_MPU_ARMV8
    return (rbar & 1U) != 0U;
#else
    return (limit_attr & XN_ENABLE) != 0U;
#endif
}

int mpu_region_get_bounds(uint32_t rbar, uint32_t limit_attr,
                          uintptr_t *base, uint32_t *size) {
    if (base == NULL || size == NULL ||
        (limit_attr & RASR_ENABLE) == 0U) {
        return 0;
    }
#if BOARD_MPU_ARMV8
    uintptr_t start = rbar & ~(uintptr_t)0x1FU;
    uintptr_t end = (limit_attr & ~(uintptr_t)0x1FU) | 0x1FU;
    if (end < start) return 0;
    *base = start;
    *size = (uint32_t)(end - start + 1U);
    return 1;
#else
    uint32_t size_field = (limit_attr >> 1) & 0x1FU;
    if (size_field < 4U) return 0;
    *size = 1UL << (size_field + 1U);
    *base = rbar & ~(uintptr_t)0x1FU;
    return 1;
#endif
}

/*============================================================================
 * 栈溢出守卫
 *============================================================================*/

uint32_t mpu_stack_guard_rasr(uint32_t base, uint32_t size,
                              uint32_t subregion_disable) {
    (void)base;  /* RBAR 由调用者单独配置 */
    uint32_t rasr_size = mpu_calc_rasr_size(size);
    uint32_t srd = (subregion_disable & 0xFF) << RASR_SRD_SHIFT;
    return RASR_ENABLE | AP_FULL | ATTR_NORMAL_WBWA | srd | rasr_size;
}

/*============================================================================
 * 加载任务 MPU regions
 *============================================================================*/

void mpu_load_task_regions(tcb_t *tcb) {
    if (tcb == NULL) return;

#if BOARD_MPU_ARMV8
    /*
     * PMSAv8 programs a region with separate RBAR/RLAR writes.  Updating live
     * regions can transiently combine a new base with an old enabled limit,
     * which may cover the executing flash code with the wrong XN/AP bits.
     * This function only runs from privileged Handler mode, so suspend the MPU
     * for the batch and restore its exact previous control state before return.
     */
    uint32_t saved_ctrl = MPU_CTRL;
    __asm volatile("dsb" ::: "memory");
    MPU_CTRL = 0;
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");
#endif

    if ((tcb->attrs & TASK_ATTR_USER) && tcb->aspace != NULL) {
        /* 用户任务: 加载 address_space 中的 MPU regions (P1-2) */
        for (uint32_t i = 0; i < MPU_REGION_COUNT; i++) {
            MPU_RNR  = i;
            MPU_RBAR = tcb->aspace->regions[i][0];
#if BOARD_MPU_ARMV8
            MPU_RLAR = tcb->aspace->regions[i][1];
#else
            MPU_RASR = tcb->aspace->regions[i][1];
#endif
        }
    } else {
        /* 内核任务(或防御:无 aspace 的用户任务): 清除全部区 */
        for (uint32_t i = 0; i < MPU_REGION_COUNT; i++) {
            MPU_RNR  = i;
            MPU_RBAR = 0;
#if BOARD_MPU_ARMV8
            MPU_RLAR = 0;
#else
            MPU_RASR = 0;
#endif
        }
    }

    __asm volatile("dsb" ::: "memory");
#if BOARD_MPU_ARMV8
    MPU_CTRL = saved_ctrl;
    __asm volatile("dsb" ::: "memory");
#endif
    __asm volatile("isb" ::: "memory");
}

/*============================================================================
 * 默认映射 (空闲任务运行时无保护)
 * ============================================================================*/

void mpu_enable_default_map(void) {
    /* 禁用所有用户 region，仅保留背景区域 */
    for (uint32_t i = 0; i < 8; i++) {
        mpu_region_disable(i);
    }
}

#else

void mpu_init(void) {
}

void mpu_load_task_regions(tcb_t *tcb) {
    (void)tcb;
}

#endif /* MPU_ENABLE */
