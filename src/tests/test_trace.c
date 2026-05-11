/**
 * @file test_trace.c
 * @brief Trace buffer 测试模块
 *
 * 测试内容:
 * 1. trace_record 写入 → trace_get_entry 读取一致性
 * 2. buffer 满了以后 wrap-around 正确
 * 3. trace_clear 清零
 * 4. trace_get_count 在写入后递增
 * 5. 所有 8 种事件类型可记录
 * 6. trace_filter 过滤功能
 */

#include "test_framework.h"
#include "kernel.h"
#include "task.h"

#if TRACE_ENABLE && TEST_ENABLE

#include "trace.h"

/*============================================================================
 * Test 1: trace_record 写入 → trace_get_entry 读取一致性
 *============================================================================*/

static void test_trace_write_read(void) {
    test_section("Test 1: trace write → read consistency");

    trace_clear();
    TEST_ASSERT_EQ(0, trace_get_count(), "count=0 after clear");

    trace_record(TRACE_TASK_SWITCH, 5, 0xABCD);
    TEST_ASSERT_EQ(1, trace_get_count(), "count=1 after record");

    const trace_entry_t *e = trace_get_entry(0);
    TEST_ASSERT_NOT_NULL(e, "entry not null");
    if (!e) return;

    TEST_ASSERT_EQ(TRACE_TASK_SWITCH, e->event, "event match");
    TEST_ASSERT_EQ(5, e->task_id, "task_id match");
    TEST_ASSERT_EQ(0xABCD, e->data, "data match");
    TEST_ASSERT(e->tick > 0, "tick > 0");
}

/*============================================================================
 * Test 2: buffer 满了以后 wrap-around 正确
 *============================================================================*/

static void test_trace_wraparound(void) {
    test_section("Test 2: trace buffer wrap-around");

    trace_clear();

    /* Fill buffer to capacity */
    for (int i = 0; i < TRACE_BUFFER_SIZE; i++) {
        trace_record(TRACE_TASK_SWITCH, (uint8_t)i, (uint16_t)i);
    }
    TEST_ASSERT_EQ(TRACE_BUFFER_SIZE, trace_get_count(), "count at capacity");

    /* Add one more → oldest drops, count stays at capacity */
    trace_record(TRACE_ISR_ENTER, 0xFF, 0xDEAD);
    TEST_ASSERT_EQ(TRACE_BUFFER_SIZE, trace_get_count(), "count stays at capacity after wrap");

    /* First entry should now be index 1 from original (index 0 was evicted) */
    const trace_entry_t *e0 = trace_get_entry(0);
    TEST_ASSERT_NOT_NULL(e0, "entry 0 not null after wrap");
    if (!e0) return;
    TEST_ASSERT_EQ(1, e0->task_id, "first entry shifted: task_id=1");
    TEST_ASSERT_EQ(1, e0->data, "first entry shifted: data=1");

    /* The last entry (index=count-1) should be the wrapped one */
    const trace_entry_t *elast = trace_get_entry(TRACE_BUFFER_SIZE - 1);
    TEST_ASSERT_NOT_NULL(elast, "last entry not null");
    if (!elast) return;
    TEST_ASSERT_EQ(TRACE_ISR_ENTER, elast->event, "last entry is the wrap entry");
    TEST_ASSERT_EQ(0xFF, elast->task_id, "last entry task_id=0xFF");
    TEST_ASSERT_EQ(0xDEAD, elast->data, "last entry data=0xDEAD");
}

/*============================================================================
 * Test 3: trace_clear 清零
 *============================================================================*/

static void test_trace_clear(void) {
    test_section("Test 3: trace clear");

    /* Write a few entries */
    trace_record(TRACE_SYSCALL, 1, 100);
    trace_record(TRACE_IPC_SEND, 2, 200);
    trace_record(TRACE_IPC_RECV, 3, 300);
    TEST_ASSERT(trace_get_count() >= 3, "entries exist before clear");

    trace_clear();
    TEST_ASSERT_EQ(0, trace_get_count(), "count=0 after clear");
    TEST_ASSERT_NULL(trace_get_entry(0), "get_entry(0) returns NULL after clear");
}

