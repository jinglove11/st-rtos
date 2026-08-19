/**
 * @file test_factory_user.c
 * @brief ABI 层 — 用户任务 Factory syscall 契约
 *
 * 用户任务持有 factory cap 时只能铸造授权类型;越权类型被拒。
*/

#include "test_framework.h"
#include "capability.h"
#include "event.h"
#include "factory.h"
#include "semaphore.h"
#include "task.h"
#include "user_api.h"
#include <string.h>

#if TEST_ENABLE && CAP_ENABLE && MPU_ENABLE

static void user_factory_syscall_task(void *arg) {
    (void)arg;

    int factory_cap = sys_cap_self_slot(CAP_OBJ_FACTORY, 0);
    if (factory_cap < 0 ||
        sys_cap_type(factory_cap) != CAP_OBJ_FACTORY ||
        sys_cap_badge(factory_cap) !=
            (FACTORY_OBJECT_BIT(CAP_OBJ_SEMAPHORE) |
             FACTORY_OBJECT_BIT(CAP_OBJ_FRAME))) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_CAP);
    }

    factory_create_request_t request;
    memset(&request, 0, sizeof(request));
    request.obj_type = CAP_OBJ_SEMAPHORE;
    request.rights = CAP_FULL;
    request.param0 = 0U;
    request.param1 = 1U;
    int sem_cap = sys_factory_create(factory_cap, &request);
    if (sem_cap < 0 || sys_cap_type(sem_cap) != CAP_OBJ_SEMAPHORE) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_STATE);
    }
    if (sys_sem_post(sem_cap) != KERN_OK ||
        sys_sem_wait(sem_cap, 1U) != KERN_OK) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_STATE);
    }

    memset(&request, 0, sizeof(request));
    request.obj_type = CAP_OBJ_FRAME;
    request.rights = CAP_FULL;
    request.param0 = 256U;
    int frame_cap = sys_factory_create(factory_cap, &request);
    if (frame_cap < 0 || sys_cap_type(frame_cap) != CAP_OBJ_FRAME ||
        sys_mem_size(frame_cap) != 256) {
        (void)sys_sem_delete(sem_cap);
        sys_task_exit((void *)(intptr_t)KERN_ERR_STATE);
    }
    uint32_t *frame = (uint32_t *)sys_mem_map(
        frame_cap, CAP_READ | CAP_WRITE);
    if ((intptr_t)frame < 0 || frame == NULL) {
        (void)sys_mem_free(frame_cap);
        (void)sys_sem_delete(sem_cap);
        sys_task_exit((void *)(intptr_t)KERN_ERR_STATE);
    }
    frame[0] = 0x4652414dU;
    if (frame[0] != 0x4652414dU ||
        sys_mem_free(frame_cap) != KERN_OK ||
        sys_mem_size(frame_cap) != KERN_ERR_CAP) {
        (void)sys_sem_delete(sem_cap);
        sys_task_exit((void *)(intptr_t)KERN_ERR_STATE);
    }

    memset(&request, 0, sizeof(request));
    request.obj_type = CAP_OBJ_EVENT;
    if (sys_factory_create(factory_cap, &request) != KERN_ERR_PERM) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_PERM);
    }
    if (sys_sem_delete(sem_cap) != KERN_OK) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_STATE);
    }
    sys_task_exit((void *)(intptr_t)KERN_OK);
}

static void test_factory_user_syscall(void) {
    test_section("Test 3: user Factory syscall");

    uint16_t free_before = cap_free_count();
    task_id_t user_id = task_create_user("u_factory",
                                          user_factory_syscall_task,
                                          NULL, 5, 768);
    TEST_ASSERT(user_id >= 0, "Factory syscall user task created");
    if (user_id < 0) {
        return;
    }

    tcb_t *user = task_get_tcb(user_id);
    cap_id_t factory_cap = factory_create_root_cap(
        user, FACTORY_OBJECT_BIT(CAP_OBJ_SEMAPHORE) |
                  FACTORY_OBJECT_BIT(CAP_OBJ_FRAME),
        CAP_FULL);
    TEST_ASSERT(factory_cap >= 0,
                "Factory syscall user receives bootstrap authority");
    if (factory_cap < 0) {
        (void)task_delete(user_id);
        return;
    }

    TEST_ASSERT_EQ((int)KERN_OK, (int)task_start(user_id),
                   "Factory syscall user task started");
    void *retval = NULL;
    kern_err_t err = task_join(user_id, &retval, 2000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "Factory syscall user task joined");
    if (err == KERN_OK) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "user Factory creates only authorized object");
    }

    (void)task_delete(user_id);
    TEST_ASSERT_EQ((int)free_before, (int)cap_free_count(),
                   "Factory syscall test restores capability pool");
}

static void test_factory_user_module(void) {
    test_factory_user_syscall();
}

TEST_ABI_MODULE(factory_user, test_factory_user_module);

#endif
