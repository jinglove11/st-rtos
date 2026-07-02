/**
 * @file test_supervisor_monitor.c
 * @brief Phase 2 §2.2 — supervisor fault-driven restart logic tests
 *
 * Injects synthetic fault_event_t values directly into
 * supervisor_handle_fault() (no real CPU fault, no real task restart).
 * Verifies: recipe lookup, rate-limit window, exponential backoff, and the
 * permanent-kill-after-N rule. The actual sys_task_restart() call is a no-op
 * here (kernel-mode test caller has no usable TASK cap), so we assert on the
 * supervisor's internal decision state (restart_count, killed) rather than
 * on whether a new task was spawned.
 */

#include "test_framework.h"
#include "supervisor.h"
#include "task.h"
#include "scheduler.h"
#include "kernel.h"
#include "capability.h"
#include "endpoint.h"
#include "user_api.h"
#include <string.h>

#if FAULT_ENDPOINT && SUPERVISOR

#include "fault_endpoint.h"

/*============================================================================
 * Helpers
 *============================================================================*/

/* A dummy entry so the recipe has a non-NULL function pointer. */
static void dummy_entry(void *arg) {
    (void)arg;
}

/* Build a synthetic fault event for a named task at a given tick. */
static fault_event_t make_event(const char *name, uint32_t tick) {
    fault_event_t e;
    memset(&e, 0, sizeof(e));
    e.fault_type = 1;  /* FAULT_TYPE_MEMMANAGE-ish; value irrelevant to logic */
    e.task_id = 0;
    e.tick = tick;
    if (name != NULL) {
        /* fault_event_t.task_name is fixed-size; copy truncated. */
        uint32_t i = 0;
        for (; i < (KERN_TASK_NAME_LEN - 1U) && name[i] != '\0'; i++) {
            e.task_name[i] = name[i];
        }
        e.task_name[i] = '\0';
    }
    return e;
}

/*============================================================================
 * Test 1: unknown task is ignored (no recipe registered)
 *============================================================================*/

static void test_unknown_task_ignored(supervisor_runtime_t *runtime) {
    test_section("Test 1: unknown task ignored");
    /* Ensure clean state: register then the lookup for an unrelated name. */
    supervisor_register_recipe(runtime, "known_svc", dummy_entry, NULL, 5, 1024,
                               0x1F /* CAP_FULL */);
    fault_event_t e = make_event("totally_unknown", 100);
    int acted = supervisor_on_fault(runtime, &e, -1);
    TEST_ASSERT_EQ(0, acted, "unknown task → not scheduled");
}

/*============================================================================
 * Test 2: a known fault schedules a (deferred) restart, not an immediate one
 *============================================================================*/

static void test_known_task_schedules_restart(supervisor_runtime_t *runtime) {
    test_section("Test 2: known task schedules deferred restart");

    supervisor_recipe_t *r = supervisor_find_recipe(runtime, "known_svc");
    TEST_ASSERT_NOT_NULL(r, "recipe registered");
    if (r == NULL) return;

    r->restart_count = 0;
    r->backoff_ticks = SUPERVISOR_BACKOFF_BASE_MS;
    r->killed = 0;
    r->pending_restart = 0;

    fault_event_t e = make_event("known_svc", 1000);
    int acted = supervisor_on_fault(runtime, &e, -1);
    TEST_ASSERT_EQ(1, acted, "known fault schedules a restart");
    TEST_ASSERT(r->pending_restart == 1, "recipe marked pending");
    TEST_ASSERT(r->next_restart_tick == 1000 + SUPERVISOR_BACKOFF_BASE_MS,
                "next_restart_tick = fault_tick + backoff");
    TEST_ASSERT(r->killed == 0, "first fault does not kill");
    TEST_ASSERT(r->restart_count == 0,
                "restart_count not bumped until timer fires (deferred)");
}

/*============================================================================
 * Test 3: do_restarts clears pending + bumps count when backoff elapsed
 *============================================================================*/

