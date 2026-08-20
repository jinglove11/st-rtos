/**
 * @file test_mpu_aspace.c
 * @brief P1-3 (C3): MPU 软映射表 + 硬件槽 LRU 驻留白盒测试
 *
 * 验证动态分配器核心不变量:
 *   - 硬件运行时槽(3..MPU_REGION_COUNT-1)只是软表的 LRU 驻留缓存
 *   - 槽满后新映射仍成功(未驻留),MemManage 按需换入
 *   - 换入恰好逐出 LRU 最老驻留项;已驻留重入返回 0(真实违例)
 *   - remove 清镜像与槽归属;表满才 RESOURCE
 *
 * 端到端(用户任务真实 fault 换入)归 abi 层,板上回归覆盖。
 */

#include "test_framework.h"
#include "kernel.h"
#include "mpu.h"
#include "task.h"
#include <stdint.h>

#if TEST_ENABLE

#if MPU_ENABLE && MEM_DYNAMIC && (MPU_REGION_COUNT >= 5)

#define RT_SLOTS (MPU_REGION_COUNT - 3)   /* 运行时槽 3..N-1 */

/* 32 字节对齐静态缓冲(RP2350 ARMv8 MPU 粒度) */
static uint8_t ntfn_buf[RT_SLOTS + 2][256] __attribute__((aligned(32)));

/* 永不 start 的占位入口:测试只借用 TCB 的 aspace */
static void aspace_dummy_entry(void *arg) { (void)arg; for (;;) { } }

static tcb_t *aspace_test_user(const char *name) {
    task_id_t tid = task_create_user(name, aspace_dummy_entry, NULL, 15, 512);
    if (tid < 0) {
        return NULL;
    }
    return task_get_tcb(tid);
}

static void aspace_test_release(tcb_t *tcb) {
    if (tcb != NULL) {
        for (uint32_t i = 0; i < MPU_MAP_MAX; i++) {
            if (tcb->aspace != NULL && tcb->aspace->maps[i].in_use) {
                (void)mpu_map_remove(tcb, tcb->aspace->maps[i].base);
            }
        }
        (void)task_delete(tcb->id);
    }
}

/*============================================================================
 * Test 1: 软表容量 > 硬件槽;LRU 换入逐出最老驻留项
 *============================================================================*/

static void test_aspace_lru_demand(void) {
    test_section("Test 1: table exceeds hw slots + LRU demand load");

    tcb_t *tcb = aspace_test_user("aspace_lru");
    TEST_ASSERT(tcb != NULL && tcb->aspace != NULL, "user task with aspace");
    if (tcb == NULL || tcb->aspace == NULL) return;

    /* 填满全部运行时槽:全部立即驻留 */
    for (int i = 0; i < RT_SLOTS; i++) {
        int mid = mpu_map_add(tcb, (uintptr_t)ntfn_buf[i], 256U,
                              AP_FULL | ATTR_NORMAL_WBWA | XN_ENABLE);
        TEST_ASSERT(mid >= 0, "map fills runtime slots");
        if (mid < 0) goto out;
    }
    for (int i = 0; i < RT_SLOTS; i++) {
        int slot = mpu_map_slot_of(tcb, (uintptr_t)ntfn_buf[i]);
        TEST_ASSERT(slot >= 3 && slot < MPU_REGION_COUNT,
                     "early map resident");
    }

    /* 槽已满:新映射仍成功,但未驻留 */
    int mid_x = mpu_map_add(tcb, (uintptr_t)ntfn_buf[RT_SLOTS], 256U,
                            AP_FULL | ATTR_NORMAL_WBWA | XN_ENABLE);
    TEST_ASSERT(mid_x >= 0, "map beyond hw slots succeeds");
    TEST_ASSERT_EQ(-1, mpu_map_slot_of(tcb, (uintptr_t)ntfn_buf[RT_SLOTS]),
                   "overflow map not resident");

    /* 按需换入:驻留 + 恰好逐出 LRU 最老项(buf[0]) */
    TEST_ASSERT_EQ(1, mpu_map_demand_load(tcb,
                     (uint32_t)(uintptr_t)&ntfn_buf[RT_SLOTS][8]),
                   "demand load swaps in");
    TEST_ASSERT(mpu_map_slot_of(tcb, (uintptr_t)ntfn_buf[RT_SLOTS]) >= 3,
                "overflow map now resident");
    int evicted = 0;
    for (int i = 0; i < RT_SLOTS; i++) {
        if (mpu_map_slot_of(tcb, (uintptr_t)ntfn_buf[i]) < 0) {
            evicted++;
        }
    }
    TEST_ASSERT_EQ(1, evicted, "exactly one LRU victim evicted");
    TEST_ASSERT_EQ(-1, mpu_map_slot_of(tcb, (uintptr_t)ntfn_buf[0]),
                   "oldest map is the victim");

    /* 已驻留再换入 → 0(真实违例信号);未覆盖地址 → -1 */
    TEST_ASSERT_EQ(0, mpu_map_demand_load(tcb,
                     (uint32_t)(uintptr_t)&ntfn_buf[RT_SLOTS][8]),
                   "resident demand returns 0");
    TEST_ASSERT_EQ(-1, mpu_map_demand_load(tcb, 0xE0000000U),
                   "uncovered address returns -1");

out:
    aspace_test_release(tcb);
}

