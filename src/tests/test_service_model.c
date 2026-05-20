/**
 * @file test_service_model.c
 * @brief Root/init bootstrap and service-model tests
 */

#include "test_framework.h"
#include "root_bootstrap.h"
#include "capability.h"
#include "endpoint.h"
#include "user_api.h"
#include "task.h"

#if TEST_ENABLE && CAP_ENABLE && MPU_ENABLE

static void root_dummy_task(void *arg) {
    (void)arg;
    task_exit(NULL);
}

static void service_ipc_task(void *arg) {
    int ep_cap = (int)(uintptr_t)arg;
    uint8_t msg_buf[KERN_EP_MSG_SIZE];
    uint32_t *msg = (uint32_t *)msg_buf;
    if (ep_cap <= 0) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_PARAM);
    }
    for (uint32_t i = 0; i < sizeof(msg_buf); i++) {
        msg_buf[i] = 0;
    }

    int err = sys_ep_recv(ep_cap, msg_buf, 1000);

    if (err == KERN_OK) {
        if (msg[0] != 0x7100U) {
            err = KERN_ERR_STATE;
        } else {
            msg[0] = 0x7101U;
            msg[1] += 7U;
            err = sys_ep_reply(ep_cap, msg_buf);
        }
    }

    sys_task_exit((void *)(intptr_t)err);
}

static void test_root_bootstrap_rejects_invalid_tasks(void) {
    test_section("Test 1: root bootstrap rejects invalid tasks");

    root_bootstrap_init();

    kern_err_t err = root_bootstrap_prepare(NULL);
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, (int)err,
                   "root bootstrap rejects NULL task");

    task_id_t tid = task_create("root_priv", root_dummy_task, NULL, 12, 512);
    TEST_ASSERT(tid >= 0, "privileged bootstrap test task created");
    if (tid >= 0) {
        tcb_t *tcb = task_get_tcb(tid);
        err = root_bootstrap_prepare(tcb);
        TEST_ASSERT_EQ((int)KERN_ERR_PERM, (int)err,
                       "root bootstrap rejects privileged compatibility task");
        task_delete(tid);
    }

    root_bootstrap_info_t info;
    err = root_bootstrap_get_info(&info);
    TEST_ASSERT_EQ((int)KERN_ERR_NOEXIST, (int)err,
                   "root bootstrap remains inactive after rejects");
}

static void test_root_bootstrap_initial_task_cap(void) {
    test_section("Test 2: root bootstrap initial task cap");

    root_bootstrap_init();

    uint16_t cap_free = cap_free_count();
    task_id_t tid = KERN_INVALID_ID;
    kern_err_t err = root_bootstrap_create("root_init", root_dummy_task,
                                           NULL, 12, 512, &tid);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "root bootstrap create OK");
    TEST_ASSERT(tid >= 0, "root/init user task created");
    if (err != KERN_OK || tid < 0) {
        return;
    }

    tcb_t *root = task_get_tcb(tid);

    root_bootstrap_info_t info;
    err = root_bootstrap_get_info(&info);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "root bootstrap info available");
    TEST_ASSERT_EQ((int)tid, (int)info.task_id,
                   "root bootstrap records task id");
    TEST_ASSERT(info.root_endpoint >= 0,
                "root bootstrap records root endpoint");
    TEST_ASSERT_EQ(2, (int)info.cap_count,
                   "root bootstrap records initial caps");

    if (err == KERN_OK && info.cap_count > 1) {
        TEST_ASSERT_EQ((int)CAP_OBJ_TASK, (int)info.caps[0].obj_type,
                       "root bootstrap cap type is task");
        TEST_ASSERT_EQ((int)CAP_FULL, (int)info.caps[0].rights,
                       "root bootstrap task cap has full rights");

        void *resolved = cap_lookup_for(root, info.caps[0].cap,
                                        CAP_OBJ_TASK, CAP_MANAGE);
        TEST_ASSERT_EQ((uintptr_t)(tid + 1), (uintptr_t)resolved,
                       "root task cap resolves task id object");

        TEST_ASSERT_EQ((int)CAP_OBJ_ENDPOINT, (int)info.caps[1].obj_type,
                       "root bootstrap cap type is endpoint");
        TEST_ASSERT_EQ((int)CAP_FULL, (int)info.caps[1].rights,
                       "root bootstrap endpoint cap has full rights");

        void *ep_obj = cap_lookup_for(root, info.caps[1].cap,
                                      CAP_OBJ_ENDPOINT, CAP_WRITE);
        TEST_ASSERT_NOT_NULL(ep_obj, "root task can resolve initial endpoint cap");
        if (ep_obj != NULL) {
            ep_id_t ep_id = (ep_id_t)((uintptr_t)ep_obj - 1U);
            TEST_ASSERT_EQ((int)info.root_endpoint, (int)ep_id,
                           "root endpoint cap matches bootstrap record");
            TEST_ASSERT(endpoint_exists(ep_id),
                        "root bootstrap endpoint exists");
        }
    }

    err = root_bootstrap_prepare(root);
    TEST_ASSERT_EQ((int)KERN_ERR_BUSY, (int)err,
                   "root bootstrap rejects duplicate prepare");

    err = task_delete(tid);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "root/init test task deleted");

    err = root_bootstrap_get_info(&info);
    TEST_ASSERT_EQ((int)KERN_ERR_NOEXIST, (int)err,
                   "root bootstrap clears on task delete");
    TEST_ASSERT_EQ((int)cap_free, (int)cap_free_count(),
                   "root bootstrap task cap released");
}

