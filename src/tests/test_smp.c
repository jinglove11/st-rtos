/**
 * @file test_smp.c
 * @brief Core completion #7 S5 — SMP dual-core parallel execution test
 *
 * Verifies CPU ownership/affinity, pinned parallel execution and cross-core
 * semaphore ping-pong through the scheduler IPI path.
 */

#include "test_framework.h"
#include "kernel.h"
#include "task.h"
#include "scheduler.h"
#include "hal.h"
#include "semaphore.h"
#include "endpoint.h"
#include "capability.h"

#if SMP && TEST_MODULE_SMP

#include <stdint.h>

/* A stress test must fail with a useful phase/result instead of making the
 * complete suite look dead forever.  These bounds do not reduce iteration
 * counts or concurrency: they only turn a lost wakeup/deadlock into a
 * deterministic failure that lets the remaining diagnostics run. */
#define SMP_OPERATION_TIMEOUT_TICKS  5000U
/* 1M 次 ping-pong 往返实测 ~11.6k/s (约 86s),join 上限需覆盖全程 */
#define SMP_JOIN_TIMEOUT_TICKS      180000U
#define SMP_TASK_CHURN_MAX_ATTEMPTS \
    (SMP_POOL_STRESS_ITERATIONS + KERNEL_MAX_TASKS)

static kern_err_t smp_join_bounded(task_id_t task, void **retval,
                                   uint32_t timeout) {
    kern_err_t err = task_join(task, retval, timeout);
    if (err != KERN_OK && err != KERN_ERR_FAULT &&
        task_get_state(task) != TASK_STATE_TERMINATED) {
        (void)task_delete(task);
    }
    return err;
}

/*============================================================================
 * Shared state for parallel execution proof
 *============================================================================*/

typedef struct {
    volatile uint32_t iterations;
    volatile uint32_t observed_cpu;
} smp_worker_result_t;

static smp_worker_result_t smp_worker_result[2];

static void smp_worker_task(void *arg) {
    uint32_t index = (uint32_t)(uintptr_t)arg;
    smp_worker_result[index].observed_cpu = hal_get_cpu_id();
    for (uint32_t i = 0; i < 10000U; i++) {
        smp_worker_result[index].iterations++;
        if ((i & 0xFFU) == 0U) {
            task_yield();
        }
    }
}

/*============================================================================
 * Test 1: core1 is running and has a current task
 *============================================================================*/

static void test_core1_running(void) {
    test_section("Test 1: core1 has a running task");

    /* _current_task[1] should be non-NULL if core1 is running. */
    tcb_t *c1 = _current_task[1];
    TEST_ASSERT_NOT_NULL(c1, "core1 _current_task is non-NULL");
    if (c1) {
        /* core1 may be running the idle task (id == -1) if no other task
         * is available — that still proves core1 is alive. */
        TEST_ASSERT(c1->id == -1 || c1->id >= 0, "core1 current task has valid id or idle");
        TEST_ASSERT_EQ(1, c1->cpu_owner, "core1 current task owned by core1");
        TEST_ASSERT((c1->affinity_mask & (1UL << 1)) != 0U,
                    "core1 current task affinity includes core1");
        TEST_ASSERT_EQ(TASK_MIGRATION_STABLE, c1->migration_state,
                       "core1 current task migration stable");
    }
}

/*============================================================================
 * Test 2: two tasks run concurrently (counter increases)
 *============================================================================*/

static void test_parallel_execution(void) {
    test_section("Test 2: parallel task execution");

    smp_worker_result[0] = (smp_worker_result_t){0};
    smp_worker_result[1] = (smp_worker_result_t){0};

    task_id_t t1 = task_create("smp_w1", smp_worker_task,
                               (void *)(uintptr_t)0U, 9, 1024);
    task_id_t t2 = task_create("smp_w2", smp_worker_task,
                               (void *)(uintptr_t)1U, 9, 1024);
    TEST_ASSERT(t1 >= 0 && t2 >= 0, "two workers created");
    if (t1 < 0 || t2 < 0) {
        if (t1 >= 0) task_delete(t1);
        if (t2 >= 0) task_delete(t2);
        return;
    }

    TEST_ASSERT_EQ(KERN_OK, task_set_affinity(t1, 1UL << 0),
                   "worker 1 pinned to core0");
    TEST_ASSERT_EQ(KERN_OK, task_set_affinity(t2, 1UL << 1),
                   "worker 2 pinned to core1");

    task_start(t1);
    task_start(t2);

    /* Wait for both workers to finish. */
    kern_err_t e1 = smp_join_bounded(t1, NULL, SMP_JOIN_TIMEOUT_TICKS);
    kern_err_t e2 = smp_join_bounded(t2, NULL, SMP_JOIN_TIMEOUT_TICKS);
    TEST_ASSERT_EQ((int)KERN_OK, (int)e1, "worker 1 joined");
    TEST_ASSERT_EQ((int)KERN_OK, (int)e2, "worker 2 joined");

    TEST_ASSERT_EQ(10000, smp_worker_result[0].iterations,
                   "core0 worker completed all iterations");
    TEST_ASSERT_EQ(10000, smp_worker_result[1].iterations,
                   "core1 worker completed all iterations");
    TEST_ASSERT_EQ(0, smp_worker_result[0].observed_cpu,
                   "worker 1 executed on core0");
    TEST_ASSERT_EQ(1, smp_worker_result[1].observed_cpu,
                   "worker 2 executed on core1");
    (void)task_delete(t1);
    (void)task_delete(t2);
}

