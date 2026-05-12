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

    void *ptr = kmem_resolve_cap(cap, CAP_WRITE);
    TEST_ASSERT_NOT_NULL(ptr, "kmem cap resolves with write");
    TEST_ASSERT_EQ(outstanding + 1, mem_get_outstanding_allocs(),
                   "cap memory increments outstanding");

    kern_err_t err = kmem_free_cap(cap);
    TEST_ASSERT_EQ(KERN_OK, err, "kmem_free_cap OK");
    TEST_ASSERT_EQ(outstanding, mem_get_outstanding_allocs(),
                   "cap cleanup frees memory");
}
#endif

static void test_mem_module(void) {
    test_kmalloc_diagnostics();
    test_kmalloc_oom_stats();
    test_mempool_diagnostics();
#if CAP_ENABLE
    test_kmem_cap_binding();
#endif
}

TEST_MODULE_REGISTER(mem, test_mem_module);

#endif /* MEM_DYNAMIC && TEST_ENABLE */
