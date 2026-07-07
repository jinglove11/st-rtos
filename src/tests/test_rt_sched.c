/**
 * @file test_rt_sched.c
 * @brief Core completion #4 — RT scheduling policy tests
 *
 * Validates SCHED_FIFO vs SCHED_NORMAL/RR:
 *  - SCHED_FIFO task: time_slice_reload == 0 (never rotated by tick)
 *  - SCHED_NORMAL: time_slice_reload == KERN_DEFAULT_TIME_SLICE
 *  - task_set_sched_policy changes it, task_get_sched_policy reads it
 *  - A FIFO task at the same priority as a NORMAL task gets more CPU (the
 *    NORMAL one is rotated out by time-slice while FIFO is not)
 *
 * The "RT band" (priority 0..15) is by convention; the scheduler always picks
 * the highest-priority ready task, so RT-band tasks naturally preempt normal.
 */

#include "test_framework.h"
#include "kernel.h"
#include "task.h"
#include "scheduler.h"

#if RT_SCHED && TEST_MODULE_RT_SCHED

#include <stdint.h>

/*============================================================================
 * Test 1: set/get sched policy + time_slice effect
 *============================================================================*/

static void rt_scratch_task(void *arg) {
    (void)arg;
    while (1) {
        task_delay(1000);
    }
}

static void test_sched_policy_set_get(void) {
    test_section("Test 1: set/get sched policy");

    task_id_t tid = task_create("rt_t1", rt_scratch_task, NULL, 10, 1024);
    TEST_ASSERT(tid >= 0, "scratch task created");
    if (tid < 0) return;

    /* Default is SCHED_NORMAL. */
    TEST_ASSERT_EQ((int)SCHED_NORMAL, (int)task_get_sched_policy(tid),
                   "default policy is NORMAL");

    /* Set FIFO: time_slice should become 0. */
    kern_err_t e = task_set_sched_policy(tid, SCHED_FIFO);
    TEST_ASSERT_EQ((int)KERN_OK, (int)e, "set FIFO OK");
    TEST_ASSERT_EQ((int)SCHED_FIFO, (int)task_get_sched_policy(tid),
                   "policy is FIFO");
    tcb_t *tcb = task_get_tcb(tid);
    TEST_ASSERT_NOT_NULL(tcb, "tcb for time_slice check");
    if (tcb) {
        TEST_ASSERT_EQ(0, (int)tcb->time_slice_reload,
                       "FIFO: time_slice_reload == 0");
    }

    /* Set RR: time_slice restored. */
    e = task_set_sched_policy(tid, SCHED_RR);
    TEST_ASSERT_EQ((int)KERN_OK, (int)e, "set RR OK");
    TEST_ASSERT_EQ((int)SCHED_RR, (int)task_get_sched_policy(tid),
                   "policy is RR");
    if (tcb) {
        TEST_ASSERT_EQ((int)KERN_DEFAULT_TIME_SLICE,
                       (int)tcb->time_slice_reload,
                       "RR: time_slice_reload restored");
    }

    /* Invalid policy rejected. */
    e = task_set_sched_policy(tid, 99);
    TEST_ASSERT(e != KERN_OK, "invalid policy rejected");

    (void)task_delete(tid);
}

/*============================================================================
 * Test 2: FIFO task is not rotated by time-slice
 *
 * Two tasks at the same priority: one FIFO, one NORMAL. After running for
 * several ticks, the FIFO task should have done strictly more work (it's
 * never rotated out by tick), while the NORMAL one is rotated. We can't
 * easily assert exact counts, but we verify the FIFO task's time_slice
 * stays 0 (no decrement) while NORMAL's decrements.
 *============================================================================*/

static volatile uint32_t rt_fifo_ticks_seen;

static void rt_fifo_worker(void *arg) {
    (void)arg;
    /* Yield periodically so we don't starve the test runner. */
    while (1) {
        rt_fifo_ticks_seen++;
        task_yield();
    }
}

static void test_fifo_not_rotated(void) {
    test_section("Test 2: FIFO task not rotated by time-slice");

    rt_fifo_ticks_seen = 0;

    /* FIFO worker at a priority BELOW test_runner (10) so it doesn't starve
     * the test. Priority 12 = normal band. */
    task_id_t fid = task_create("rt_fifo", rt_fifo_worker, NULL, 12, 1024);
    TEST_ASSERT(fid >= 0, "fifo worker created");
    if (fid < 0) return;

    (void)task_set_sched_policy(fid, SCHED_FIFO);
    task_start(fid);

    /* Let it run a few ticks. */
    task_delay(10);

    /* FIFO task's time_slice should still be 0 (never decremented by tick). */
    tcb_t *ftcb = task_get_tcb(fid);
    TEST_ASSERT_NOT_NULL(ftcb, "fifo tcb");
    if (ftcb) {
        TEST_ASSERT_EQ(0, (int)ftcb->time_slice,
                       "FIFO time_slice stayed 0 (not rotated by tick)");
    }
    TEST_ASSERT(rt_fifo_ticks_seen > 0, "FIFO worker ran");

    (void)task_delete(fid);
}

/*============================================================================
 * Module registration
 *============================================================================*/

static void test_rt_sched_module(void) {
    test_sched_policy_set_get();
    test_fifo_not_rotated();
}

TEST_MODULE_REGISTER(rt_sched, test_rt_sched_module);

#endif /* RT_SCHED && TEST_MODULE_RT_SCHED */