static void test_root_bootstrap_create_validation(void) {
    test_section("Test 3: root bootstrap create validation");

    root_bootstrap_init();

    uint16_t cap_free = cap_free_count();
    task_id_t tid = 123;
    kern_err_t err = root_bootstrap_create("bad_root", NULL, NULL,
                                           12, 512, &tid);
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, (int)err,
                   "root bootstrap create rejects NULL entry");
    TEST_ASSERT_EQ(123, (int)tid,
                   "root bootstrap leaves out task untouched on reject");
    TEST_ASSERT_EQ((int)cap_free, (int)cap_free_count(),
                   "root bootstrap reject does not consume caps");

    err = root_bootstrap_create("root_busy", root_dummy_task,
                                NULL, 12, 512, &tid);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "root bootstrap create first root OK");
    TEST_ASSERT(tid >= 0, "root bootstrap create returned task id");

    task_id_t second = KERN_INVALID_ID;
    err = root_bootstrap_create("root_busy2", root_dummy_task,
                                NULL, 12, 512, &second);
    TEST_ASSERT_EQ((int)KERN_ERR_BUSY, (int)err,
                   "root bootstrap create rejects duplicate root");
    TEST_ASSERT_EQ((int)KERN_INVALID_ID, (int)second,
                   "duplicate root does not create task");

    if (tid >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(tid),
                       "root bootstrap validation task deleted");
    }
    TEST_ASSERT_EQ((int)cap_free, (int)cap_free_count(),
                   "root bootstrap validation cleanup restored caps");
}

static void test_root_bootstrap_start_policy(void) {
    test_section("Test 4: root bootstrap start policy");

    root_bootstrap_init();

    kern_err_t err = root_bootstrap_start();
    TEST_ASSERT_EQ((int)KERN_ERR_NOEXIST, (int)err,
                   "root bootstrap start rejects missing root");

    uint16_t cap_free = cap_free_count();
    task_id_t tid = KERN_INVALID_ID;
    err = root_bootstrap_create("root_start", root_dummy_task,
                                NULL, 12, 512, &tid);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "root bootstrap start test creates root");
    TEST_ASSERT(tid >= 0, "root bootstrap start test task id valid");

    root_bootstrap_info_t info;
    err = root_bootstrap_get_info(&info);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "root bootstrap start test info available");
    TEST_ASSERT_EQ(0, (int)info.started,
                   "root bootstrap records not-started state");

    err = root_bootstrap_start();
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "root bootstrap start OK");

    err = root_bootstrap_get_info(&info);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "root bootstrap started info available");
    TEST_ASSERT_EQ(1, (int)info.started,
                   "root bootstrap records started state");

    err = root_bootstrap_start();
    TEST_ASSERT_EQ((int)KERN_ERR_STATE, (int)err,
                   "root bootstrap rejects duplicate start");

    if (tid >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(tid),
                       "root bootstrap started task deleted");
    }

    err = root_bootstrap_get_info(&info);
    TEST_ASSERT_EQ((int)KERN_ERR_NOEXIST, (int)err,
                   "root bootstrap clears started root on delete");
    TEST_ASSERT_EQ((int)cap_free, (int)cap_free_count(),
                   "root bootstrap start cleanup restored caps");
}

