/**
 * @file test_mem.c
 * @brief Memory diagnostics and capability binding tests
 */

#include "test_framework.h"
#include "mem.h"
#include "mempool.h"
#include "trace.h"
#include "stats.h"
#include "capability.h"
#include "task.h"
#include "mpu.h"

#if MEM_DYNAMIC && TEST_ENABLE

#if TRACE_ENABLE && KERN_TASK_STATS
static void mem_trace_count_cb(const trace_entry_t *entry, void *ctx) {
    (void)entry;
    (void)ctx;
}
#endif

static void test_kmalloc_diagnostics(void) {
    test_section("Test 1: kmalloc diagnostics");

#if TRACE_ENABLE && KERN_TASK_STATS
    trace_clear();
    stats_clear_events();
#endif

    mem_stats_t before = mem_get_stats();
    void *ptr = kmalloc(32);
    TEST_ASSERT_NOT_NULL(ptr, "kmalloc returns memory");
    TEST_ASSERT_EQ(before.outstanding_allocs + 1,
                   mem_get_outstanding_allocs(),
                   "outstanding increments");

    kfree(ptr);
    TEST_ASSERT_EQ(before.outstanding_allocs,
                   mem_get_outstanding_allocs(),
                   "outstanding decrements");

#if TRACE_ENABLE && KERN_TASK_STATS
    uint16_t mem_events = trace_filter(TRACE_MEM, mem_trace_count_cb, NULL);
    TEST_ASSERT(mem_events >= 2, "mem trace records alloc/free");
    TEST_ASSERT(stats_get_event_count(STATS_SUBSYS_MEM, STATS_COUNTER_OK) >= 2,
                "mem stats records ok events");
#endif
}

static void test_kmalloc_oom_stats(void) {
    test_section("Test 2: kmalloc OOM stats");

#if TRACE_ENABLE && KERN_TASK_STATS
    trace_clear();
    stats_clear_events();
#endif

    uint32_t fail_before = mem_get_fail_count();
    void *ptr = kmalloc(mem_get_free() + 1024);
    TEST_ASSERT_NULL(ptr, "oversized kmalloc fails");
    TEST_ASSERT_EQ(fail_before + 1, mem_get_fail_count(),
                   "fail count increments");

#if KERN_TASK_STATS
    TEST_ASSERT(stats_get_event_count(STATS_SUBSYS_MEM,
                                      STATS_COUNTER_QUEUE_FULL) >= 1,
                "OOM stats records queue_full bucket");
#endif
}

static void test_mempool_diagnostics(void) {
    test_section("Test 3: mempool diagnostics");

    pool_id_t pool = mempool_create(16, 2);
    TEST_ASSERT(pool >= 0, "mempool create");

    void *a = mempool_alloc(pool);
    void *b = mempool_alloc(pool);
    void *c = mempool_alloc(pool);
    TEST_ASSERT_NOT_NULL(a, "mempool alloc A");
    TEST_ASSERT_NOT_NULL(b, "mempool alloc B");
    TEST_ASSERT_NULL(c, "mempool exhausted returns NULL");

    mempool_free(pool, a);
    mempool_free(pool, b);
    TEST_ASSERT_EQ(2, (int)mempool_get_free_count(pool),
                   "mempool free count restored");

    mempool_delete(pool);
}

