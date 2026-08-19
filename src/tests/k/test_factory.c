/**
 * @file test_factory.c
 * @brief M2 Factory capability, attenuation, lifecycle and user ABI tests
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

static void factory_placeholder_task(void *arg) {
    (void)arg;
}

static void factory_delete_sem_cap(tcb_t *owner, cap_id_t cap) {
    void *object = cap_lookup_for(owner, cap, CAP_OBJ_SEMAPHORE, CAP_MANAGE);
    if (object != NULL) {
        sem_id_t sem = sem_id_from_obj(object);
        if (sem >= 0) {
            (void)sem_delete(sem);
        }
    }
}

static void test_factory_authority_and_lifecycle(void) {
    test_section("Test 1: Factory authority and lifecycle");

    uint16_t free_before = cap_free_count();
    task_id_t owner_id = task_create_user("fac_owner",
                                           factory_placeholder_task,
                                           NULL, 10, 512);
    task_id_t other_id = task_create_user("fac_other",
                                           factory_placeholder_task,
                                           NULL, 10, 512);
    TEST_ASSERT(owner_id >= 0 && other_id >= 0,
                "Factory owner tasks created");
    if (owner_id < 0 || other_id < 0) {
        if (owner_id >= 0) {
            (void)task_delete(owner_id);
        }
        if (other_id >= 0) {
            (void)task_delete(other_id);
        }
        return;
    }

    tcb_t *owner = task_get_tcb(owner_id);
    tcb_t *other = task_get_tcb(other_id);
    uint32_t root_mask = FACTORY_OBJECT_BIT(CAP_OBJ_SEMAPHORE) |
                         FACTORY_OBJECT_BIT(CAP_OBJ_EVENT) |
                         FACTORY_OBJECT_BIT(CAP_OBJ_FRAME);
    cap_id_t root = factory_create_root_cap(owner, root_mask, CAP_FULL);
    TEST_ASSERT(root >= 0, "root Factory cap created");
    TEST_ASSERT(cap_is_local_cptr(root), "root Factory is a local CPtr");
    TEST_ASSERT_EQ((int)root_mask, (int)cap_get_badge(root),
                   "root Factory badge is its authorization mask");
    if (root < 0) {
        (void)task_delete(owner_id);
        (void)task_delete(other_id);
        return;
    }

    void *factory_object = cap_lookup_for(owner, root, CAP_OBJ_FACTORY,
                                          CAP_WRITE);
    TEST_ASSERT_NOT_NULL(factory_object, "root Factory resolves");
    uint32_t factory_generation =
        factory_object != NULL
            ? ((kobject_header_t *)factory_object)->generation
            : 0U;

    factory_create_request_t request;
    memset(&request, 0, sizeof(request));
    request.obj_type = CAP_OBJ_SEMAPHORE;
    request.rights = CAP_FULL;
    request.param0 = 0U;
    request.param1 = 1U;
    cap_id_t sem_root = factory_create_for(owner, root, &request);
    TEST_ASSERT(sem_root >= 0, "root Factory creates semaphore");
    TEST_ASSERT_NOT_NULL(cap_lookup_for(owner, sem_root, CAP_OBJ_SEMAPHORE,
                                        CAP_WRITE),
                         "created semaphore cap installed in caller CSpace");

    request.obj_type = CAP_OBJ_FRAME;
    request.param0 = 256U;
    request.param1 = 0U;
    cap_id_t frame_root = factory_create_for(owner, root, &request);
    TEST_ASSERT(frame_root >= 0, "root Factory creates Frame");
    kobject_header_t *frame_object =
        (kobject_header_t *)cap_lookup_for(owner, frame_root,
                                           CAP_OBJ_FRAME, CAP_MANAGE);
    TEST_ASSERT_NOT_NULL(frame_object,
                         "created Frame cap resolves persistent metadata");

    uint32_t sem_mask = FACTORY_OBJECT_BIT(CAP_OBJ_SEMAPHORE);
    cap_id_t sem_factory = cap_mint_for(
        owner, root, CAP_WRITE | CAP_TRANSFER | CAP_GRANT, sem_mask);
    TEST_ASSERT(sem_factory >= 0, "Factory authority attenuated by mint");
    TEST_ASSERT_EQ((int)sem_mask, (int)cap_get_badge(sem_factory),
                   "minted Factory carries reduced mask");

    cap_id_t broadened = cap_mint_for(
        owner, sem_factory, CAP_WRITE,
        sem_mask | FACTORY_OBJECT_BIT(CAP_OBJ_EVENT));
    TEST_ASSERT(broadened < 0,
                "Factory mint cannot broaden object authorization");

    request.obj_type = CAP_OBJ_SEMAPHORE;
    request.param0 = 0U;
    request.param1 = 1U;
    cap_id_t sem_child = factory_create_for(owner, sem_factory, &request);
    TEST_ASSERT(sem_child >= 0,
                "attenuated Factory creates authorized semaphore");

    request.obj_type = CAP_OBJ_FRAME;
    request.param0 = 256U;
    request.param1 = 0U;
    cap_id_t denied_frame =
        factory_create_for(owner, sem_factory, &request);
    TEST_ASSERT_EQ((int)KERN_ERR_PERM, (int)denied_frame,
                   "attenuated Factory rejects unauthorized Frame");

    request.obj_type = CAP_OBJ_EVENT;
    request.param0 = 0U;
    request.param1 = 0U;
    cap_id_t denied = factory_create_for(owner, sem_factory, &request);
    TEST_ASSERT_EQ((int)KERN_ERR_PERM, (int)denied,
                   "attenuated Factory rejects unauthorized object type");

    cap_id_t copied = cap_copy_to(owner, sem_factory, other, CAP_WRITE);
    TEST_ASSERT(copied >= 0, "Factory cap copied to another CSpace");
    TEST_ASSERT_EQ((int)sem_mask, (int)cap_get_badge(copied),
                   "ordinary Factory copy preserves authorization mask");

    request.obj_type = CAP_OBJ_SEMAPHORE;
    request.param0 = 0U;
    request.param1 = 1U;
    cap_id_t sem_other = factory_create_for(other, copied, &request);
    TEST_ASSERT(sem_other >= 0,
                "copied Factory creates object for destination task");
    TEST_ASSERT_EQ((int)KERN_ERR_CAP,
                   (int)factory_create_for(other, root, &request),
                   "foreign local Factory CPtr is rejected");

    factory_delete_sem_cap(owner, sem_root);
    factory_delete_sem_cap(owner, sem_child);
    factory_delete_sem_cap(other, sem_other);
    if (frame_root >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK,
                       (int)cap_revoke_for(owner, frame_root),
                       "Frame created by Factory is revocable");
    }

    TEST_ASSERT_EQ((int)KERN_OK, (int)cap_revoke_for(owner, root),
                   "revoking root Factory removes its derivation tree");
    TEST_ASSERT_NULL(cap_lookup_for(owner, sem_factory, CAP_OBJ_FACTORY,
                                    CAP_WRITE),
                     "minted Factory becomes stale after root revoke");
    TEST_ASSERT_NULL(cap_lookup_for(other, copied, CAP_OBJ_FACTORY,
                                    CAP_WRITE),
                     "copied Factory becomes stale after root revoke");
    if (factory_object != NULL) {
        TEST_ASSERT(((kobject_header_t *)factory_object)->generation !=
                        factory_generation,
                    "last Factory cap release advances object generation");
    }

    cap_id_t replacement =
        factory_create_root_cap(owner, sem_mask, CAP_FULL);
    TEST_ASSERT(replacement >= 0, "released Factory pool slot is reusable");
    void *replacement_object =
        cap_lookup_for(owner, replacement, CAP_OBJ_FACTORY, CAP_WRITE);
    TEST_ASSERT_EQ((uintptr_t)factory_object, (uintptr_t)replacement_object,
                   "Factory allocation reuses released pool slot");
    if (replacement_object != NULL) {
        TEST_ASSERT(((kobject_header_t *)replacement_object)->generation !=
                        factory_generation,
                    "reused Factory keeps advanced generation");
    }
    if (replacement >= 0) {
        (void)cap_revoke_for(owner, replacement);
    }

    (void)task_delete(owner_id);
    (void)task_delete(other_id);
    TEST_ASSERT_EQ((int)free_before, (int)cap_free_count(),
                   "Factory authority test restores capability pool");
}

static void test_factory_revoke_while_pinned(void) {
    test_section("Test 2: Factory revoke waits for active operation");

    uint16_t free_before = cap_free_count();
    task_id_t owner_id = task_create_user("fac_pin",
                                           factory_placeholder_task,
                                           NULL, 10, 512);
    TEST_ASSERT(owner_id >= 0, "pinned Factory owner created");
    if (owner_id < 0) {
        return;
    }

    tcb_t *owner = task_get_tcb(owner_id);
    cap_id_t root = factory_create_root_cap(
        owner, FACTORY_OBJECT_BIT(CAP_OBJ_SEMAPHORE), CAP_FULL);
    TEST_ASSERT(root >= 0, "pinned Factory cap created");
    if (root < 0) {
        (void)task_delete(owner_id);
        return;
    }

    void *object = NULL;
    uint32_t mask = 0U;
    kern_err_t err = cap_object_pin_for(owner, root, CAP_OBJ_FACTORY,
                                         CAP_WRITE, &object, &mask);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "Factory operation pins object");
    TEST_ASSERT_EQ((int)FACTORY_OBJECT_BIT(CAP_OBJ_SEMAPHORE), (int)mask,
                   "pinned Factory returns authorization badge");
    if (err != KERN_OK || object == NULL) {
        (void)cap_revoke_for(owner, root);
        (void)task_delete(owner_id);
        return;
    }

    kobject_header_t *header = (kobject_header_t *)object;
    uint32_t generation = header->generation;
    TEST_ASSERT_EQ((int)KERN_OK, (int)cap_revoke_for(owner, root),
                   "last Factory cap may be revoked while pinned");
    TEST_ASSERT_EQ((int)generation, (int)header->generation,
                   "pinned Factory is not recycled early");
    TEST_ASSERT((header->flags & KOBJ_FLAG_CLEANUP_PENDING) != 0U,
                "pinned Factory records deferred cleanup");

    TEST_ASSERT_EQ((int)KERN_OK,
                   (int)cap_object_unpin(object, CAP_OBJ_FACTORY),
                   "Factory operation releases object pin");
    TEST_ASSERT(header->generation != generation,
                "Factory recycles after final pin release");

    (void)task_delete(owner_id);
    TEST_ASSERT_EQ((int)free_before, (int)cap_free_count(),
                   "pinned Factory test restores capability pool");
}

static void test_factory_module(void) {
    test_factory_authority_and_lifecycle();
    test_factory_revoke_while_pinned();
}

TEST_K_MODULE(factory, test_factory_module);

#endif /* TEST_ENABLE && CAP_ENABLE && MPU_ENABLE */