static void test_root_bootstrap_create_service(void) {
    test_section("Test 5: root bootstrap creates service task");

    root_bootstrap_init();

    kern_err_t err = root_bootstrap_create_service("svc_bad", root_dummy_task,
                                                   NULL, 12, 512,
                                                   NULL, NULL);
    TEST_ASSERT_EQ((int)KERN_ERR_NOEXIST, (int)err,
                   "root service create rejects missing root");

    task_id_t root_id = KERN_INVALID_ID;
    err = root_bootstrap_create("root_svc", root_dummy_task,
                                NULL, 12, 512, &root_id);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "root service test creates root");
    if (err != KERN_OK || root_id < 0) {
        return;
    }

    uint16_t cap_free_after_root = cap_free_count();
    task_id_t service_id = KERN_INVALID_ID;
    cap_id_t service_cap = KERN_INVALID_ID;

    err = root_bootstrap_create_service("svc_null", NULL, NULL, 12, 512,
                                        &service_id, &service_cap);
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, (int)err,
                   "root service create rejects NULL entry");
    TEST_ASSERT_EQ((int)KERN_INVALID_ID, (int)service_id,
                   "NULL service create does not return task");
    TEST_ASSERT_EQ((int)KERN_INVALID_ID, (int)service_cap,
                   "NULL service create does not return cap");
    TEST_ASSERT_EQ((int)cap_free_after_root, (int)cap_free_count(),
                   "NULL service create does not consume caps");

    err = root_bootstrap_create_service("svc", root_dummy_task, NULL,
                                        13, 512,
                                        &service_id, &service_cap);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "root service create OK");
    TEST_ASSERT(service_id >= 0, "root service task id valid");
    TEST_ASSERT(service_cap >= 0, "root receives service task cap");

    tcb_t *root = task_get_tcb(root_id);
    void *resolved = cap_lookup_for(root, service_cap,
                                    CAP_OBJ_TASK, CAP_MANAGE);
    TEST_ASSERT_EQ((uintptr_t)(service_id + 1), (uintptr_t)resolved,
                   "root service cap resolves service task id");

    if (service_id >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(service_id),
                       "root service task deleted");
    }
    if (root_id >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(root_id),
                       "root service test root deleted");
    }
    TEST_ASSERT_EQ((int)cap_free_after_root + 2, (int)cap_free_count(),
                   "root service cleanup restored all caps");
}

static void test_root_bootstrap_start_service(void) {
    test_section("Test 6: root bootstrap starts service via cap");

    root_bootstrap_init();

    kern_err_t err = root_bootstrap_start_service(KERN_INVALID_ID);
    TEST_ASSERT_EQ((int)KERN_ERR_NOEXIST, (int)err,
                   "root service start rejects missing root");

    task_id_t root_id = KERN_INVALID_ID;
    err = root_bootstrap_create("root_stsvc", root_dummy_task,
                                NULL, 12, 512, &root_id);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "root service start test creates root");
    if (err != KERN_OK || root_id < 0) {
        return;
    }

    uint16_t cap_free_after_root = cap_free_count();
    err = root_bootstrap_start_service(KERN_INVALID_ID);
    TEST_ASSERT_EQ((int)KERN_ERR_CAP, (int)err,
                   "root service start rejects invalid cap");

    root_bootstrap_info_t info;
    err = root_bootstrap_get_info(&info);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "root service start gets bootstrap info");
    if (err == KERN_OK && info.cap_count > 0) {
        err = root_bootstrap_start_service(info.caps[0].cap);
        TEST_ASSERT_EQ((int)KERN_ERR_PARAM, (int)err,
                       "root service start rejects root self cap");
    }

    task_id_t service_id = KERN_INVALID_ID;
    cap_id_t service_cap = KERN_INVALID_ID;
    err = root_bootstrap_create_service("svc_start", root_dummy_task,
                                        NULL, 13, 512,
                                        &service_id, &service_cap);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "root service start creates service");
    TEST_ASSERT(service_id >= 0, "root service start task id valid");
    TEST_ASSERT(service_cap >= 0, "root service start cap valid");

    err = root_bootstrap_start_service(service_cap);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "root service start via cap OK");

    if (service_id >= 0) {
        TEST_ASSERT_EQ((int)TASK_STATE_READY,
                       (int)task_get_state(service_id),
                       "root service task is ready after start");
    }

    err = root_bootstrap_start_service(service_cap);
    TEST_ASSERT_EQ((int)KERN_ERR_STATE, (int)err,
                   "root service duplicate start rejected");

    if (service_id >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(service_id),
                       "root started service task deleted");
    }
    if (root_id >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(root_id),
                       "root service start root deleted");
    }
    TEST_ASSERT_EQ((int)cap_free_after_root + 2, (int)cap_free_count(),
                   "root service start cleanup restored all caps");
}