#if CAP_ENABLE
static void test_kmem_cap_binding(void) {
    test_section("Test 4: memory capability binding");

    uint32_t outstanding = mem_get_outstanding_allocs();
    cap_id_t cap = kmem_alloc_cap(24, CAP_READ | CAP_WRITE | CAP_MANAGE);
    TEST_ASSERT(cap >= 0, "kmem_alloc_cap returns cap");
    uint8_t type = UINT8_MAX;
    TEST_ASSERT_EQ(KERN_OK, cap_get_type(cap, &type),
                   "memory capability type query succeeds");
    TEST_ASSERT_EQ(CAP_OBJ_FRAME, type,
                   "legacy kmem allocation returns a Frame cap");

    void *ptr = kmem_resolve_cap(cap, CAP_WRITE);
    TEST_ASSERT_NOT_NULL(ptr, "kmem cap resolves with write");
    TEST_ASSERT_EQ(outstanding + 1, mem_get_outstanding_allocs(),
                   "Frame backing increments outstanding once");

    kobject_header_t *frame_object =
        (kobject_header_t *)cap_resolve(cap, CAP_OBJ_FRAME, CAP_MANAGE);
    TEST_ASSERT_NOT_NULL(frame_object, "Frame object metadata resolves");
    uint32_t frame_generation =
        frame_object != NULL ? frame_object->generation : 0U;

    void *base = NULL;
    size_t size = 0;
    kern_err_t err = kmem_get_bounds(cap, &base, &size);
    TEST_ASSERT_EQ(KERN_OK, err, "kmem_get_bounds OK");
    TEST_ASSERT_EQ((uintptr_t)ptr, (uintptr_t)base,
                   "kmem bounds base matches resolved ptr");
    TEST_ASSERT_EQ((int)24, (int)size, "kmem bounds size recorded");

    err = kmem_get_bounds(cap, NULL, &size);
    TEST_ASSERT_EQ(KERN_ERR_PARAM, err, "kmem_get_bounds rejects NULL base");

    void *range = NULL;
    err = kmem_get_range(cap, CAP_WRITE, 8, 8, &range);
    TEST_ASSERT_EQ(KERN_OK, err, "kmem_get_range inside bounds OK");
    TEST_ASSERT_EQ((uintptr_t)((uint8_t *)ptr + 8), (uintptr_t)range,
                   "kmem_get_range returns offset pointer");

    err = kmem_get_range(cap, CAP_WRITE, 20, 8, &range);
    TEST_ASSERT_EQ(KERN_ERR_PARAM, err, "kmem_get_range rejects overflow");

    err = kmem_get_range(cap, CAP_WRITE, 24, 1, &range);
    TEST_ASSERT_EQ(KERN_ERR_PARAM, err, "kmem_get_range rejects end overflow");

    err = kmem_get_range(cap, CAP_WRITE, 0, 0, &range);
    TEST_ASSERT_EQ(KERN_ERR_PARAM, err, "kmem_get_range rejects zero length");

    err = kmem_get_range(cap, CAP_WRITE, 0, 4, NULL);
    TEST_ASSERT_EQ(KERN_ERR_PARAM, err, "kmem_get_range rejects NULL output");

    err = kmem_free_cap(cap);
    TEST_ASSERT_EQ(KERN_OK, err, "kmem_free_cap OK");
    if (frame_object != NULL) {
        TEST_ASSERT(frame_object->generation != frame_generation,
                    "Frame release advances object generation");
    }
    TEST_ASSERT_EQ(outstanding, mem_get_outstanding_allocs(),
                   "cap cleanup frees memory");

    cap_id_t replacement =
        kmem_alloc_cap(24, CAP_READ | CAP_WRITE | CAP_MANAGE);
    TEST_ASSERT(replacement >= 0, "released Frame slot is reusable");
    kobject_header_t *replacement_object =
        (kobject_header_t *)cap_resolve(replacement, CAP_OBJ_FRAME,
                                        CAP_MANAGE);
    TEST_ASSERT_EQ((uintptr_t)frame_object, (uintptr_t)replacement_object,
                   "Frame metadata slot persists across reuse");
    if (replacement_object != NULL) {
        TEST_ASSERT(replacement_object->generation != frame_generation,
                    "reused Frame keeps advanced generation");
    }
    TEST_ASSERT_EQ(KERN_OK, kmem_free_cap(replacement),
                   "replacement Frame released");
    TEST_ASSERT_EQ(outstanding, mem_get_outstanding_allocs(),
                   "Frame reuse test restores outstanding allocations");
}