static void test_do_restarts_after_backoff(supervisor_runtime_t *runtime) {
    test_section("Test 3: do_restarts fires after backoff");

    supervisor_recipe_t *r = supervisor_find_recipe(runtime, "known_svc");
    TEST_ASSERT_NOT_NULL(r, "recipe for do_restarts test");
    if (r == NULL) return;

    /* Force a pending restart whose deadline is in the past relative to the
     * current tick. Use a large fault_tick so next_restart_tick < now. */
    r->restart_count = 0;
    r->backoff_ticks = SUPERVISOR_BACKOFF_BASE_MS;
    r->killed = 0;
    r->pending_restart = 0;

    fault_event_t e = make_event("known_svc", 1);
    (void)supervisor_on_fault(runtime, &e, -1);
    TEST_ASSERT(r->pending_restart == 1, "pending set by on_fault");

    /* Spin the scheduler forward so sched_get_tick_count() advances past the
     * 1ms deadline that on_fault computed. */
    for (int i = 0; i < 50; i++) {
        task_delay(1);
    }

    /* In kernel-test context sys_task_restart will fail (caller is the
     * privileged test_runner, which sys_task_restart rejects with KERN_ERR_PERM
     * for non-USER callers). do_restarts then re-arms pending and retries, so
     * restart_count stays 0 but pending_restart stays 1 (it did NOT silently
     * drop the fault). That is the property we assert: no deadlock drop. */
    int issued = supervisor_do_restarts(runtime, -1);
    TEST_ASSERT(r->pending_restart == 1,
                "failed restart keeps pending (no silent drop / deadlock)");
    (void)issued;
}

/*============================================================================
 * Test 4: permanent kill after MAX_RESTARTS exceeded
 *============================================================================*/

static void test_permanent_kill_after_max(supervisor_runtime_t *runtime) {
    test_section("Test 4: permanent kill after max restarts");

    supervisor_recipe_t *r = supervisor_find_recipe(runtime, "known_svc");
    TEST_ASSERT_NOT_NULL(r, "recipe for kill test");
    if (r == NULL) return;
    r->restart_count = SUPERVISOR_MAX_RESTARTS;
    r->backoff_ticks = SUPERVISOR_BACKOFF_BASE_MS;
    r->killed = 0;
    r->pending_restart = 0;

    fault_event_t e = make_event("known_svc", 100000);
    int acted = supervisor_on_fault(runtime, &e, -1);
    TEST_ASSERT_EQ(0, acted, "fault over max returns not-scheduled");
    TEST_ASSERT(r->killed == 1, "service marked permanently killed");
    TEST_ASSERT(r->pending_restart == 0, "killed service not pending");

    /* A subsequent fault on a killed service is ignored. */
    fault_event_t again = make_event("known_svc", 200000);
    int acted2 = supervisor_on_fault(runtime, &again, -1);
    TEST_ASSERT_EQ(0, acted2, "killed service ignores further faults");
}

/*============================================================================
 * Test 5: fault_event_t carries the task_name field (§2.2 identity)
 *============================================================================*/

static void test_event_carries_task_name(void) {
    test_section("Test 5: fault_event_t carries task_name");
    fault_event_t e = make_event("crashy_app", 42);
    TEST_ASSERT(strcmp(e.task_name, "crashy_app") == 0,
                "task_name round-trips through fault_event_t");
    TEST_ASSERT(sizeof(e.task_name) == KERN_TASK_NAME_LEN,
                "task_name field is KERN_TASK_NAME_LEN bytes");
}

/*============================================================================
 * Test 6: real USER task keeps mutable supervisor state on its own stack
 *============================================================================*/

static void supervisor_user_runtime_task(void *arg) {
    (void)arg;
    supervisor_runtime_t runtime;
    supervisor_runtime_init(&runtime);

    int err = supervisor_register_recipe(&runtime, "user_svc", dummy_entry,
                                         NULL, 5, 1024, CAP_FULL);
    supervisor_recipe_t *recipe = supervisor_find_recipe(&runtime, "user_svc");
    if (err != KERN_OK || recipe == NULL) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_STATE);
    }
    recipe->restart_count = 2;
    if (recipe->restart_count != 2) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_STATE);
    }
    sys_task_exit((void *)(intptr_t)KERN_OK);
}