static void test_root_bootstrap_service_endpoint(void) {
    test_section("Test 7: root bootstrap service endpoint caps");

    root_bootstrap_init();

    kern_err_t err =
        root_bootstrap_create_service_endpoint(KERN_INVALID_ID, "svc_ep",
                                               16, 2, NULL, NULL, NULL);
    TEST_ASSERT_EQ((int)KERN_ERR_NOEXIST, (int)err,
                   "service endpoint create rejects missing root");

    task_id_t root_id = KERN_INVALID_ID;
    err = root_bootstrap_create("root_ep", root_dummy_task,
                                NULL, 12, 512, &root_id);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "service endpoint test creates root");
    if (err != KERN_OK || root_id < 0) {
        return;
    }

    uint16_t cap_free_after_root = cap_free_count();

    err = root_bootstrap_create_service_endpoint(KERN_INVALID_ID, "svc_ep",
                                                 16, 2, NULL, NULL, NULL);
    TEST_ASSERT_EQ((int)KERN_ERR_CAP, (int)err,
                   "service endpoint create rejects invalid service cap");

    root_bootstrap_info_t info;
    err = root_bootstrap_get_info(&info);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "service endpoint test gets bootstrap info");
    if (err == KERN_OK && info.cap_count > 0) {
        err = root_bootstrap_create_service_endpoint(info.caps[0].cap,
                                                     "svc_ep", 16, 2,
                                                     NULL, NULL, NULL);
        TEST_ASSERT_EQ((int)KERN_ERR_PARAM, (int)err,
                       "service endpoint create rejects root self cap");
    }

    task_id_t service_id = KERN_INVALID_ID;
    cap_id_t service_task_cap = KERN_INVALID_ID;
    err = root_bootstrap_create_service("svc_ep_task", root_dummy_task,
                                        NULL, 13, 512,
                                        &service_id, &service_task_cap);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "service endpoint test creates service");
    TEST_ASSERT(service_id >= 0, "service endpoint service id valid");
    TEST_ASSERT(service_task_cap >= 0, "service endpoint service task cap valid");

    ep_id_t ep_id = KERN_INVALID_ID;
    cap_id_t root_ep_cap = KERN_INVALID_ID;
    cap_id_t service_ep_cap = KERN_INVALID_ID;
    err = root_bootstrap_create_service_endpoint(service_task_cap, "svc_ep",
                                                 16, 2,
                                                 &ep_id,
                                                 &root_ep_cap,
                                                 &service_ep_cap);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "service endpoint create OK");
    TEST_ASSERT(ep_id >= 0, "service endpoint id valid");
    TEST_ASSERT(root_ep_cap >= 0, "root receives service endpoint cap");
    TEST_ASSERT(service_ep_cap >= 0, "service receives endpoint cap");
    TEST_ASSERT(endpoint_exists(ep_id), "service endpoint exists");

    tcb_t *root = task_get_tcb(root_id);
    tcb_t *service = task_get_tcb(service_id);

    void *root_ep = cap_lookup_for(root, root_ep_cap,
                                   CAP_OBJ_ENDPOINT, CAP_MANAGE);
    TEST_ASSERT_EQ((uintptr_t)(ep_id + 1), (uintptr_t)root_ep,
                   "root service endpoint cap resolves endpoint");

    void *service_ep = cap_lookup_for(service, service_ep_cap,
                                      CAP_OBJ_ENDPOINT, CAP_READ | CAP_WRITE);
    TEST_ASSERT_EQ((uintptr_t)(ep_id + 1), (uintptr_t)service_ep,
                   "service endpoint cap resolves endpoint");

    service_ep = cap_lookup_for(service, service_ep_cap,
                                CAP_OBJ_ENDPOINT, CAP_MANAGE);
    TEST_ASSERT_NULL(service_ep,
                     "service endpoint cap lacks manage rights");

    if (service_id >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(service_id),
                       "service endpoint task deleted");
    }
    TEST_ASSERT(!endpoint_exists(ep_id),
                "service endpoint deleted with service task");

    if (root_id >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(root_id),
                       "service endpoint test root deleted");
    }
    TEST_ASSERT_EQ((int)cap_free_after_root + 2, (int)cap_free_count(),
                   "service endpoint cleanup restored all caps");
}