static void test_kmmio_cap_strict_rejection(void) {
    test_section("Test 5: MMIO capability strict rejection");

    cap_id_t cap = KERN_INVALID_ID;
    kern_err_t err = kmmio_create_cap(0x40000000UL, 4, 4, CAP_FULL, NULL);
    TEST_ASSERT_EQ(KERN_ERR_PARAM, err, "kmmio_create_cap rejects NULL out cap");

    err = kmmio_create_cap(0x40000000UL, 0, 4, CAP_FULL, &cap);
    TEST_ASSERT_EQ(KERN_ERR_PARAM, err, "kmmio_create_cap rejects zero size");

    err = kmmio_create_cap(0x40000002UL, 4, 4, CAP_FULL, &cap);
    TEST_ASSERT_EQ(KERN_ERR_PARAM, err, "kmmio_create_cap rejects unaligned base");

    err = kmmio_create_cap(0x40000000UL, 4, 3, CAP_FULL, &cap);
    TEST_ASSERT_EQ(KERN_ERR_PARAM, err, "kmmio_create_cap rejects invalid width");

    err = kmmio_create_cap(0x08000000UL, 4, 4, CAP_FULL, &cap);
    TEST_ASSERT_EQ(KERN_ERR_PERM, err, "kmmio_create_cap rejects Flash");

    err = kmmio_create_cap(0x20000000UL, 4, 4, CAP_FULL, &cap);
    TEST_ASSERT_EQ(KERN_ERR_PERM, err, "kmmio_create_cap rejects SRAM");

    void *heap = kmalloc(16);
    TEST_ASSERT_NOT_NULL(heap, "heap pointer allocated for MMIO rejection");
    if (heap != NULL) {
        err = kmmio_create_cap((uintptr_t)heap, 4, 4, CAP_FULL, &cap);
        TEST_ASSERT_EQ(KERN_ERR_PERM, err, "kmmio_create_cap rejects heap memory");
        kfree(heap);
    }

    err = kmmio_create_cap(0x5ffffffcUL, 8, 4, CAP_FULL, &cap);
    TEST_ASSERT_EQ(KERN_ERR_PERM, err, "kmmio_create_cap rejects peripheral overflow");

    err = kmmio_get_bounds(KERN_INVALID_ID, &(uintptr_t){0}, &(size_t){0},
                           &(uint8_t){0});
    TEST_ASSERT_EQ(KERN_ERR_CAP, err, "kmmio_get_bounds rejects invalid cap");
    err = kmmio_delete_cap(KERN_INVALID_ID);
    TEST_ASSERT_EQ(KERN_ERR_CAP, err, "kmmio_delete_cap rejects invalid cap");
}

static void test_kshm_cap_lifecycle(void) {
    test_section("Test 6: shared memory capability lifecycle");

    uint32_t outstanding = mem_get_outstanding_allocs();
    cap_id_t cap = kshm_create_cap(64,
                                   CAP_READ | CAP_WRITE |
                                   CAP_MANAGE | CAP_GRANT);
    TEST_ASSERT(cap >= 0, "kshm_create_cap returns cap");
    TEST_ASSERT_EQ((int)(outstanding + 1),
                   (int)mem_get_outstanding_allocs(),
                   "kshm create allocates backing");

    void *base = NULL;
    size_t size = 0;
    kern_err_t err = kshm_get_bounds(cap, &base, &size);
    TEST_ASSERT_EQ(KERN_OK, err, "kshm_get_bounds OK");
    TEST_ASSERT_NOT_NULL(base, "kshm base is non-NULL");
    TEST_ASSERT_EQ(64, (int)size, "kshm size recorded");

    void *range = NULL;
    err = kshm_get_range(cap, CAP_WRITE, 16, 8, &range);
    TEST_ASSERT_EQ(KERN_OK, err, "kshm_get_range inside bounds OK");
    TEST_ASSERT_EQ((uintptr_t)((uint8_t *)base + 16), (uintptr_t)range,
                   "kshm_get_range returns offset pointer");

    err = kshm_get_range(cap, CAP_WRITE, 60, 8, &range);
    TEST_ASSERT_EQ(KERN_ERR_PARAM, err, "kshm_get_range rejects overflow");

    err = kshm_get_range(cap, CAP_WRITE, 0, 0, &range);
    TEST_ASSERT_EQ(KERN_ERR_PARAM, err, "kshm_get_range rejects zero length");

    cap_id_t child = cap_derive(cap, CAP_READ);
    TEST_ASSERT(child >= 0, "kshm child cap derived");
    err = kshm_get_range(child, CAP_READ, 0, 4, &range);
    TEST_ASSERT_EQ(KERN_OK, err, "kshm child read range OK");
    err = kshm_get_range(child, CAP_WRITE, 0, 4, &range);
    TEST_ASSERT_EQ(KERN_ERR_CAP, err, "kshm child write rejected");

    err = kshm_delete_cap(cap);
    TEST_ASSERT_EQ(KERN_OK, err, "kshm_delete_cap cascades parent");
    err = kshm_get_bounds(child, &base, &size);
    TEST_ASSERT_EQ(KERN_ERR_CAP, err, "kshm child invalid after parent revoke");
    TEST_ASSERT_EQ((int)outstanding,
                   (int)mem_get_outstanding_allocs(),
                   "kshm cleanup restores outstanding");

    cap_id_t bad = kshm_create_cap(0, CAP_FULL);
    TEST_ASSERT(bad < 0, "kshm_create_cap rejects zero size");
    err = kshm_delete_cap(KERN_INVALID_ID);
    TEST_ASSERT_EQ(KERN_ERR_CAP, err, "kshm_delete_cap rejects invalid cap");
}

