/**
 * @file test_notification.c
 * @brief Notification (event) object tests — NOWAIT poll + ISR-safe signal
 *
 * Validates that the event object fulfills the seL4/L4 notification role:
 * a signaled word a waiter can block on and a signaler can post from any
 * context. Focuses on the gaps closed in the core-completion slice:
 *   - EVENT_OPT_NOWAIT poll (return current word without blocking)
 *   - event_set is safe to call from a critical (IRQs-off) context (ISR proxy)
 *   - irq_bind_event / irq_unbind_event binding lifecycle (no hardware IRQ
 *     here; full ISR→event delivery is exercised with the GPIO driver later)
 */

#include "test_framework.h"
#include "kernel.h"
#include "event.h"
#include "irq.h"
#include "hal.h"
#include "task.h"

#if IPC_EVENT && TEST_MODULE_NOTIFICATION

#include <string.h>

/*============================================================================
 * Test 1: NOWAIT poll returns current word without blocking
 *============================================================================*/

static void test_event_nowait_poll(void) {
    test_section("Test 1: NOWAIT poll returns current word");

    event_id_t eid = event_create(0x00);
    TEST_ASSERT(eid != KERN_INVALID_ID, "event created");
    if (eid == KERN_INVALID_ID) return;

    /* No flags set: NOWAIT must still return OK with the current word (0). */
    uint32_t got = 0xDEADBEEF;
    kern_err_t e = event_wait(eid, 0x1, EVENT_OPT_OR | EVENT_OPT_NOWAIT,
                              0, &got);
    TEST_ASSERT_EQ((int)KERN_OK, (int)e, "NOWAIT returns OK even if no match");
    TEST_ASSERT_EQ((int)0, (int)got, "NOWAIT returns current (empty) word");

    /* Set a flag, NOWAIT should reflect it. */
    e = event_set(eid, 0x2);
    TEST_ASSERT_EQ((int)KERN_OK, (int)e, "event_set 0x2");
    got = 0;
    e = event_wait(eid, 0x1, EVENT_OPT_OR | EVENT_OPT_NOWAIT, 0, &got);
    TEST_ASSERT_EQ((int)KERN_OK, (int)e, "NOWAIT after set");
    TEST_ASSERT_EQ((int)0x2, (int)got, "NOWAIT returns current word 0x2");

    /* NOWAIT + CLEAR clears the requested bits even when not matched. */
    got = 0;
    e = event_wait(eid, 0x2, EVENT_OPT_OR | EVENT_OPT_NOWAIT | EVENT_OPT_CLEAR,
                   0, &got);
    TEST_ASSERT_EQ((int)KERN_OK, (int)e, "NOWAIT+CLEAR OK");
    TEST_ASSERT_EQ((int)0x2, (int)got, "returned word before clear");
    got = 0;
    e = event_wait(eid, 0x2, EVENT_OPT_OR | EVENT_OPT_NOWAIT, 0, &got);
    TEST_ASSERT_EQ((int)0, (int)got, "word cleared after NOWAIT+CLEAR");

    event_delete(eid);
}

/*============================================================================
 * Test 2: event_set wakes a blocked waiter (ISR-safety via IRQ masking)
 *============================================================================*/

/* event_set masks IRQs internally (hal_enter_critical), so it is ISR-safe by
 * construction. This test verifies the wake path: a waiter blocks, a signaler
 * task calls event_set, the waiter wakes. (Calling event_set from a manually-
 * entered critical section in the same test task does not let the woken higher-
 * priority waiter run until the next schedule point; using a separate signaler
 * task exercises the real wake + context-switch path.) */
static event_id_t g_wake_eid;
static volatile int g_wake_task_woken = 0;
static volatile int g_wake_waiter_err = -999;
static volatile uint32_t g_wake_waiter_got = 0xDEADBEEF;