/*============================================================================
 * Test 3: cross-core semaphore ping-pong exercises remote wakeup/IPI
 *============================================================================*/

#ifndef SMP_STRESS_ITERATIONS
#define SMP_STRESS_ITERATIONS 10000U
#endif

#define SMP_PING_PONG_ITERATIONS ((uint32_t)SMP_STRESS_ITERATIONS)

static sem_id_t smp_ping_sem[2];
static volatile uint32_t smp_ping_count[2];
static volatile uint32_t smp_ping_cpu[2];

static void smp_ping_task(void *arg) {
    uint32_t index = (uint32_t)(uintptr_t)arg;
    uint32_t peer = index ^ 1U;
    smp_ping_cpu[index] = hal_get_cpu_id();

    for (uint32_t i = 0; i < SMP_PING_PONG_ITERATIONS; i++) {
        if (sem_wait(smp_ping_sem[index],
                     SMP_OPERATION_TIMEOUT_TICKS) != KERN_OK) {
            return;
        }
        smp_ping_count[index]++;
        (void)sem_post(smp_ping_sem[peer]);
    }
}

static void test_cross_core_ping_pong(void) {
    test_section("Test 3: cross-core IPI ping-pong");

    smp_ping_count[0] = smp_ping_count[1] = 0U;
    smp_ping_cpu[0] = smp_ping_cpu[1] = UINT32_MAX;
    smp_ping_sem[0] = sem_create(1U, 1U);
    smp_ping_sem[1] = sem_create(0U, 1U);
    TEST_ASSERT(smp_ping_sem[0] >= 0 && smp_ping_sem[1] >= 0,
                "ping-pong semaphores created");
    if (smp_ping_sem[0] < 0 || smp_ping_sem[1] < 0) {
        return;
    }

    task_id_t a = task_create("smp_p0", smp_ping_task,
                              (void *)(uintptr_t)0U, 9, 1024);
    task_id_t b = task_create("smp_p1", smp_ping_task,
                              (void *)(uintptr_t)1U, 9, 1024);
    TEST_ASSERT(a >= 0 && b >= 0, "ping-pong tasks created");
    if (a >= 0 && b >= 0) {
        (void)task_set_affinity(a, 1UL << 0);
        (void)task_set_affinity(b, 1UL << 1);
        (void)task_start(a);
        (void)task_start(b);
        TEST_ASSERT_EQ(KERN_OK,
                       smp_join_bounded(a, NULL, SMP_JOIN_TIMEOUT_TICKS),
                       "core0 ping task joined");
        TEST_ASSERT_EQ(KERN_OK,
                       smp_join_bounded(b, NULL, SMP_JOIN_TIMEOUT_TICKS),
                       "core1 ping task joined");
        TEST_ASSERT_EQ(SMP_PING_PONG_ITERATIONS, smp_ping_count[0],
                       "core0 received every ping");
        TEST_ASSERT_EQ(SMP_PING_PONG_ITERATIONS, smp_ping_count[1],
                       "core1 received every ping");
        TEST_ASSERT_EQ(0, smp_ping_cpu[0], "ping task 0 stayed on core0");
        TEST_ASSERT_EQ(1, smp_ping_cpu[1], "ping task 1 stayed on core1");
        (void)task_delete(a);
        (void)task_delete(b);
    } else {
        if (a >= 0) (void)task_delete(a);
        if (b >= 0) (void)task_delete(b);
    }

    (void)sem_delete(smp_ping_sem[0]);
    (void)sem_delete(smp_ping_sem[1]);
}

/*============================================================================
 * Test 4: cross-core endpoint call/reply ping-pong
 *============================================================================*/

static ep_id_t smp_ping_ep;
static volatile uint32_t smp_ep_count[2];
static volatile uint32_t smp_ep_cpu[2];
static volatile kern_err_t smp_ep_error[2];

