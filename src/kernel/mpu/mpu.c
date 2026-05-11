/**
 * @file mpu.c
 * @brief Cortex-M7 MPU 实现
 */

#include "mpu.h"
#include "hal.h"
#include "kernel_config.h"
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
#define MPU_RASR        (*(volatile uint32_t *)(MPU_BASE + 0x10))

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

void mpu_init(void) {
    /* 确保 MPU 关闭 */
    MPU_CTRL = 0;

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

    MPU_RNR  = region;
    MPU_RBAR = base | RBAR_VALID | region;
    MPU_RASR = attr | mpu_calc_rasr_size(size);
    __asm volatile("dsb");
    __asm volatile("isb");
}

void mpu_region_disable(uint32_t region) {
    if (region >= 8) return;

    MPU_RNR  = region;
    MPU_RBAR = 0;
    MPU_RASR = 0;
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

    if (tcb->attrs & TASK_ATTR_USER) {
        /* 用户任务: 加载 TCB 中的 MPU regions */
        for (uint32_t i = 0; i < 8; i++) {
            MPU_RNR  = i;
            MPU_RBAR = tcb->mpu_regions[i][0];
            MPU_RASR = tcb->mpu_regions[i][1];
        }
    } else {
        /* 内核任务: 禁用所有 user regions，清除残留 */
        for (uint32_t i = 0; i < 8; i++) {
            MPU_RNR  = i;
            MPU_RBAR = 0;
            MPU_RASR = 0;
        }
    }

    __asm volatile("dsb");
    __asm volatile("isb");
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

#endif /* MPU_ENABLE */