#if MPU_ENABLE
#define TEST_MPU_AP_MASK (0x7UL << 24)

static void shm_map_dummy_task(void *arg) {
    (void)arg;
}

static void test_kshm_map_to_task(void) {
    test_section("Test 7: shared memory task mapping");

    uint32_t outstanding = mem_get_outstanding_allocs();
    task_id_t tid = task_create_user("shmmap", shm_map_dummy_task, NULL, 10, 512);
    TEST_ASSERT(tid >= 0, "user task created for shm map");

    tcb_t *tcb = task_get_tcb(tid);
    TEST_ASSERT_NOT_NULL(tcb, "user task TCB resolved");

    cap_id_t root = kshm_create_aligned_cap(256,
                                            CAP_READ | CAP_WRITE |
                                            CAP_MANAGE | CAP_TRANSFER |
                                            CAP_GRANT);
    TEST_ASSERT(root >= 0, "aligned kshm root cap created");

    cap_id_t task_cap = KERN_INVALID_ID;
    if (tcb != NULL && root >= 0) {
        task_cap = cap_copy_to(NULL, root, tcb, CAP_READ | CAP_WRITE);
    }
    TEST_ASSERT(task_cap >= 0, "shm cap copied into task CSpace");

    void *addr = NULL;
    kern_err_t err = kshm_map_to_task(tcb, task_cap, CAP_READ, &addr);
    TEST_ASSERT_EQ(KERN_OK, err, "read-only shm map OK");
    TEST_ASSERT_NOT_NULL(addr, "mapped address returned");

    int mapped_slot = -1;
    uint8_t mapped_region = 0;
    for (int i = 0; i < TASK_SHM_MAP_MAX; i++) {
        if (tcb != NULL && tcb->shm_maps[i].in_use &&
            tcb->shm_maps[i].cap == task_cap) {
            mapped_slot = i;
            mapped_region = tcb->shm_maps[i].region;
            break;
        }
    }
    TEST_ASSERT(mapped_slot >= 0, "task records shm mapping");
    TEST_ASSERT(mapped_region >= 3 && mapped_region < 8,
                "shm map uses dynamic MPU region");
    if (mapped_slot >= 0) {
        uint32_t rasr = tcb->mpu_regions[mapped_region][1];
        TEST_ASSERT((rasr & RASR_ENABLE) != 0, "shm MPU region enabled");
        uint32_t rbar = tcb->mpu_regions[mapped_region][0];
        TEST_ASSERT(mpu_region_allows_read(rbar, rasr),
                    "read-only shm map allows user read access");
        TEST_ASSERT(!mpu_region_allows_write(rbar, rasr),
                    "read-only shm map rejects user write access");
        TEST_ASSERT(mpu_region_is_execute_never(rbar, rasr),
                    "shm map is execute-never");
    }

    err = kshm_map_to_task(tcb, task_cap, CAP_READ, &addr);
    TEST_ASSERT_EQ(KERN_ERR_BUSY, err, "duplicate shm map rejected");

    err = kshm_unmap_from_task(tcb, task_cap);
    TEST_ASSERT_EQ(KERN_OK, err, "shm unmap OK");
    if (mapped_slot >= 0) {
        TEST_ASSERT((tcb->mpu_regions[mapped_region][1] & RASR_ENABLE) == 0,
                    "shm MPU region disabled after unmap");
        TEST_ASSERT(tcb->shm_maps[mapped_slot].in_use == 0,
                    "shm mapping metadata cleared");
    }

    err = kshm_map_to_task(tcb, task_cap, CAP_READ | CAP_WRITE, &addr);
    TEST_ASSERT_EQ(KERN_OK, err, "read-write shm map OK");
    mapped_slot = -1;
    mapped_region = 0;
    for (int i = 0; i < TASK_SHM_MAP_MAX; i++) {
        if (tcb != NULL && tcb->shm_maps[i].in_use &&
            tcb->shm_maps[i].cap == task_cap) {
            mapped_slot = i;
            mapped_region = tcb->shm_maps[i].region;
            break;
        }
    }
    TEST_ASSERT(mapped_slot >= 0, "read-write map metadata recorded");
    if (mapped_slot >= 0) {
        uint32_t rbar = tcb->mpu_regions[mapped_region][0];
        uint32_t rasr = tcb->mpu_regions[mapped_region][1];
        TEST_ASSERT(mpu_region_allows_write(rbar, rasr),
                    "read-write shm map uses user write access");
    }

    err = task_delete(tid);
    TEST_ASSERT_EQ(KERN_OK, err, "task delete clears shm maps");
    tcb = NULL;

    err = kshm_delete_cap(root);
    TEST_ASSERT_EQ(KERN_OK, err, "root shm cap deleted after task cleanup");
    TEST_ASSERT_EQ((int)outstanding,
                   (int)mem_get_outstanding_allocs(),
                   "shm mapping cleanup restores outstanding");
}