static void smp_ep_server_task(void *arg) {
    (void)arg;
    smp_ep_cpu[1] = hal_get_cpu_id();

    for (uint32_t i = 0; i < SMP_PING_PONG_ITERATIONS; i++) {
        uint32_t msg = 0;
        kern_err_t err = endpoint_recv(smp_ping_ep, &msg,
                                       SMP_OPERATION_TIMEOUT_TICKS);
        if (err != KERN_OK || msg != i) {
            smp_ep_error[1] = (err != KERN_OK) ? err : KERN_ERR_STATE;
            return;
        }
        msg++;
        err = endpoint_reply(smp_ping_ep, &msg);
        if (err != KERN_OK) {
            smp_ep_error[1] = err;
            return;
        }
        smp_ep_count[1]++;
    }
}

static void smp_ep_client_task(void *arg) {
    (void)arg;
    smp_ep_cpu[0] = hal_get_cpu_id();

    for (uint32_t i = 0; i < SMP_PING_PONG_ITERATIONS; i++) {
        uint32_t msg = i;
        kern_err_t err = endpoint_send(smp_ping_ep, &msg,
                                       SMP_OPERATION_TIMEOUT_TICKS);
        if (err != KERN_OK || msg != i + 1U) {
            smp_ep_error[0] = (err != KERN_OK) ? err : KERN_ERR_STATE;
            return;
        }
        smp_ep_count[0]++;
    }
}

static void test_cross_core_endpoint_ping_pong(void) {
    test_section("Test 4: cross-core endpoint ping-pong");

    smp_ep_count[0] = smp_ep_count[1] = 0U;
    smp_ep_cpu[0] = smp_ep_cpu[1] = UINT32_MAX;
    smp_ep_error[0] = smp_ep_error[1] = KERN_OK;
    smp_ping_ep = endpoint_create("smp_ep", sizeof(uint32_t), 2U);
    TEST_ASSERT(smp_ping_ep >= 0, "SMP endpoint created");
    if (smp_ping_ep < 0) return;

    task_id_t server = task_create("smp_es", smp_ep_server_task, NULL, 9, 1024);
    task_id_t client = task_create("smp_ec", smp_ep_client_task, NULL, 9, 1024);
    TEST_ASSERT(server >= 0 && client >= 0, "endpoint ping-pong tasks created");
    if (server >= 0 && client >= 0) {
        (void)task_set_affinity(server, 1UL << 1);
        (void)task_set_affinity(client, 1UL << 0);
        (void)task_start(server);
        (void)task_start(client);
        TEST_ASSERT_EQ(KERN_OK,
                       smp_join_bounded(client, NULL,
                                        SMP_JOIN_TIMEOUT_TICKS),
                       "endpoint client joined");
        TEST_ASSERT_EQ(KERN_OK,
                       smp_join_bounded(server, NULL,
                                        SMP_JOIN_TIMEOUT_TICKS),
                       "endpoint server joined");
        TEST_ASSERT_EQ(KERN_OK, smp_ep_error[0], "endpoint client error-free");
        TEST_ASSERT_EQ(KERN_OK, smp_ep_error[1], "endpoint server error-free");
        TEST_ASSERT_EQ(SMP_PING_PONG_ITERATIONS, smp_ep_count[0],
                       "endpoint client completed every call");
        TEST_ASSERT_EQ(SMP_PING_PONG_ITERATIONS, smp_ep_count[1],
                       "endpoint server completed every reply");
        TEST_ASSERT_EQ(0, smp_ep_cpu[0], "endpoint client stayed on core0");
        TEST_ASSERT_EQ(1, smp_ep_cpu[1], "endpoint server stayed on core1");
        (void)task_delete(client);
        (void)task_delete(server);
    } else {
        if (server >= 0) (void)task_delete(server);
        if (client >= 0) (void)task_delete(client);
    }

    (void)endpoint_delete(smp_ping_ep);
}

/*============================================================================
 * Test 5: capability pool derive/lookup/delete from both cores
 *============================================================================*/

#define SMP_POOL_STRESS_ITERATIONS \
    ((SMP_PING_PONG_ITERATIONS / 100U) < 100U ? 100U : \
     (SMP_PING_PONG_ITERATIONS / 100U))

#if CAP_ENABLE
static uint32_t smp_cap_object[2];
static volatile uint32_t smp_cap_count[2];
static volatile uint32_t smp_cap_errors[2];

static void smp_cap_stress_task(void *arg) {
    uint32_t index = (uint32_t)(uintptr_t)arg;

    for (uint32_t i = 0; i < SMP_POOL_STRESS_ITERATIONS; i++) {
        cap_id_t root = cap_create(&smp_cap_object[index], CAP_OBJ_SYSTEM,
                                   CAP_FULL, (uint8_t)task_self());
        if (root < 0) {
            smp_cap_errors[index]++;
            continue;
        }
        cap_id_t child = cap_derive(root, CAP_READ);
        if (child < 0 ||
            cap_resolve(child, CAP_OBJ_SYSTEM, CAP_READ) !=
                &smp_cap_object[index]) {
            smp_cap_errors[index]++;
        }
        if (child >= 0) cap_delete(child);
        cap_delete(root);
        smp_cap_count[index]++;
    }
}