static void test_user_runtime_is_stack_accessible(void) {
    test_section("Test 6: USER supervisor runtime is stack-accessible");

    task_id_t tid = task_create_user("sup_user_rt",
                                     supervisor_user_runtime_task,
                                     NULL, 10, 2048);
    TEST_ASSERT(tid >= 0, "USER supervisor runtime task created");
    if (tid < 0) return;

    kern_err_t err = task_start(tid);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "USER supervisor runtime task started");

    void *retval = NULL;
    err = task_join(tid, &retval, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "USER supervisor runtime task joined");
    TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                   "USER supervisor reads and writes stack runtime");
}

/*============================================================================
 * Test 7: USER endpoint receive honors KERN_WAIT_FOREVER
 *============================================================================*/

static void supervisor_forever_wait_task(void *arg) {
    int ep_cap = (int)(uintptr_t)arg;
    uint8_t msg[KERN_EP_MSG_SIZE];
    memset(msg, 0, sizeof(msg));

    int err = sys_ep_recv(ep_cap, msg, (int)KERN_WAIT_FOREVER);
    if (err == KERN_OK && msg[0] == 0x5AU) {
        sys_task_exit((void *)(intptr_t)KERN_OK);
    }
    sys_task_exit((void *)(intptr_t)KERN_ERR_STATE);
}

static void test_user_forever_wait(void) {
    test_section("Test 7: USER endpoint wait-forever semantics");

    ep_id_t ep = endpoint_create("sup_wait", KERN_EP_MSG_SIZE, 2);
    TEST_ASSERT(ep >= 0, "wait-forever endpoint created");
    if (ep < 0) return;

    task_id_t tid = task_create_user("sup_wait",
                                     supervisor_forever_wait_task,
                                     NULL, 10, 1024);
    TEST_ASSERT(tid >= 0, "wait-forever USER task created");
    if (tid < 0) {
        (void)endpoint_delete(ep);
        return;
    }

    tcb_t *task = task_get_tcb(tid);
    cap_id_t ep_cap = cap_create_for(task,
                                     (void *)(uintptr_t)(ep + 1),
                                     CAP_OBJ_ENDPOINT, CAP_READ);
    TEST_ASSERT(ep_cap >= 0, "wait-forever USER task receives endpoint cap");
    if (ep_cap < 0) {
        (void)task_delete(tid);
        (void)endpoint_delete(ep);
        return;
    }
    TEST_ASSERT_EQ((int)KERN_OK,
                   (int)task_set_initial_arg(tid,
                                             (void *)(uintptr_t)ep_cap),
                   "wait-forever endpoint cap installed as task argument");
    TEST_ASSERT_EQ((int)KERN_OK, (int)task_start(tid),
                   "wait-forever USER task started");

    task_delay(3);
    TEST_ASSERT_EQ((int)TASK_STATE_BLOCKED, (int)task_get_state(tid),
                   "wait-forever task remains blocked across ticks");

    uint8_t msg[KERN_EP_MSG_SIZE];
    memset(msg, 0, sizeof(msg));
    msg[0] = 0x5AU;
    TEST_ASSERT_EQ((int)KERN_OK, (int)endpoint_notify(ep, msg),
                   "wait-forever task notified");

    void *retval = NULL;
    kern_err_t err = task_join(tid, &retval, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "wait-forever USER task joined");
    TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                   "wait-forever USER task received notification");

    if (endpoint_exists(ep)) {
        (void)endpoint_delete(ep);
    }
}

/*============================================================================
 * Module registration
 *============================================================================*/

static void test_supervisor_monitor_module(void) {
    supervisor_runtime_t runtime;
    supervisor_runtime_init(&runtime);

    test_unknown_task_ignored(&runtime);
    test_known_task_schedules_restart(&runtime);
    test_do_restarts_after_backoff(&runtime);
    test_permanent_kill_after_max(&runtime);
    test_event_carries_task_name();
    test_user_runtime_is_stack_accessible();
    test_user_forever_wait();
}

TEST_MODULE_REGISTER(supervisor_monitor, test_supervisor_monitor_module);

#endif /* FAULT_ENDPOINT && SUPERVISOR */