static void test_kshm_revoke_unmaps_task(void) {
    test_section("Test 8: shared memory revoke unmaps task");

    uint32_t outstanding = mem_get_outstanding_allocs();
    task_id_t tid = task_create_user("shmrev", shm_map_dummy_task, NULL, 10, 512);
    TEST_ASSERT(tid >= 0, "user task created for shm revoke");

    tcb_t *tcb = task_get_tcb(tid);
    TEST_ASSERT_NOT_NULL(tcb, "user task TCB resolved for revoke");

    cap_id_t root = kshm_create_aligned_cap(256,
                                            CAP_READ | CAP_WRITE |
                                            CAP_MANAGE | CAP_TRANSFER |
                                            CAP_GRANT);
    TEST_ASSERT(root >= 0, "aligned kshm root cap created for revoke");

    cap_id_t task_cap = KERN_INVALID_ID;
    if (tcb != NULL && root >= 0) {
        task_cap = cap_copy_to(NULL, root, tcb, CAP_READ | CAP_WRITE);
    }
    TEST_ASSERT(task_cap >= 0, "shm cap copied before revoke");

    void *addr = NULL;
    kern_err_t err = kshm_map_to_task(tcb, task_cap,
                                      CAP_READ | CAP_WRITE, &addr);
    TEST_ASSERT_EQ(KERN_OK, err, "shm map before revoke OK");

    uint8_t mapped_region = 0;
    int saw_mapping = 0;
    for (int i = 0; i < TASK_SHM_MAP_MAX; i++) {
        if (tcb != NULL && tcb->shm_maps[i].in_use &&
            tcb->shm_maps[i].cap == task_cap) {
            mapped_region = tcb->shm_maps[i].region;
            saw_mapping = 1;
            break;
        }
    }
    TEST_ASSERT(saw_mapping == 1, "shm mapping exists before revoke");

    err = cap_revoke(root);
    TEST_ASSERT_EQ(KERN_OK, err, "root revoke OK");
    if (tcb != NULL && saw_mapping) {
        int still_mapped = 0;
        for (int i = 0; i < TASK_SHM_MAP_MAX; i++) {
            if (tcb->shm_maps[i].in_use && tcb->shm_maps[i].cap == task_cap) {
                still_mapped = 1;
            }
        }
        TEST_ASSERT(still_mapped == 0, "revoke clears task shm mapping");
        TEST_ASSERT((tcb->mpu_regions[mapped_region][1] & RASR_ENABLE) == 0,
                    "revoke disables mapped MPU region");
    }

    void *base = NULL;
    size_t size = 0;
    err = kshm_get_bounds(task_cap, &base, &size);
    TEST_ASSERT_EQ(KERN_ERR_CAP, err, "revoked task shm cap invalid");

    if (tid >= 0) {
        err = task_delete(tid);
        TEST_ASSERT_EQ(KERN_OK, err, "task delete after shm revoke OK");
    }
    TEST_ASSERT_EQ((int)outstanding,
                   (int)mem_get_outstanding_allocs(),
                   "shm revoke cleanup restores outstanding");
}