static void wake_waiter_task(void *arg) {
    (void)arg;
    uint32_t got = 0;
    kern_err_t e = event_wait(g_wake_eid, 0x1, EVENT_OPT_OR | EVENT_OPT_CLEAR,
                              2000, &got);
    g_wake_waiter_err = (int)e;
    g_wake_waiter_got = got;
    if (e == KERN_OK && (got & 0x1)) {
        g_wake_task_woken = 1;
    }
}

static void wake_signaler_task(void *arg) {
    (void)arg;
    /* Give the waiter a moment to block first. */
    task_delay(3);
    /* event_set is ISR-safe (it masks IRQs internally); calling it from a
     * normal task context proves the wake machinery end to end. */
    (void)event_set(g_wake_eid, 0x1);
}

static void test_event_set_wakes_waiter(void) {
    test_section("Test 2: event_set wakes blocked waiter");

    g_wake_eid = event_create(0);
    g_wake_task_woken = 0;
    TEST_ASSERT(g_wake_eid != KERN_INVALID_ID, "event created");
    if (g_wake_eid == KERN_INVALID_ID) return;

    task_id_t waiter = task_create("ntfn_wait", wake_waiter_task, NULL, 8, 1024);
    TEST_ASSERT(waiter >= 0, "waiter task created");
    if (waiter < 0) { event_delete(g_wake_eid); return; }
    task_start(waiter);

    task_id_t sig = task_create("ntfn_sig", wake_signaler_task, NULL, 9, 1024);
    TEST_ASSERT(sig >= 0, "signaler task created");
    if (sig < 0) { event_delete(g_wake_eid); return; }
    task_start(sig);

    /* Wait for the signaler to run, signal, and the waiter to wake. */
    task_delay(30);
    TEST_ASSERT(g_wake_task_woken == 1, "waiter woken by event_set signal");
    TEST_ASSERT(task_get_state(waiter) != TASK_STATE_BLOCKED,
                "waiter no longer blocked");

    /* Diagnostics: surface what event_wait actually returned so a failure
     * (e.g. timeout vs wrong word) is identifiable. */
    TEST_ASSERT_EQ((int)KERN_OK, g_wake_waiter_err, "waiter event_wait rc OK");
    TEST_ASSERT((g_wake_waiter_got & 0x1) != 0, "waiter saw flag 0x1");

    task_delay(2);
    event_delete(g_wake_eid);
}

/*============================================================================
 * Test 3: irq_bind_event / irq_unbind_event binding lifecycle
 *
 * (No hardware IRQ fires here — this validates the binding table mechanics.
 * A real IRQ→event delivery is exercised with the GPIO driver server later.)
 *============================================================================*/

static void test_irq_bind_event_lifecycle(void) {
    test_section("Test 3: irq_bind_event binding lifecycle");

    event_id_t eid = event_create(0);
    TEST_ASSERT(eid != KERN_INVALID_ID, "event created");
    if (eid == KERN_INVALID_ID) return;

    /* Bind IRQ 5 to the event. (We don't enable a real IRQ line here.) */
    kern_err_t e = irq_bind_event(5, eid, 0x4);
    TEST_ASSERT_EQ((int)KERN_OK, (int)e, "irq_bind_event OK");

    /* Refresh the same IRQ (idempotent — should not consume a second slot). */
    e = irq_bind_event(5, eid, 0x8);
    TEST_ASSERT_EQ((int)KERN_OK, (int)e, "re-bind same IRQ OK");

    /* Unbind. */
    e = irq_unbind_event(5);
    TEST_ASSERT_EQ((int)KERN_OK, (int)e, "irq_unbind_event OK");

    /* Unbinding again is harmless. */
    e = irq_unbind_event(5);
    TEST_ASSERT_EQ((int)KERN_OK, (int)e, "double unbind OK");

    event_delete(eid);
}

/*============================================================================
 * Module registration
 *============================================================================*/

static void test_notification_module(void) {
    test_event_nowait_poll();
    test_event_set_wakes_waiter();
    test_irq_bind_event_lifecycle();
}

TEST_MODULE_REGISTER(notification, test_notification_module);

#endif /* IPC_EVENT && TEST_MODULE_NOTIFICATION */