static void test_cross_core_cap_pool(void) {
    test_section("Test 5: cross-core capability pool stress");
    smp_cap_count[0] = smp_cap_count[1] = 0U;
    smp_cap_errors[0] = smp_cap_errors[1] = 0U;

    task_id_t a = task_create("smp_c0", smp_cap_stress_task,
                              (void *)(uintptr_t)0U, 9, 1024);
    task_id_t b = task_create("smp_c1", smp_cap_stress_task,
                              (void *)(uintptr_t)1U, 9, 1024);
    TEST_ASSERT(a >= 0 && b >= 0, "cap stress tasks created");
    if (a >= 0 && b >= 0) {
        (void)task_set_affinity(a, 1UL << 0);
        (void)task_set_affinity(b, 1UL << 1);
        (void)task_start(a);
        (void)task_start(b);
        TEST_ASSERT_EQ(KERN_OK,
                       smp_join_bounded(a, NULL, SMP_JOIN_TIMEOUT_TICKS),
                       "cap stress core0 joined");
        TEST_ASSERT_EQ(KERN_OK,
                       smp_join_bounded(b, NULL, SMP_JOIN_TIMEOUT_TICKS),
                       "cap stress core1 joined");
        TEST_ASSERT_EQ(0, smp_cap_errors[0], "cap stress core0 error-free");
        TEST_ASSERT_EQ(0, smp_cap_errors[1], "cap stress core1 error-free");
        TEST_ASSERT_EQ(SMP_POOL_STRESS_ITERATIONS, smp_cap_count[0],
                       "cap stress core0 completed");
        TEST_ASSERT_EQ(SMP_POOL_STRESS_ITERATIONS, smp_cap_count[1],
                       "cap stress core1 completed");
        (void)task_delete(a);
        (void)task_delete(b);
    } else {
        if (a >= 0) (void)task_delete(a);
        if (b >= 0) (void)task_delete(b);
    }
}
#endif

/*============================================================================
 * Test 6: concurrent task create/exit/join/id-reuse
 *============================================================================*/

static volatile uint32_t smp_child_runs[2];
static volatile uint32_t smp_task_churn[2];
static volatile uint32_t smp_task_errors[2];

static void smp_churn_child(void *arg) {
    uint32_t index = (uint32_t)(uintptr_t)arg;
    smp_child_runs[index]++;
}

static void smp_task_churn_worker(void *arg) {
    uint32_t index = (uint32_t)(uintptr_t)arg;
    uint32_t attempts = 0U;

    while (smp_task_churn[index] < SMP_POOL_STRESS_ITERATIONS &&
           attempts < SMP_TASK_CHURN_MAX_ATTEMPTS) {
        attempts++;
        /* A task that exited in the immediately preceding tick intentionally
         * retains its ID for one tick so a late join-by-ID remains valid.
         * A transient RESOURCE result is backpressure, not a failed reuse
         * operation.  The overall attempts bound still catches a pool leak. */
        task_id_t child = task_create("smp_ch", smp_churn_child,
                                      (void *)(uintptr_t)index, 10, 512);
        if (child < 0) {
            (void)task_delay(1U);
            continue;
        }

        kern_err_t join_err = KERN_ERR_STATE;
        kern_err_t delete_err = KERN_ERR_STATE;
        if (child >= 0 &&
            task_set_affinity(child, 1UL << index) == KERN_OK &&
            task_start(child) == KERN_OK) {
            join_err = smp_join_bounded(child, NULL,
                                        SMP_OPERATION_TIMEOUT_TICKS);
            delete_err = task_delete(child);
        }
        /* Reclaim may win the tick boundary immediately after a successful
         * join; NOEXIST then means the slot was already safely recycled. */
        if (child < 0 || join_err != KERN_OK ||
            (delete_err != KERN_OK && delete_err != KERN_ERR_NOEXIST)) {
            smp_task_errors[index]++;
            if (child >= 0) (void)task_delete(child);
            continue;
        }
        smp_task_churn[index]++;
    }

    if (smp_task_churn[index] < SMP_POOL_STRESS_ITERATIONS) {
        smp_task_errors[index] +=
            SMP_POOL_STRESS_ITERATIONS - smp_task_churn[index];
    }
}