static void test_root_bootstrap_service_ipc(void) {
    test_section("Test 8: root bootstrap service IPC");

    root_bootstrap_init();

    task_id_t root_id = KERN_INVALID_ID;
    kern_err_t err = root_bootstrap_create("root_ipc", root_dummy_task,
                                           NULL, 12, 512, &root_id);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "service IPC test creates root");
    if (err != KERN_OK || root_id < 0) {
        return;
    }

    uint16_t cap_free_after_root = cap_free_count();
    task_id_t service_id = KERN_INVALID_ID;
    cap_id_t service_task_cap = KERN_INVALID_ID;
    err = root_bootstrap_create_service("svc_ipc", service_ipc_task,
                                        NULL, 13, 512,
                                        &service_id, &service_task_cap);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "service IPC creates service");

    ep_id_t ep_id = KERN_INVALID_ID;
    cap_id_t root_ep_cap = KERN_INVALID_ID;
    cap_id_t service_ep_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = root_bootstrap_create_service_endpoint(service_task_cap,
                                                     "svc_ipc_ep",
                                                     sizeof(uint32_t) * 4U,
                                                     2,
                                                     &ep_id,
                                                     &root_ep_cap,
                                                     &service_ep_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "service IPC creates endpoint");
    TEST_ASSERT(ep_id >= 0, "service IPC endpoint id valid");
    TEST_ASSERT(root_ep_cap >= 0, "service IPC root endpoint cap valid");
    TEST_ASSERT(service_ep_cap >= 0, "service IPC service endpoint cap valid");

    if (err == KERN_OK) {
        err = root_bootstrap_start_service(service_task_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "service IPC starts service");

    uint32_t msg[4] = {0x7100U, 35U, 0U, 0U};
    if (err == KERN_OK) {
        err = endpoint_send(ep_id, msg, 1000);
    }
    if (err != KERN_OK) {
        test_print_num("[DIAG] service IPC endpoint_send err=", err);
        test_print_num("[DIAG] service IPC service_ep_cap=", service_ep_cap);
        test_print_num("[DIAG] service IPC ep_id=", ep_id);
        if (service_id >= 0) {
            test_print_num("[DIAG] service IPC service_state=",
                           (int32_t)task_get_state(service_id));
        }
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "service IPC endpoint_send OK");
    TEST_ASSERT_EQ((int)0x7101U, (int)msg[0],
                   "service IPC reply opcode OK");
    TEST_ASSERT_EQ(42, (int)msg[1],
                   "service IPC reply payload OK");

    void *retval = NULL;
    if (service_id >= 0) {
        err = task_join(service_id, &retval, 1000);
        if (err != KERN_OK || (intptr_t)retval != KERN_OK) {
            test_print_num("[DIAG] service IPC join err=", err);
            test_print_num("[DIAG] service IPC retval=",
                           (int32_t)(intptr_t)retval);
        }
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "service IPC service exited OK");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "service IPC service retval OK");
    }

    if (service_id >= 0 && task_get_state(service_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(service_id);
    }
    if (root_id >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(root_id),
                       "service IPC root deleted");
    }
    TEST_ASSERT_EQ((int)cap_free_after_root + 2, (int)cap_free_count(),
                   "service IPC cleanup restored all caps");
}

#endif

void test_service_model_module(void) {
#if TEST_ENABLE && CAP_ENABLE && MPU_ENABLE
    test_root_bootstrap_rejects_invalid_tasks();
    test_root_bootstrap_initial_task_cap();
    test_root_bootstrap_create_validation();
    test_root_bootstrap_start_policy();
    test_root_bootstrap_create_service();
    test_root_bootstrap_start_service();
    test_root_bootstrap_service_endpoint();
    test_root_bootstrap_service_ipc();
#else
    test_print("Service model tests disabled\r\n");
#endif
}

TEST_MODULE_REGISTER(service_model, test_service_model_module);