/*============================================================================
 * Test 4: trace_get_count 在写入后递增
 *============================================================================*/

static void test_trace_count_increments(void) {
    test_section("Test 4: count increments");

    trace_clear();
    uint16_t prev = 0;

    for (int i = 0; i < 10; i++) {
        trace_record(TRACE_TASK_SWITCH, 0, 0);
        uint16_t cur = trace_get_count();
        TEST_ASSERT(cur > prev || (prev >= TRACE_BUFFER_SIZE && cur == TRACE_BUFFER_SIZE),
                    "count monotonic or capped");
        prev = cur;
    }
}

/*============================================================================
 * Test 5: 所有 8 种事件类型可记录
 *============================================================================*/

static void test_trace_all_event_types(void) {
    test_section("Test 5: all 8 event types");

    trace_clear();

    const uint8_t events[] = {
        TRACE_TASK_SWITCH, TRACE_ISR_ENTER, TRACE_ISR_EXIT, TRACE_SYSCALL,
        TRACE_IPC_SEND, TRACE_IPC_RECV, TRACE_BH_SCHEDULE, TRACE_FAULT
    };

    for (int i = 0; i < 8; i++) {
        trace_record(events[i], (uint8_t)i, (uint16_t)(i * 100));
    }

    TEST_ASSERT_EQ(8, trace_get_count(), "8 entries recorded");

    for (int i = 0; i < 8; i++) {
        const trace_entry_t *e = trace_get_entry((uint16_t)i);
        TEST_ASSERT_NOT_NULL(e, "entry exists");
        if (e) {
            TEST_ASSERT_EQ(events[i], e->event, "event type match");
            TEST_ASSERT_EQ(i, e->task_id, "task_id match");
            TEST_ASSERT_EQ((uint16_t)(i * 100), e->data, "data match");
        }
    }
}

/*============================================================================
 * Test 6: trace_filter 过滤功能
 *============================================================================*/

static uint16_t filter_match_count;
static uint8_t  filter_event_seen;

static void filter_callback(const trace_entry_t *e, void *ctx) {
    (void)ctx;
    filter_match_count++;
    filter_event_seen = e->event;
    /* verify tick is non-zero for recorded entries */
    if (e->tick == 0) {
        filter_match_count = 0xFFFF; /* signal error via debug */
    }
}

static void test_trace_filter(void) {
    test_section("Test 6: trace filter");

    trace_clear();

    /* Mix of events */
    trace_record(TRACE_FAULT, 1, 10);
    trace_record(TRACE_SYSCALL, 2, 20);
    trace_record(TRACE_FAULT, 3, 30);
    trace_record(TRACE_TASK_SWITCH, 4, 40);
    trace_record(TRACE_FAULT, 5, 50);

    filter_match_count = 0;
    filter_event_seen = 0;
    uint16_t matched = trace_filter(TRACE_FAULT, filter_callback, NULL);

    TEST_ASSERT_EQ(3, matched, "filter matched 3 FAULT events");
    TEST_ASSERT_EQ(3, filter_match_count, "callback called 3 times");
    TEST_ASSERT_EQ(TRACE_FAULT, filter_event_seen, "callback saw FAULT events");

    /* Filter for event that doesn't exist */
    matched = trace_filter(TRACE_ISR_ENTER, filter_callback, NULL);
    TEST_ASSERT_EQ(0, matched, "filter matched 0 ISR_ENTER events");
}

/*============================================================================
 * Module registration
 *============================================================================*/

static void test_trace_module(void) {
    test_trace_write_read();
    test_trace_wraparound();
    test_trace_clear();
    test_trace_count_increments();
    test_trace_all_event_types();
    test_trace_filter();
}

TEST_MODULE_REGISTER(trace, test_trace_module);

#endif /* TRACE_ENABLE && TEST_ENABLE */