static void test_cross_core_task_reuse(void) {
    test_section("Test 6: cross-core task slot reuse stress");
    smp_child_runs[0] = smp_child_runs[1] = 0U;
    smp_task_churn[0] = smp_task_churn[1] = 0U;
    smp_task_errors[0] = smp_task_errors[1] = 0U;

    task_id_t a = task_create("smp_t0", smp_task_churn_worker,
                              (void *)(uintptr_t)0U, 9, 1024);
    task_id_t b = task_create("smp_t1", smp_task_churn_worker,
                              (void *)(uintptr_t)1U, 9, 1024);
    TEST_ASSERT(a >= 0 && b >= 0, "task reuse workers created");
    if (a >= 0 && b >= 0) {
        (void)task_set_affinity(a, 1UL << 0);
        (void)task_set_affinity(b, 1UL << 1);
        (void)task_start(a);
        (void)task_start(b);
        TEST_ASSERT_EQ(KERN_OK,
                       smp_join_bounded(a, NULL, SMP_JOIN_TIMEOUT_TICKS),
                       "task reuse core0 joined");
        TEST_ASSERT_EQ(KERN_OK,
                       smp_join_bounded(b, NULL, SMP_JOIN_TIMEOUT_TICKS),
                       "task reuse core1 joined");
        TEST_ASSERT_EQ(0, smp_task_errors[0], "task reuse core0 error-free");
        TEST_ASSERT_EQ(0, smp_task_errors[1], "task reuse core1 error-free");
        TEST_ASSERT_EQ(SMP_POOL_STRESS_ITERATIONS, smp_task_churn[0],
                       "task reuse core0 completed");
        TEST_ASSERT_EQ(SMP_POOL_STRESS_ITERATIONS, smp_task_churn[1],
                       "task reuse core1 completed");
        TEST_ASSERT_EQ(SMP_POOL_STRESS_ITERATIONS, smp_child_runs[0],
                       "every core0 child ran once");
        TEST_ASSERT_EQ(SMP_POOL_STRESS_ITERATIONS, smp_child_runs[1],
                       "every core1 child ran once");
        (void)task_delete(a);
        (void)task_delete(b);
    } else {
        if (a >= 0) (void)task_delete(a);
        if (b >= 0) (void)task_delete(b);
    }
}

/*============================================================================
 * Test 7: deterministic send/delete/timeout/fault interleavings
 *============================================================================*/

#define SMP_RACE_ITERATIONS \
    ((SMP_PING_PONG_ITERATIONS / 1000U) < 10U ? 10U : \
     (SMP_PING_PONG_ITERATIONS / 1000U))

static volatile ep_id_t smp_race_ep;
static volatile uint32_t smp_race_seed;
static volatile kern_err_t smp_race_send_result;
static volatile kern_err_t smp_race_recv_result;
static volatile kern_err_t smp_race_delete_result;

static int smp_race_ipc_result_ok(kern_err_t err) {
    return err == KERN_OK || err == KERN_ERR_TIMEOUT ||
           err == KERN_ERR_NOEXIST || err == KERN_ERR_PARAM;
}

static void smp_race_recv_task(void *arg) {
    (void)arg;
    uint32_t msg = 0U;
    if ((smp_race_seed & 1U) != 0U) (void)task_delay(1U);
    smp_race_recv_result = endpoint_recv(smp_race_ep, &msg, 2U);
    if (smp_race_recv_result == KERN_OK) {
        msg ^= 0xA5A5A5A5U;
        (void)endpoint_reply(smp_race_ep, &msg);
    }
}

static void smp_race_send_task(void *arg) {
    (void)arg;
    uint32_t msg = 0x12345678U;
    if ((smp_race_seed & 2U) != 0U) (void)task_delay(1U);
    smp_race_send_result = endpoint_send(smp_race_ep, &msg, 2U);
}

static void smp_race_delete_task(void *arg) {
    (void)arg;
    (void)task_delay(1U + ((smp_race_seed >> 2) & 1U));
    smp_race_delete_result = endpoint_delete(smp_race_ep);
}

static void smp_race_fault_task(void *arg) {
    (void)arg;
    if ((smp_race_seed & 8U) != 0U) (void)task_delay(1U);
    (void)task_terminate_with_result(sched_get_current(), KERN_ERR_FAULT);
    sched_yield();
    for (;;) __asm volatile("wfi");
}