static void test_kframe_revoke_unmaps_task(void) {
    test_section("Test 9: Frame revoke unmaps task");

    uint32_t outstanding = mem_get_outstanding_allocs();
    task_id_t tid = task_create_user("framerev", shm_map_dummy_task,
                                     NULL, 10, 512);
    TEST_ASSERT(tid >= 0, "user task created for Frame revoke");

    tcb_t *tcb = task_get_tcb(tid);
    TEST_ASSERT_NOT_NULL(tcb, "user task TCB resolved for Frame revoke");

    cap_id_t root = kframe_create_cap(
        256, CAP_READ | CAP_WRITE | CAP_MANAGE | CAP_TRANSFER | CAP_GRANT);
    TEST_ASSERT(root >= 0, "Frame root cap created for revoke");

    cap_id_t task_cap = KERN_INVALID_ID;
    if (tcb != NULL && root >= 0) {
        task_cap = cap_copy_to(NULL, root, tcb, CAP_READ | CAP_WRITE);
    }
    TEST_ASSERT(task_cap >= 0, "Frame cap copied before revoke");

    void *addr = NULL;
    kern_err_t err = kmem_map_to_task(tcb, task_cap,
                                      CAP_READ | CAP_WRITE, &addr);
    TEST_ASSERT_EQ(KERN_OK, err, "Frame map before revoke OK");
    TEST_ASSERT_NOT_NULL(addr, "Frame map returns backing address");

    uint8_t mapped_region = 0U;
    int saw_mapping = 0;
    for (int i = 0; i < TASK_SHM_MAP_MAX; i++) {
        if (tcb != NULL && tcb->shm_maps[i].in_use &&
            tcb->shm_maps[i].cap == task_cap) {
            mapped_region = tcb->shm_maps[i].region;
            saw_mapping = 1;
            break;
        }
    }
    TEST_ASSERT_EQ(1, saw_mapping, "Frame mapping exists before revoke");

    err = cap_revoke(root);
    TEST_ASSERT_EQ(KERN_OK, err, "Frame root revoke OK");
    if (tcb != NULL && saw_mapping) {
        int still_mapped = 0;
        for (int i = 0; i < TASK_SHM_MAP_MAX; i++) {
            if (tcb->shm_maps[i].in_use &&
                tcb->shm_maps[i].cap == task_cap) {
                still_mapped = 1;
            }
        }
        TEST_ASSERT_EQ(0, still_mapped,
                       "Frame revoke clears task mapping");
        TEST_ASSERT((tcb->mpu_regions[mapped_region][1] & RASR_ENABLE) == 0U,
                    "Frame revoke disables mapped MPU region");
    }

    void *base = NULL;
    size_t size = 0U;
    err = kmem_get_bounds(task_cap, &base, &size);
    TEST_ASSERT_EQ(KERN_ERR_CAP, err,
                   "revoked task Frame cap is invalid");

    if (tid >= 0) {
        err = task_delete(tid);
        TEST_ASSERT_EQ(KERN_OK, err,
                       "task delete after Frame revoke OK");
    }
    TEST_ASSERT_EQ((int)outstanding,
                   (int)mem_get_outstanding_allocs(),
                   "Frame revoke cleanup restores outstanding");
}
#endif
#endif

static void test_mem_module(void) {
    test_kmalloc_diagnostics();
    test_kmalloc_oom_stats();
    test_mempool_diagnostics();
#if CAP_ENABLE
    test_kmem_cap_binding();
    test_kmmio_cap_strict_rejection();
    test_kshm_cap_lifecycle();
#if MPU_ENABLE
    test_kshm_map_to_task();
    test_kshm_revoke_unmaps_task();
    test_kframe_revoke_unmaps_task();
#endif
#endif
}

TEST_K_MODULE(mem, test_mem_module);

#endif /* MEM_DYNAMIC && TEST_ENABLE */