/*============================================================================
 * Test 2: remove 清槽/镜像;重复与容量边界
 *============================================================================*/

static void test_aspace_remove_and_limits(void) {
    test_section("Test 2: remove clears slot + table limits");

    tcb_t *tcb = aspace_test_user("aspace_rm");
    TEST_ASSERT(tcb != NULL && tcb->aspace != NULL, "user task with aspace");
    if (tcb == NULL || tcb->aspace == NULL) return;

    uintptr_t b0 = (uintptr_t)ntfn_buf[0];
    int mid = mpu_map_add(tcb, b0, 256U,
                          AP_FULL | ATTR_NORMAL_WBWA | XN_ENABLE);
    TEST_ASSERT(mid >= 0, "map added");
    int slot = mpu_map_slot_of(tcb, b0);
    TEST_ASSERT(slot >= 3, "resident");

    /* remove:驻留槽清零,槽归属归还 */
    TEST_ASSERT_EQ(KERN_OK, mpu_map_remove(tcb, b0), "remove ok");
    TEST_ASSERT_EQ(-1, mpu_map_slot_of(tcb, b0), "no longer mapped");
    if (slot >= 0 && slot < MPU_REGION_COUNT) {
        TEST_ASSERT_EQ(0, tcb->aspace->regions[slot][1] & RASR_ENABLE,
                       "mirror slot disabled");
        TEST_ASSERT((int8_t)-1 == tcb->aspace->slot_owner[slot],
                    "slot ownership returned");
    }
    TEST_ASSERT_EQ(KERN_ERR_NOEXIST, mpu_map_remove(tcb, b0),
                   "double remove NOEXIST");

    /* 同一 base 重复 add → BUSY */
    TEST_ASSERT(mpu_map_add(tcb, b0, 256U,
                            AP_FULL | ATTR_NORMAL_WBWA | XN_ENABLE) >= 0,
                "re-add after remove ok");
    TEST_ASSERT_EQ(-(int)KERN_ERR_BUSY,
                   mpu_map_add(tcb, b0, 256U,
                               AP_FULL | ATTR_NORMAL_WBWA | XN_ENABLE),
                   "duplicate base BUSY");

    /* 表满 → RESOURCE(唯一拒映射条件) */
    for (uint32_t i = 1; i < MPU_MAP_MAX; i++) {
        int r = mpu_map_add(tcb, (uintptr_t)ntfn_buf[1] + 0x100000U * i,
                            256U, AP_FULL | ATTR_NORMAL_WBWA | XN_ENABLE);
        TEST_ASSERT(r >= 0, "table fill");
        if (r < 0) break;
    }
    TEST_ASSERT_EQ(-(int)KERN_ERR_RESOURCE,
                   mpu_map_add(tcb, (uintptr_t)ntfn_buf[2] + 0x2000000U,
                               256U, AP_FULL | ATTR_NORMAL_WBWA | XN_ENABLE),
                   "table full RESOURCE");

    /* 内核任务(无 aspace)→ STATE */
    tcb_t *self = sched_get_current();
    if (self != NULL && self->aspace == NULL) {
        TEST_ASSERT_EQ(-(int)KERN_ERR_STATE,
                       mpu_map_add(self, b0, 256U, AP_FULL),
                       "kernel task (no aspace) rejected");
    }

    aspace_test_release(tcb);
}

static void test_mpu_aspace_module(void) {
    test_aspace_lru_demand();
    test_aspace_remove_and_limits();
}

TEST_K_MODULE(mpu_aspace, test_mpu_aspace_module);

#endif /* MPU_ENABLE && MEM_DYNAMIC && MPU_REGION_COUNT >= 5 */

#endif /* TEST_ENABLE */