static void test_cross_core_event_interleavings(void) {
    test_section("Test 7: send/delete/timeout/fault interleavings");
    uint32_t failures = 0U;
    uint32_t seed = 0x13579BDFU;

    for (uint32_t i = 0; i < SMP_RACE_ITERATIONS; i++) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        smp_race_seed = seed;
        smp_race_send_result = KERN_ERR_STATE;
        smp_race_recv_result = KERN_ERR_STATE;
        smp_race_delete_result = KERN_ERR_STATE;
        smp_race_ep = endpoint_create("smp_rx", sizeof(uint32_t), 2U);
        if (smp_race_ep < 0) {
            failures++;
            continue;
        }

        task_id_t recv = task_create("smp_rr", smp_race_recv_task, NULL, 9, 512);
        task_id_t send = task_create("smp_rs", smp_race_send_task, NULL, 9, 512);
        task_id_t del = task_create("smp_rd", smp_race_delete_task, NULL, 8, 512);
        task_id_t fault = task_create("smp_rf", smp_race_fault_task, NULL, 9, 512);
        if (recv < 0 || send < 0 || del < 0 || fault < 0) {
            failures++;
            if (recv >= 0) (void)task_delete(recv);
            if (send >= 0) (void)task_delete(send);
            if (del >= 0) (void)task_delete(del);
            if (fault >= 0) (void)task_delete(fault);
            (void)endpoint_delete(smp_race_ep);
            continue;
        }

        (void)task_set_affinity(recv, 1UL << 1);
        (void)task_set_affinity(send, 1UL << 0);
        (void)task_set_affinity(del, 1UL << (seed & 1U));
        (void)task_set_affinity(fault, 1UL << ((seed >> 1) & 1U));
        (void)task_start(recv);
        (void)task_start(send);
        (void)task_start(fault);
        (void)task_start(del);

        kern_err_t recv_join = smp_join_bounded(
            recv, NULL, SMP_OPERATION_TIMEOUT_TICKS);
        kern_err_t send_join = smp_join_bounded(
            send, NULL, SMP_OPERATION_TIMEOUT_TICKS);
        kern_err_t fault_join = smp_join_bounded(
            fault, NULL, SMP_OPERATION_TIMEOUT_TICKS);
        kern_err_t del_join = smp_join_bounded(
            del, NULL, SMP_OPERATION_TIMEOUT_TICKS);
        if (recv_join != KERN_OK || send_join != KERN_OK ||
            del_join != KERN_OK || fault_join != KERN_ERR_FAULT ||
            !smp_race_ipc_result_ok(smp_race_recv_result) ||
            !smp_race_ipc_result_ok(smp_race_send_result) ||
            smp_race_delete_result != KERN_OK) {
            failures++;
        }

        (void)task_delete(recv);
        (void)task_delete(send);
        (void)task_delete(fault);
        (void)task_delete(del);
        if (endpoint_exists(smp_race_ep)) (void)endpoint_delete(smp_race_ep);
    }

    TEST_ASSERT_EQ(0, failures, "all randomized interleavings completed safely");
}

/*============================================================================
 * Test 8: continuation arming race — prepare/commit 窗口双核定点竞态
 *
 * core0 task 反复 sem_wait (走 prepare→commit 路径)。
 * core1 task 反复 sem_post,在 core0 的 ARMING/BLOCKED 窗口投递唤醒。
 * 验证:
 *   - 无 lost wakeup (sem_wait 不永久阻塞)
 *   - 无 double-wakeup (wait_queue invariant 不 panic)
 *   - 最终计数匹配
 *============================================================================*/

#define ARMING_RACE_ITERATIONS 500U

static sem_id_t arming_race_sem;
static volatile uint32_t arming_race_wait_count;
static volatile uint32_t arming_race_post_count;

static void arming_race_waiter(void *arg) {
    (void)arg;
    for (uint32_t i = 0; i < ARMING_RACE_ITERATIONS; i++) {
        kern_err_t err = sem_wait(arming_race_sem, SMP_OPERATION_TIMEOUT_TICKS);
        if (err != KERN_OK) {
            task_exit((void *)(intptr_t)err);
        }
        arming_race_wait_count++;
    }
    task_exit(NULL);
}

static void arming_race_poster(void *arg) {
    (void)arg;
    for (uint32_t i = 0; i < ARMING_RACE_ITERATIONS; i++) {
        kern_err_t err = sem_post(arming_race_sem);
        if (err != KERN_OK) {
            task_exit((void *)(intptr_t)err);
        }
        arming_race_post_count++;
    }
    task_exit(NULL);
}

static void test_continuation_arming_race(void) {
    test_section("Test 8: continuation arming race");

    arming_race_wait_count = 0;
    arming_race_post_count = 0;
    arming_race_sem = sem_create(0, ARMING_RACE_ITERATIONS);
    TEST_ASSERT(arming_race_sem >= 0, "arming race sem created");
    if (arming_race_sem < 0) return;

    task_id_t waiter = task_create("arm_w", arming_race_waiter, NULL, 8, 1024);
    task_id_t poster = task_create("arm_p", arming_race_poster, NULL, 8, 1024);
    TEST_ASSERT(waiter >= 0 && poster >= 0, "arming race tasks created");

    if (waiter >= 0 && poster >= 0) {
        (void)task_set_affinity(waiter, 1UL << 0);
        (void)task_set_affinity(poster, 1UL << 1);
        (void)task_start(waiter);
        (void)task_start(poster);

        void *wret = NULL, *pret = NULL;
        kern_err_t werr = smp_join_bounded(waiter, &wret, SMP_JOIN_TIMEOUT_TICKS);
        kern_err_t perr = smp_join_bounded(poster, &pret, SMP_JOIN_TIMEOUT_TICKS);

        TEST_ASSERT_EQ(KERN_OK, (int)werr, "arming race waiter joined");
        TEST_ASSERT_EQ(KERN_OK, (int)perr, "arming race poster joined");
        TEST_ASSERT(wret == NULL, "arming race waiter no error");
        TEST_ASSERT(pret == NULL, "arming race poster no error");
        TEST_ASSERT_EQ(ARMING_RACE_ITERATIONS, (int)arming_race_wait_count,
                       "arming race: all wakeups received");
        TEST_ASSERT_EQ(ARMING_RACE_ITERATIONS, (int)arming_race_post_count,
                       "arming race: all posts completed");
    }

    sem_delete(arming_race_sem);
}

