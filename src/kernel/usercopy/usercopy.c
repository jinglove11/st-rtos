/**
 * @file usercopy.c
 * @brief User pointer validation and copy helpers for syscall boundary
 */

#include "usercopy.h"
#include "scheduler.h"
#include "task.h"
#include "mpu.h"
#include "board_config.h"
#include <string.h>

#if SYSCALL_ENABLE

#define USERCOPY_FLASH_BASE BOARD_FLASH_BASE
#define USERCOPY_FLASH_SIZE BOARD_FLASH_SIZE
#define USERCOPY_SRAM_BASE  BOARD_SRAM_BASE
#define USERCOPY_SRAM_SIZE  BOARD_SRAM_SIZE
#define USERCOPY_MPU_REGION_MAX 8

static int range_in_region(uintptr_t ptr, uint32_t len,
                           uintptr_t base, uint32_t size) {
    if (len == 0) {
        return 1;
    }
    if (ptr < base) {
        return 0;
    }

    uintptr_t end = ptr + len - 1;
    if (end < ptr) {
        return 0;
    }

    return end < (base + size);
}

#if MPU_ENABLE && !BOARD_MPU_ARMV8
static int mpu_subregions_allow(uintptr_t start, uintptr_t end,
                                uintptr_t base, uint32_t size,
                                uint32_t rasr) {
    uint32_t srd = (rasr >> RASR_SRD_SHIFT) & 0xFF;
    if (srd == 0 || size < 256) {
        return 1;
    }

    uint32_t sub_size = size / 8;
    for (uint32_t sub = 0; sub < 8; sub++) {
        if ((srd & (1U << sub)) == 0) {
            continue;
        }

        uintptr_t sub_start = base + (sub * sub_size);
        uintptr_t sub_end = sub_start + sub_size - 1;
        if (start <= sub_end && end >= sub_start) {
            return 0;
        }
    }

    return 1;
}
#endif

static int user_range_allowed_by_mpu(const void *ptr, uint32_t len,
                                     uint32_t access) {
#if MPU_ENABLE
    tcb_t *cur = sched_get_current();
    if (cur == NULL || (cur->attrs & TASK_ATTR_USER) == 0) {
        return 0;
    }

    uintptr_t p = (uintptr_t)ptr;
    if (p == 0 && len > 0) {
        return 0;
    }
    if (len == 0) {
        return 1;
    }

    uintptr_t end = p + len - 1;
    if (end < p) {
        return 0;
    }

    for (uint32_t i = 0; i < USERCOPY_MPU_REGION_MAX; i++) {
        uint32_t rbar = cur->mpu_regions[i][0];
        uint32_t rasr = cur->mpu_regions[i][1];
        if ((rasr & RASR_ENABLE) == 0) {
            continue;
        }

        if ((access & USER_ACCESS_WRITE) != 0) {
            if (!mpu_region_allows_write(rbar, rasr)) {
                continue;
            }
        } else if (!mpu_region_allows_read(rbar, rasr)) {
            continue;
        }

        uint32_t size;
        uintptr_t base;
        if (!mpu_region_get_bounds(rbar, rasr, &base, &size)) {
            continue;
        }
        if (range_in_region(p, len, base, size)
#if !BOARD_MPU_ARMV8
            && mpu_subregions_allow(p, end, base, size, rasr)
#endif
        ) {
            return 1;
        }
    }
#else
    (void)ptr;
    (void)len;
    (void)access;
#endif
    return 0;
}

int user_access_ok(const void *ptr, uint32_t len, uint32_t access) {
    uintptr_t p = (uintptr_t)ptr;

    if (access == 0 || (access & ~(USER_ACCESS_READ | USER_ACCESS_WRITE)) != 0) {
        return 0;
    }
    if (p == 0 && len > 0) {
        return 0;
    }
    if (len == 0) {
        return 1;
    }

    if (user_range_allowed_by_mpu(ptr, len, access)) {
        return 1;
    }

    tcb_t *cur = sched_get_current();
    if (cur && (cur->attrs & TASK_ATTR_USER) != 0) {
#if !MPU_ENABLE
        uintptr_t stack_base = (uintptr_t)cur->stack_base;
        if (range_in_region(p, len, stack_base, cur->stack_size)) {
            return 1;
        }
        if ((access & USER_ACCESS_WRITE) == 0 &&
            range_in_region(p, len, USERCOPY_FLASH_BASE,
                            USERCOPY_FLASH_SIZE)) {
            return 1;
        }
#endif
        return 0;
    }

    if ((access & USER_ACCESS_WRITE) != 0) {
        return range_in_region(p, len, USERCOPY_SRAM_BASE, USERCOPY_SRAM_SIZE);
    }

    return range_in_region(p, len, USERCOPY_FLASH_BASE, USERCOPY_FLASH_SIZE) ||
           range_in_region(p, len, USERCOPY_SRAM_BASE, USERCOPY_SRAM_SIZE);
}

kern_err_t copy_from_user(void *dst, const void *user_src, uint32_t len) {
    if (len == 0) {
        return KERN_OK;
    }
    if (dst == NULL || !user_access_ok(user_src, len, USER_ACCESS_READ)) {
        return KERN_ERR_PARAM;
    }

    memcpy(dst, user_src, len);
    return KERN_OK;
}

kern_err_t copy_to_user(void *user_dst, const void *src, uint32_t len) {
    if (len == 0) {
        return KERN_OK;
    }
    if (src == NULL || !user_access_ok(user_dst, len, USER_ACCESS_WRITE)) {
        return KERN_ERR_PARAM;
    }

    memcpy(user_dst, src, len);
    return KERN_OK;
}

kern_err_t strncpy_from_user(char *dst, const char *user_src, uint32_t max_len) {
    if (dst == NULL || max_len == 0) {
        return KERN_ERR_PARAM;
    }

    if (user_src == NULL) {
        dst[0] = '\0';
        return KERN_OK;
    }

    for (uint32_t i = 0; i < max_len; i++) {
        if (!user_access_ok(user_src + i, 1, USER_ACCESS_READ)) {
            dst[0] = '\0';
            return KERN_ERR_PARAM;
        }

        dst[i] = user_src[i];
        if (dst[i] == '\0') {
            return KERN_OK;
        }
    }

    dst[max_len - 1] = '\0';
    return KERN_ERR_PARAM;
}

#endif /* SYSCALL_ENABLE */