/*============================================================================
 * Test 9: cross-core concurrent cap transfer to one shared dst CSpace
 *
 * 两个核同时向同一 dst task 传输 cap (COPY)。dst CSpace 64 slots,
 * 每核 60 次 → 120 次请求并发打满 CSpace:
 * - 成功的传输必须原子发布 (dst cap 有效,指向正确的发送方对象)
 * - CSpace 满时返回 KERN_ERR_RESOURCE (cap_txn reservation 在 CAP_LOCK 下)
 * - 不允许其他错误 (generation 破坏/部分传输)
 *============================================================================*/

#define SMP_CAP_XFER_ITERATIONS 60U

static uint64_t test_cspace_occupied(tcb_t *task) {
    cnode_t *cnode = cap_space_of(task);
    return cnode != NULL ? cnode->occupied : 0;
}

static uint32_t smp_cap_xfer_object[2];
static volatile uint32_t smp_xfer_ok[2];
static volatile uint32_t smp_xfer_busy[2];
static volatile uint32_t smp_xfer_err[2];
static cap_id_t smp_xfer_out[2][SMP_CAP_XFER_ITERATIONS];
static task_id_t smp_xfer_dst_id = (task_id_t)-1;

static void smp_cap_xfer_dst_task(void *arg) {
    (void)arg;
    for (;;) {
        (void)task_delay(1000);
    }
}

static void smp_cap_xfer_task(void *arg) {
    uint32_t index = (uint32_t)(uintptr_t)arg;
    tcb_t *me = task_get_tcb(task_self());
    tcb_t *dst = task_get_tcb(smp_xfer_dst_id);

    for (uint32_t i = 0; i < SMP_CAP_XFER_ITERATIONS; i++) {
        cap_id_t cap = cap_create(&smp_cap_xfer_object[index],
                                  CAP_OBJ_SYSTEM, CAP_FULL,
                                  (uint8_t)task_self());
        if (cap < 0) {
            smp_xfer_err[index]++;
            continue;
        }
        ipc_cap_xfer_t xfer;
        xfer.src_cap = cap;
        xfer.rights = CAP_READ;
        xfer.flags = IPC_CAP_COPY;
        cap_id_t out = (cap_id_t)-1;
        kern_err_t err = ipc_transfer_caps(me, dst, &xfer, 1, &out);
        if (err == KERN_OK) {
            smp_xfer_out[index][smp_xfer_ok[index]] = out;
            smp_xfer_ok[index]++;
        } else if (err == KERN_ERR_RESOURCE) {
            smp_xfer_busy[index]++;
        } else {
            smp_xfer_err[index]++;
        }
        (void)cap_delete(cap);
    }
    task_exit(NULL);
}

static void test_cross_core_cap_transfer(void) {
    test_section("Test 9: cross-core concurrent cap transfer");

    smp_xfer_ok[0] = smp_xfer_ok[1] = 0U;
    smp_xfer_busy[0] = smp_xfer_busy[1] = 0U;
    smp_xfer_err[0] = smp_xfer_err[1] = 0U;
    smp_cap_xfer_object[0] = 3301U;
    smp_cap_xfer_object[1] = 3302U;

    task_id_t dst = task_create("smp_dst", smp_cap_xfer_dst_task, NULL, 8, 1024);
    TEST_ASSERT(dst >= 0, "shared dst task created");
    if (dst < 0) {
        return;
    }
    tcb_t *dst_tcb = task_get_tcb(dst);
    dst_tcb->attrs = TASK_ATTR_USER;
    smp_xfer_dst_id = dst;

    uint16_t pool_before = cap_free_count();

    task_id_t a = task_create("smp_t0", smp_cap_xfer_task,
                              (void *)(uintptr_t)0U, 9, 1024);
    task_id_t b = task_create("smp_t1", smp_cap_xfer_task,
                              (void *)(uintptr_t)1U, 9, 1024);
    TEST_ASSERT(a >= 0 && b >= 0, "cap transfer workers created");

    if (a >= 0 && b >= 0) {
        (void)task_set_affinity(a, 1UL << 0);
        (void)task_set_affinity(b, 1UL << 1);
        (void)task_start(a);
        (void)task_start(b);
        TEST_ASSERT_EQ(KERN_OK,
                       smp_join_bounded(a, NULL, SMP_JOIN_TIMEOUT_TICKS),
                       "cap transfer core0 joined");
        TEST_ASSERT_EQ(KERN_OK,
                       smp_join_bounded(b, NULL, SMP_JOIN_TIMEOUT_TICKS),
                       "cap transfer core1 joined");

        TEST_ASSERT_EQ((int)SMP_CAP_XFER_ITERATIONS,
                       (int)(smp_xfer_ok[0] + smp_xfer_busy[0]),
                       "cap transfer core0 fully accounted");
        TEST_ASSERT_EQ((int)SMP_CAP_XFER_ITERATIONS,
                       (int)(smp_xfer_ok[1] + smp_xfer_busy[1]),
                       "cap transfer core1 fully accounted");
        TEST_ASSERT_EQ(0, (int)(smp_xfer_err[0] + smp_xfer_err[1]),
                       "cap transfer: no unexpected errors");

        uint32_t total_ok = smp_xfer_ok[0] + smp_xfer_ok[1];
        TEST_ASSERT(total_ok > 0, "cap transfer: at least one succeeded");
        /* occupied 是位图 (uint64),用 popcount 数实际占用 slot 数 */
        uint64_t occupied = test_cspace_occupied(dst_tcb);
        TEST_ASSERT_EQ((int)total_ok, (int)__builtin_popcountll(occupied),
                       "dst cspace holds exactly the received caps");

        /* 每个成功传输的 cap 必须解析到发送方的对象 */
        for (uint32_t k = 0; k < 2U; k++) {
            for (uint32_t i = 0; i < smp_xfer_ok[k]; i++) {
                void *ptr = cap_lookup_for(dst_tcb, smp_xfer_out[k][i],
                                           CAP_OBJ_SYSTEM, CAP_READ);
                TEST_ASSERT(ptr == &smp_cap_xfer_object[k],
                            "received cap resolves to sender object");
            }
        }

        /* 清理: 撤销全部 dst caps → CSpace 清空 */
        for (uint32_t k = 0; k < 2U; k++) {
            for (uint32_t i = 0; i < smp_xfer_ok[k]; i++) {
                (void)cap_revoke_for(dst_tcb, smp_xfer_out[k][i]);
            }
        }
        TEST_ASSERT_EQ(0, (int)test_cspace_occupied(dst_tcb),
                       "dst cspace drained after revoke");
    }

    (void)task_delete(a);
    (void)task_delete(b);
    (void)task_delete(dst);
    TEST_ASSERT_EQ((int)pool_before, (int)cap_free_count(),
                   "cap transfer: pool balanced after cleanup");
}

/*============================================================================
 * Test 10: both cores have distinct current tasks after workload
 *============================================================================*/

static void test_dual_core_active(void) {
    test_section("Test 8: both cores were active");

    /* After the parallel test, both _current_task[0] and [1] should be
     * valid (one may be idle). This confirms core1 didn't crash. */
    tcb_t *c0 = _current_task[0];
    tcb_t *c1 = _current_task[1];
    TEST_ASSERT_NOT_NULL(c0, "core0 still has a task");
    TEST_ASSERT_NOT_NULL(c1, "core1 still has a task (didn't crash)");
}

/*============================================================================
 * Module registration
 *============================================================================*/

static void test_smp_module(void) {
    test_print("[SMP] 1 core1 state\r\n");
    test_core1_running();
    test_print("[SMP] 2 pinned parallel workers\r\n");
    test_parallel_execution();
    test_print("[SMP] 3 semaphore ping-pong\r\n");
    test_cross_core_ping_pong();
    test_print("[SMP] 4 endpoint ping-pong\r\n");
    test_cross_core_endpoint_ping_pong();
#if CAP_ENABLE
    test_print("[SMP] 5 capability pool stress\r\n");
    test_cross_core_cap_pool();
#endif
    test_print("[SMP] 6 task-slot reuse stress\r\n");
    test_cross_core_task_reuse();
    test_print("[SMP] 7 event interleavings\r\n");
    test_cross_core_event_interleavings();
    test_print("[SMP] 8 continuation arming race\r\n");
    test_continuation_arming_race();
#if CAP_ENABLE
    test_print("[SMP] 9 concurrent cap transfer\r\n");
    test_cross_core_cap_transfer();
#endif
    test_print("[SMP] 10 final core state\r\n");
    test_dual_core_active();
    test_print("[SMP] complete\r\n");
}

TEST_MODULE_REGISTER(smp, test_smp_module);

#endif /* SMP && TEST_MODULE_SMP */
