/**
 * @file test_service_model.c
 * @brief Root/init bootstrap and service-model tests
 */

#include "test_framework.h"
#include "root_bootstrap.h"
#include "capability.h"
#include "endpoint.h"
#include "factory.h"
#include "user_api.h"
#include "task.h"
#include "nameserver.h"
#include "fs_proto.h"
#include "fs_runtime.h"
#include "supervisor.h"
#include "fs_types.h"   /* Phase F2: dirent_t/vfs_stat_t (原在 inode.h) */
#include <string.h>

#if TEST_ENABLE && CAP_ENABLE && MPU_ENABLE

static void root_dummy_task(void *arg) {
    (void)arg;
    /* root_bootstrap_create() creates a USER task.  Calling the kernel-only
     * task_exit() directly from it faults as soon as the peer CPU actually
     * schedules the task.  Stay alive through the user syscall ABI until the
     * test explicitly deletes this placeholder service. */
    for (;;) {
        (void)sys_task_delay(1000);
    }
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

static void nameserver_ping_task(void *arg) {
    int err = nameserver_service_run((int)(uintptr_t)arg, 1);
    sys_task_exit((void *)(intptr_t)err);
}

static void test_supervisor_service_stats(void) {
    test_section("Test 0: supervisor service stats");

    supervisor_service_t svc =
        SUPERVISOR_SERVICE_INIT(KERN_ERR_STATE);
    TEST_ASSERT(strcmp(supervisor_service_name(&svc), "(unnamed)") == 0,
                "supervisor unnamed service has fallback name");
    supervisor_set_service_name(&svc, "svc.test");
    TEST_ASSERT(strcmp(supervisor_service_name(&svc), "svc.test") == 0,
                "supervisor service name updates");
    TEST_ASSERT_EQ(0, (int)supervisor_restart_count(&svc),
                   "supervisor restart count starts at zero");
    TEST_ASSERT_EQ(0, (int)supervisor_recover_count(&svc),
                   "supervisor recover count starts at zero");
    TEST_ASSERT_EQ(0, (int)supervisor_fault_count(&svc),
                   "supervisor fault count starts at zero");
    TEST_ASSERT_EQ(0, (int)supervisor_pending_clients(&svc),
                   "supervisor pending clients start at zero");
    TEST_ASSERT_EQ((int)SUPERVISOR_RESTART_MANUAL,
                   (int)supervisor_restart_policy(&svc),
                   "supervisor restart policy starts manual");
    TEST_ASSERT_EQ(0, (int)supervisor_max_restarts(&svc),
                   "supervisor max restarts starts at zero");
    TEST_ASSERT_EQ((int)KERN_ERR_STATE,
                   supervisor_last_health(&svc),
                   "supervisor initial health recorded");

    supervisor_record_restart(&svc);
    supervisor_record_recover(&svc);
    supervisor_record_fault(&svc);
    supervisor_set_restart_policy(&svc, SUPERVISOR_RESTART_AUTO, 3);
    supervisor_client_blocked(&svc);
    supervisor_client_blocked(&svc);
    supervisor_client_unblocked(&svc);
    supervisor_set_health(&svc, KERN_OK);
    TEST_ASSERT_EQ(1, (int)supervisor_restart_count(&svc),
                   "supervisor restart count increments");
    TEST_ASSERT_EQ(1, (int)supervisor_recover_count(&svc),
                   "supervisor recover count increments");
    TEST_ASSERT_EQ(1, (int)supervisor_fault_count(&svc),
                   "supervisor fault count increments");
    TEST_ASSERT_EQ(1, (int)supervisor_pending_clients(&svc),
                   "supervisor pending clients track blocked clients");
    TEST_ASSERT_EQ((int)SUPERVISOR_RESTART_AUTO,
                   (int)supervisor_restart_policy(&svc),
                   "supervisor restart policy updates to auto");
    TEST_ASSERT_EQ(3, (int)supervisor_max_restarts(&svc),
                   "supervisor max restarts updates");
    TEST_ASSERT(supervisor_should_auto_restart(&svc),
                "supervisor auto restart allowed below max");
    TEST_ASSERT(strcmp(supervisor_restart_policy_name(
                           supervisor_restart_policy(&svc)),
                       "auto") == 0,
                "supervisor restart policy name reports auto");
    TEST_ASSERT_EQ((int)KERN_OK,
                   supervisor_last_health(&svc),
                   "supervisor health updates");

    supervisor_set_restart_policy(&svc,
                                  (supervisor_restart_policy_t)99,
                                  5);
    TEST_ASSERT_EQ((int)SUPERVISOR_RESTART_MANUAL,
                   (int)supervisor_restart_policy(&svc),
                   "supervisor invalid restart policy falls back to manual");
    TEST_ASSERT_EQ(0, (int)supervisor_max_restarts(&svc),
                   "supervisor invalid restart policy clears max restarts");
    TEST_ASSERT(!supervisor_should_auto_restart(&svc),
                "supervisor manual policy disables auto restart");

    supervisor_set_restart_policy(&svc, SUPERVISOR_RESTART_AUTO, 1);
    supervisor_record_restart(&svc);
    TEST_ASSERT(!supervisor_should_auto_restart(&svc),
                "supervisor auto restart stops at max");

    supervisor_record_recover(&svc);
    supervisor_record_fault(&svc);
    supervisor_clear_counts(&svc);
    TEST_ASSERT_EQ(0, (int)supervisor_restart_count(&svc),
                   "supervisor clear counts clears restart count");
    TEST_ASSERT_EQ(0, (int)supervisor_recover_count(&svc),
                   "supervisor clear counts clears recover count");
    TEST_ASSERT_EQ(0, (int)supervisor_fault_count(&svc),
                   "supervisor clear counts clears fault count");
    TEST_ASSERT_EQ((int)SUPERVISOR_RESTART_AUTO,
                   (int)supervisor_restart_policy(&svc),
                   "supervisor clear counts preserves policy");
    TEST_ASSERT_EQ(1, (int)supervisor_max_restarts(&svc),
                   "supervisor clear counts preserves max restarts");
    TEST_ASSERT_EQ((int)KERN_OK,
                   supervisor_last_health(&svc),
                   "supervisor clear counts preserves health");

    supervisor_reset_service(&svc, KERN_ERR_STATE);
    TEST_ASSERT(strcmp(supervisor_service_name(&svc), "svc.test") == 0,
                "supervisor reset preserves service name");
    TEST_ASSERT_EQ(0, (int)supervisor_restart_count(&svc),
                   "supervisor reset clears restart count");
    TEST_ASSERT_EQ(0, (int)supervisor_recover_count(&svc),
                   "supervisor reset clears recover count");
    TEST_ASSERT_EQ(0, (int)supervisor_fault_count(&svc),
                   "supervisor reset clears fault count");
    TEST_ASSERT_EQ(0, (int)supervisor_pending_clients(&svc),
                   "supervisor reset clears pending clients");
    TEST_ASSERT_EQ((int)SUPERVISOR_RESTART_MANUAL,
                   (int)supervisor_restart_policy(&svc),
                   "supervisor reset restores manual policy");
    TEST_ASSERT_EQ(0, (int)supervisor_max_restarts(&svc),
                   "supervisor reset clears max restarts");
    TEST_ASSERT_EQ((int)KERN_ERR_STATE,
                   supervisor_last_health(&svc),
                   "supervisor reset updates health");

    supervisor_set_pending_clients(&svc, 0);
    supervisor_client_unblocked(&svc);
    TEST_ASSERT_EQ(0, (int)supervisor_pending_clients(&svc),
                   "supervisor pending clients do not underflow");

    supervisor_service_init(&svc, KERN_ERR_NOEXIST);
    TEST_ASSERT(strcmp(supervisor_service_name(&svc), "(unnamed)") == 0,
                "supervisor init clears service name");
    TEST_ASSERT_EQ(0, (int)supervisor_restart_count(&svc),
                   "supervisor init clears restart count");
    TEST_ASSERT_EQ(0, (int)supervisor_recover_count(&svc),
                   "supervisor init clears recover count");
    TEST_ASSERT_EQ(0, (int)supervisor_fault_count(&svc),
                   "supervisor init clears fault count");
    TEST_ASSERT_EQ(0, (int)supervisor_pending_clients(&svc),
                   "supervisor init clears pending clients");
    TEST_ASSERT_EQ((int)SUPERVISOR_RESTART_MANUAL,
                   (int)supervisor_restart_policy(&svc),
                   "supervisor init resets restart policy");
    TEST_ASSERT_EQ(0, (int)supervisor_max_restarts(&svc),
                   "supervisor init clears max restarts");
    TEST_ASSERT_EQ((int)KERN_ERR_NOEXIST,
                   supervisor_last_health(&svc),
                   "supervisor init resets health");

    supervisor_record_restart(NULL);
    supervisor_record_recover(NULL);
    supervisor_record_fault(NULL);
    supervisor_set_pending_clients(NULL, 1);
    supervisor_client_blocked(NULL);
    supervisor_client_unblocked(NULL);
    supervisor_set_health(NULL, KERN_OK);
    supervisor_clear_counts(NULL);
    supervisor_reset_service(NULL, KERN_ERR_STATE);
    TEST_ASSERT_EQ(0, (int)supervisor_restart_count(NULL),
                   "supervisor NULL restart count safe");
    TEST_ASSERT_EQ(0, (int)supervisor_recover_count(NULL),
                   "supervisor NULL recover count safe");
    TEST_ASSERT_EQ(0, (int)supervisor_fault_count(NULL),
                   "supervisor NULL fault count safe");
    TEST_ASSERT_EQ(0, (int)supervisor_pending_clients(NULL),
                   "supervisor NULL pending clients safe");
    TEST_ASSERT_EQ((int)SUPERVISOR_RESTART_MANUAL,
                   (int)supervisor_restart_policy(NULL),
                   "supervisor NULL restart policy safe");
    TEST_ASSERT_EQ(0, (int)supervisor_max_restarts(NULL),
                   "supervisor NULL max restarts safe");
    TEST_ASSERT(!supervisor_should_auto_restart(NULL),
                "supervisor NULL auto restart decision safe");
    TEST_ASSERT(strcmp(supervisor_service_name(NULL), "(unnamed)") == 0,
                "supervisor NULL service name safe");
    TEST_ASSERT(strcmp(supervisor_restart_policy_name(
                           SUPERVISOR_RESTART_MANUAL),
                       "manual") == 0,
                "supervisor restart policy name reports manual");
    supervisor_restart_policy_t parsed_policy = SUPERVISOR_RESTART_MANUAL;
    TEST_ASSERT_EQ((int)KERN_OK,
                   supervisor_parse_restart_policy("auto", &parsed_policy),
                   "supervisor parses auto restart policy");
    TEST_ASSERT_EQ((int)SUPERVISOR_RESTART_AUTO,
                   (int)parsed_policy,
                   "supervisor parsed auto restart policy value");
    TEST_ASSERT_EQ((int)KERN_OK,
                   supervisor_parse_restart_policy("manual", &parsed_policy),
                   "supervisor parses manual restart policy");
    TEST_ASSERT_EQ((int)SUPERVISOR_RESTART_MANUAL,
                   (int)parsed_policy,
                   "supervisor parsed manual restart policy value");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   supervisor_parse_restart_policy("bad", &parsed_policy),
                   "supervisor rejects invalid restart policy string");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   supervisor_parse_restart_policy(NULL, &parsed_policy),
                   "supervisor rejects NULL restart policy string");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   supervisor_parse_restart_policy("auto", NULL),
                   "supervisor rejects NULL restart policy output");

    supervisor_service_init_named(&svc, "svc.named", KERN_ERR_STATE);
    TEST_ASSERT(strcmp(supervisor_service_name(&svc), "svc.named") == 0,
                "supervisor named init records service name");

    supervisor_registry_init();
    TEST_ASSERT_EQ(0, (int)supervisor_service_count(),
                   "supervisor registry starts empty");

    supervisor_service_t *a =
        supervisor_register_service("svc.a", KERN_ERR_STATE);
    supervisor_service_t *b =
        supervisor_register_service("svc.b", KERN_ERR_NOEXIST);
    TEST_ASSERT(a != NULL, "supervisor registry registers first service");
    TEST_ASSERT(b != NULL, "supervisor registry registers second service");
    TEST_ASSERT_EQ(2, (int)supervisor_service_count(),
                   "supervisor registry counts services");
    TEST_ASSERT(supervisor_find_service("svc.a") == a,
                "supervisor registry finds service by name");
    TEST_ASSERT(supervisor_register_service("svc.a", KERN_OK) == a,
                "supervisor registry duplicate returns existing service");
    TEST_ASSERT_EQ(2, (int)supervisor_service_count(),
                   "supervisor registry duplicate does not allocate");
    TEST_ASSERT(supervisor_service_at(0) == a,
                "supervisor registry iterates first service");
    TEST_ASSERT(supervisor_service_at(1) == b,
                "supervisor registry iterates second service");
    TEST_ASSERT(supervisor_service_at(2) == NULL,
                "supervisor registry iteration stops at count");

    TEST_ASSERT(supervisor_register_service("svc.c", KERN_ERR_STATE) != NULL,
                "supervisor registry registers third service");
    TEST_ASSERT(supervisor_register_service("svc.d", KERN_ERR_STATE) != NULL,
                "supervisor registry registers fourth service");
    TEST_ASSERT(supervisor_register_service("svc.e", KERN_ERR_STATE) == NULL,
                "supervisor registry reports full table");
    TEST_ASSERT(supervisor_register_service(NULL, KERN_ERR_STATE) == NULL,
                "supervisor registry rejects NULL service name");

    supervisor_registry_init();
}

static void nameserver_register_once_task(void *arg) {
    int err = nameserver_service_run((int)(uintptr_t)arg, 1);
    sys_task_exit((void *)(intptr_t)err);
}

static void nameserver_register_lookup_task(void *arg) {
    int err = nameserver_service_run((int)(uintptr_t)arg, 2);
    sys_task_exit((void *)(intptr_t)err);
}

static void nameserver_negative_task(void *arg) {
    int err = nameserver_service_run((int)(uintptr_t)arg, 3);
    sys_task_exit((void *)(intptr_t)err);
}

static void nameserver_unregister_task(void *arg) {
    int err = nameserver_service_run((int)(uintptr_t)arg, 3);
    sys_task_exit((void *)(intptr_t)err);
}

static void nameserver_protocol_errors_task(void *arg) {
    int err = nameserver_service_run((int)(uintptr_t)arg, 6);
    sys_task_exit((void *)(intptr_t)err);
}

static void nameserver_recycle_task(void *arg) {
    int err = nameserver_service_run((int)(uintptr_t)arg, 36);
    sys_task_exit((void *)(intptr_t)err);
}

static void nameserver_registry_full_task(void *arg) {
    int err = nameserver_service_run((int)(uintptr_t)arg,
                                     NS_REGISTRY_MAX + 1U);
    sys_task_exit((void *)(intptr_t)err);
}

static void nameserver_owner_task(void *arg) {
    int err = nameserver_service_run((int)(uintptr_t)arg, 3);
    sys_task_exit((void *)(intptr_t)err);
}

static void nameserver_fs_runtime_task(void *arg) {
    int err = nameserver_service_run((int)(uintptr_t)arg, 5);
    sys_task_exit((void *)(intptr_t)err);
}

static void fs_service_session_task(void *arg) {
    int err = fs_server_run((int)(uintptr_t)arg, 19);
    sys_task_exit((void *)(intptr_t)err);
}

static void fs_service_negative_task(void *arg) {
    int err = fs_server_run((int)(uintptr_t)arg, 8);
    sys_task_exit((void *)(intptr_t)err);
}

static void fs_client_session_task(void *arg) {
    int fs_ep_cap = (int)(uintptr_t)arg;
    char buf[FS_PAYLOAD_MAX];
    const char payload[] = "fs-ok";
    dirent_t entry;
    int fd;
    int err;

    if (fs_ep_cap <= 0) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_PARAM);
    }

    err = fs_ping(fs_ep_cap, 1000);
    if (err != KERN_OK) {
        sys_task_exit((void *)(intptr_t)err);
    }

    fd = fs_open(fs_ep_cap, "/tmp/fssvc",
                 O_RDWR | O_CREAT | O_TRUNC, 1000);
    if (fd <= 0) {
        sys_task_exit((void *)(intptr_t)fd);
    }

    err = fs_write(fs_ep_cap, fd, payload,
                   (uint32_t)(sizeof(payload) - 1U), 1000);
    if (err != (int)(sizeof(payload) - 1U)) {
        (void)fs_close(fs_ep_cap, fd, 1000);
        sys_task_exit((void *)(intptr_t)KERN_ERR_STATE);
    }

    err = fs_lseek(fs_ep_cap, fd, 0, SEEK_SET, 1000);
    if (err != 0) {
        (void)fs_close(fs_ep_cap, fd, 1000);
        sys_task_exit((void *)(intptr_t)KERN_ERR_STATE);
    }

    for (uint32_t i = 0; i < sizeof(buf); i++) {
        buf[i] = 0;
    }
    err = fs_read(fs_ep_cap, fd, buf,
                  (uint32_t)(sizeof(payload) - 1U), 1000);
    if (err != (int)(sizeof(payload) - 1U)) {
        (void)fs_close(fs_ep_cap, fd, 1000);
        sys_task_exit((void *)(intptr_t)KERN_ERR_STATE);
    }
    for (uint32_t i = 0; i < sizeof(payload) - 1U; i++) {
        if (buf[i] != payload[i]) {
            (void)fs_close(fs_ep_cap, fd, 1000);
            sys_task_exit((void *)(intptr_t)KERN_ERR_STATE);
        }
    }

    err = fs_close(fs_ep_cap, fd, 1000);
    if (err != KERN_OK) {
        sys_task_exit((void *)(intptr_t)err);
    }

    vfs_stat_t st;
    err = fs_stat(fs_ep_cap, "/tmp/fssvc", &st, 1000);
    if (err != KERN_OK || st.type != INODE_TYPE_FILE ||
        st.size != (uint32_t)(sizeof(payload) - 1U)) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_STATE);
    }

    err = fs_unlink(fs_ep_cap, "/tmp/fssvc", 1000);
    if (err != KERN_OK) {
        sys_task_exit((void *)(intptr_t)err);
    }

    fd = fs_open(fs_ep_cap, "/tmp/fssvc", O_RDONLY, 1000);
    if (fd != KERN_ERR_NOEXIST) {
        if (fd > 0) {
            (void)fs_close(fs_ep_cap, fd, 1000);
        }
        sys_task_exit((void *)(intptr_t)KERN_ERR_STATE);
    }

    err = fs_mkdir(fs_ep_cap, "/tmp/fsdir", 1000);
    if (err != KERN_OK) {
        sys_task_exit((void *)(intptr_t)err);
    }

    err = fs_stat(fs_ep_cap, "/tmp/fsdir", &st, 1000);
    if (err != KERN_OK || st.type != INODE_TYPE_DIR) {
        (void)fs_unlink(fs_ep_cap, "/tmp/fsdir", 1000);
        sys_task_exit((void *)(intptr_t)KERN_ERR_STATE);
    }

    fd = fs_open(fs_ep_cap, "/tmp/fsdir", O_RDONLY, 1000);
    if (fd <= 0) {
        (void)fs_unlink(fs_ep_cap, "/tmp/fsdir", 1000);
        sys_task_exit((void *)(intptr_t)fd);
    }

    err = fs_readdir(fs_ep_cap, fd, &entry, 1000);
    if (err != KERN_OK) {
        (void)fs_close(fs_ep_cap, fd, 1000);
        (void)fs_unlink(fs_ep_cap, "/tmp/fsdir", 1000);
        sys_task_exit((void *)(intptr_t)err);
    }
    if (entry.name[0] != '.' || entry.name[1] != '\0' ||
        entry.type != INODE_TYPE_DIR) {
        (void)fs_close(fs_ep_cap, fd, 1000);
        (void)fs_unlink(fs_ep_cap, "/tmp/fsdir", 1000);
        sys_task_exit((void *)(intptr_t)KERN_ERR_STATE);
    }

    err = fs_close(fs_ep_cap, fd, 1000);
    if (err != KERN_OK) {
        (void)fs_unlink(fs_ep_cap, "/tmp/fsdir", 1000);
        sys_task_exit((void *)(intptr_t)err);
    }

    err = fs_unlink(fs_ep_cap, "/tmp/fsdir", 1000);
    if (err != KERN_OK) {
        sys_task_exit((void *)(intptr_t)err);
    }

    fd = fs_open(fs_ep_cap, "/tmp/fsdir", O_RDONLY, 1000);
    if (fd != KERN_ERR_NOEXIST) {
        if (fd > 0) {
            (void)fs_close(fs_ep_cap, fd, 1000);
        }
        sys_task_exit((void *)(intptr_t)KERN_ERR_STATE);
    }

    fd = fs_open(fs_ep_cap, "/tmp", O_RDONLY, 1000);
    if (fd <= 0) {
        sys_task_exit((void *)(intptr_t)fd);
    }

    err = fs_readdir(fs_ep_cap, fd, &entry, 1000);
    if (err != KERN_OK) {
        (void)fs_close(fs_ep_cap, fd, 1000);
        sys_task_exit((void *)(intptr_t)err);
    }
    if (entry.name[0] != '.' || entry.name[1] != '\0' ||
        entry.type != INODE_TYPE_DIR) {
        (void)fs_close(fs_ep_cap, fd, 1000);
        sys_task_exit((void *)(intptr_t)KERN_ERR_STATE);
    }

    err = fs_close(fs_ep_cap, fd, 1000);
    sys_task_exit((void *)(intptr_t)err);
}

static kern_err_t service_model_tmp_unlink(const char *name) {
    /* Phase F2:内核 VFS 移除后,vfs_lookup 不可用。
     * fs_server 测试用 O_CREAT|O_TRUNC 覆盖旧文件,unlink 只需返回 OK
     * (测试断言接受 OK 或 NOEXIST)。后续可改走 fs_unlink IPC。 */
    (void)name;
    return KERN_OK;
}

static void fs_test_copy_path(fs_msg_t *msg, const char *path) {
    for (uint32_t i = 0; i < FS_PATH_MAX; i++) {
        msg->path[i] = path[i];
        if (path[i] == '\0') {
            return;
        }
    }
    msg->path[FS_PATH_MAX - 1U] = '\0';
}

static void fs_test_send_expect(ep_id_t ep,
                                fs_msg_t *msg,
                                int expected_status,
                                const char *label) {
    kern_err_t err = endpoint_send(ep, msg, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, label);
    TEST_ASSERT_EQ(expected_status, (int)msg->status, label);
}

static void test_fs_runtime_state_helpers(void) {
    test_section("Test 22: FS runtime state helpers");

    fs_runtime_clear_name_server();
    fs_runtime_clear_inbox();

    TEST_ASSERT(!fs_runtime_name_server_bound(),
                "FS runtime reports name-server unbound");
    TEST_ASSERT_EQ((int)KERN_INVALID_ID,
                   (int)fs_runtime_name_server_cap(),
                   "FS runtime clears name-server cap");
    TEST_ASSERT_EQ((int)KERN_ERR_STATE,
                   fs_runtime_name_server_status(0),
                   "FS runtime reports unbound name-server");
    TEST_ASSERT(!fs_runtime_inbox_bound(),
                "FS runtime reports inbox unbound");
    TEST_ASSERT_EQ((int)KERN_INVALID_ID,
                   (int)fs_runtime_inbox_cap(),
                   "FS runtime clears inbox cap");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   fs_runtime_bind_name_server(0),
                   "FS runtime rejects bad name-server cap");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   fs_runtime_bind_owned_inbox(0),
                   "FS runtime rejects bad inbox cap");
    TEST_ASSERT_EQ((int)KERN_ERR_STATE,
                   fs_runtime_release_service(0),
                   "FS runtime release reports missing inbox");

    const char *ready_reason = NULL;
    TEST_ASSERT_EQ((int)KERN_ERR_STATE,
                   fs_runtime_lookup_ready(0, &ready_reason),
                   "FS runtime lookup ready rejects missing name-server");
    TEST_ASSERT(ready_reason != NULL &&
                strcmp(ready_reason, "name-server") == 0,
                "FS runtime lookup ready reports missing name-server");

    cap_id_t service_cap = 123;
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   fs_runtime_lookup_service(NULL, &service_cap, 0),
                   "FS runtime lookup rejects NULL name");
    TEST_ASSERT_EQ((int)KERN_INVALID_ID, (int)service_cap,
                   "FS runtime clears output on NULL name");
    service_cap = 123;
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   fs_runtime_lookup_service("fs.ramfs", NULL, 0),
                   "FS runtime lookup rejects NULL output");
    TEST_ASSERT_EQ(123, (int)service_cap,
                   "FS runtime leaves caller variable untouched on NULL output");

    TEST_ASSERT_EQ((int)KERN_OK,
                   fs_runtime_bind_name_server(123),
                   "FS runtime records name-server cap");
    TEST_ASSERT(fs_runtime_name_server_bound(),
                "FS runtime reports bound name-server");
    TEST_ASSERT_EQ(123, (int)fs_runtime_name_server_cap(),
                   "FS runtime returns name-server cap");
    TEST_ASSERT(fs_runtime_name_server_status(0) != KERN_OK,
                "FS runtime bad name-server cap reports non-OK");
    ready_reason = NULL;
    TEST_ASSERT(fs_runtime_lookup_ready(0, &ready_reason) != KERN_OK,
                "FS runtime lookup ready rejects bad name-server cap");
    TEST_ASSERT(ready_reason != NULL &&
                strcmp(ready_reason, "name-server") == 0,
                "FS runtime lookup ready reports bad name-server");
    service_cap = 123;
    TEST_ASSERT_EQ((int)KERN_ERR_STATE,
                   fs_runtime_lookup_service("fs.ramfs", &service_cap, 0),
                   "FS runtime lookup reports missing inbox");
    TEST_ASSERT_EQ((int)KERN_INVALID_ID, (int)service_cap,
                   "FS runtime clears output on missing inbox");

    TEST_ASSERT_EQ((int)KERN_OK,
                   fs_runtime_bind_owned_inbox(456),
                   "FS runtime records inbox cap");
    TEST_ASSERT(fs_runtime_inbox_bound(),
                "FS runtime reports inbox bound");
    TEST_ASSERT_EQ(456, (int)fs_runtime_inbox_cap(),
                   "FS runtime returns inbox cap");
    ready_reason = NULL;
    TEST_ASSERT(fs_runtime_lookup_ready(0, &ready_reason) != KERN_OK,
                "FS runtime lookup ready still rejects bad name-server cap");
    TEST_ASSERT(ready_reason != NULL &&
                strcmp(ready_reason, "name-server") == 0,
                "FS runtime lookup ready keeps name-server blocker first");
    fs_runtime_clear_inbox();
    TEST_ASSERT(!fs_runtime_inbox_bound(),
                "FS runtime clears inbox");
    fs_runtime_clear_name_server();
    TEST_ASSERT(!fs_runtime_name_server_bound(),
                "FS runtime clears name-server");
}

static void test_fs_runtime_lookup_release_caps(void) {
    test_section("Test 23: FS runtime lookup release restores caps");

    fs_runtime_clear_name_server();
    fs_runtime_clear_inbox();

    uint16_t cap_free_before = cap_free_count();
    ep_id_t ns_ep = endpoint_create("fs_rt_ns", KERN_EP_MSG_SIZE, 4);
    TEST_ASSERT(ns_ep >= 0, "FS runtime name-server endpoint created");

    task_id_t ns_id = KERN_INVALID_ID;
    cap_id_t ns_service_cap = KERN_INVALID_ID;
    cap_id_t ns_root_cap = KERN_INVALID_ID;
    if (ns_ep >= 0) {
        ns_id = task_create_user("fs_rt_ns", nameserver_fs_runtime_task,
                                 NULL, 13, 1536);
        TEST_ASSERT(ns_id >= 0, "FS runtime name-server task created");
    }

    tcb_t *ns_tcb = task_get_tcb(ns_id);
    if (ns_tcb != NULL) {
        ns_service_cap = cap_create_for(ns_tcb, endpoint_obj_for_cap(ns_ep), CAP_OBJ_ENDPOINT,
                                        CAP_READ | CAP_WRITE);
    }
    if (ns_ep >= 0) {
        ns_root_cap = cap_create(endpoint_obj_for_cap(ns_ep), CAP_OBJ_ENDPOINT, CAP_FULL, 0);
    }
    TEST_ASSERT(ns_service_cap >= 0,
                "FS runtime name-server receives endpoint cap");
    TEST_ASSERT(ns_root_cap >= 0,
                "FS runtime root receives name-server cap");

    kern_err_t err = KERN_ERR_STATE;
    if (ns_id >= 0 && ns_service_cap >= 0 && ns_tcb != NULL &&
        ns_tcb->sp != NULL) {
        uint32_t *stacked_r0 = (uint32_t *)((uint8_t *)ns_tcb->sp + 32U);
        *stacked_r0 = (uint32_t)ns_service_cap;
        err = task_start(ns_id);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "FS runtime name-server task started");

    ep_id_t service_ep = endpoint_create("fs_rt_svc", KERN_EP_MSG_SIZE, 2);
    TEST_ASSERT(service_ep >= 0, "FS runtime service endpoint created");
    cap_id_t service_cap = KERN_INVALID_ID;
    if (service_ep >= 0) {
        service_cap = cap_create(endpoint_obj_for_cap(service_ep), CAP_OBJ_ENDPOINT,
                                 CAP_READ | CAP_WRITE | CAP_TRANSFER, 0);
    }
    TEST_ASSERT(service_cap >= 0, "FS runtime service cap created");

    if (ns_root_cap >= 0 && service_cap >= 0) {
        err = nameserver_register(ns_root_cap, "fs.ramfs", service_cap,
                                  0x46530001U, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "FS runtime service registered");

    ep_id_t inbox_ep = endpoint_create("fs_rt_inbox", KERN_EP_MSG_SIZE, 2);
    TEST_ASSERT(inbox_ep >= 0, "FS runtime inbox endpoint created");
    cap_id_t inbox_cap = KERN_INVALID_ID;
    if (inbox_ep >= 0) {
        inbox_cap = cap_create(endpoint_obj_for_cap(inbox_ep), CAP_OBJ_ENDPOINT, CAP_FULL, 0);
    }
    TEST_ASSERT(inbox_cap >= 0, "FS runtime inbox cap created");

    if (ns_root_cap >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK,
                       fs_runtime_bind_name_server(ns_root_cap),
                       "FS runtime binds name-server cap for leak test");
    }
    if (inbox_cap >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK,
                       fs_runtime_bind_owned_inbox(inbox_cap),
                       "FS runtime binds inbox cap for leak test");
    }

    void *inbox_obj = inbox_ep >= 0
        ? endpoint_obj_for_cap(inbox_ep) : NULL;
    uint16_t inbox_refs_before = inbox_obj != NULL
        ? cap_object_refcount(inbox_obj, CAP_OBJ_ENDPOINT) : 0U;
    uint16_t cap_free_before_lookups = cap_free_count();
    for (uint32_t i = 0; i < 4U; i++) {
        cap_id_t lookup_cap = KERN_INVALID_ID;
        err = fs_runtime_lookup_service("fs.ramfs", &lookup_cap, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "FS runtime lookup returns service cap");
        TEST_ASSERT(lookup_cap > 0, "FS runtime lookup cap valid");
        TEST_ASSERT(cap_free_count() < cap_free_before_lookups,
                    "FS runtime lookup consumes temporary caps");
        TEST_ASSERT_EQ((int)KERN_OK,
                       fs_runtime_release_service(lookup_cap),
                       "FS runtime release service OK");
        /* ACK completion and server-side temporary-cap release are separate
         * scheduling events.  Track this inbox object instead of the global
         * cap pool, which can change concurrently on the other core. */
        for (uint32_t wait = 0;
             wait < 100U && inbox_obj != NULL &&
             cap_object_refcount(inbox_obj, CAP_OBJ_ENDPOINT) >
                 inbox_refs_before;
             wait++) {
            task_delay(1);
        }
        TEST_ASSERT_EQ((int)inbox_refs_before,
                    (int)cap_object_refcount(inbox_obj, CAP_OBJ_ENDPOINT),
                    "FS runtime release restores temporary cap");
    }

    void *retval = NULL;
    if (ns_id >= 0) {
        err = task_join(ns_id, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "FS runtime name-server joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "FS runtime name-server retval OK");
    }

    fs_runtime_clear_inbox();
    fs_runtime_clear_name_server();
    if (ns_id >= 0 && task_get_state(ns_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(ns_id);
    }
    if (inbox_ep >= 0) {
        (void)endpoint_delete(inbox_ep);
    }
    if (service_ep >= 0) {
        (void)endpoint_delete(service_ep);
    }
    if (ns_ep >= 0) {
        (void)endpoint_delete(ns_ep);
    }
    TEST_ASSERT_EQ((int)cap_free_before, (int)cap_free_count(),
                   "FS runtime lookup release cleanup restored caps");
}

static void nameserver_register_client_task(void *arg) {
    (void)arg;
    int ns_ep_cap = sys_cap_self_slot(CAP_OBJ_ENDPOINT, 0);
    cap_id_t service_ep_cap = sys_cap_self_slot(CAP_OBJ_ENDPOINT, 1);
    const char name[] = "svc.echo";
    int err;

    if (ns_ep_cap <= 0 || service_ep_cap <= 0) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_PARAM);
    }

    err = nameserver_register(ns_ep_cap, name, service_ep_cap, 0x42U, 1000);
    sys_task_exit((void *)(intptr_t)err);
}

static void nameserver_lookup_client_task(void *arg) {
    (void)arg;
    int ns_ep_cap = sys_cap_self_slot(CAP_OBJ_ENDPOINT, 0);
    cap_id_t inbox_cap = sys_cap_self_slot(CAP_OBJ_ENDPOINT, 1);
    const char name[] = "svc.echo";
    cap_id_t service_cap = KERN_INVALID_ID;
    int err;

    if (ns_ep_cap <= 0 || inbox_cap <= 0) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_PARAM);
    }

    err = nameserver_lookup_begin(ns_ep_cap, name, inbox_cap,
                                  &service_cap, 1000);
    if (err == KERN_OK) {
        uint8_t probe[KERN_EP_MSG_SIZE];
        for (uint32_t i = 0; i < sizeof(probe); i++) {
            probe[i] = 0;
        }
        err = sys_ep_send(service_cap, probe, 0);
        if (err == KERN_ERR_TIMEOUT) {
            err = KERN_OK;
        }
    }
    if (err == KERN_OK) {
        err = nameserver_lookup_ack(inbox_cap);
    }

    sys_task_exit((void *)(intptr_t)err);
}

static void nameserver_missing_lookup_client_task(void *arg) {
    int ns_ep_cap = (int)(uintptr_t)arg;
    uint8_t msg_buf[KERN_EP_MSG_SIZE];
    ns_name_msg_t *msg = (ns_name_msg_t *)msg_buf;
    const char name[] = "svc.missing";
    int err;

    if (ns_ep_cap <= 0) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_PARAM);
    }
    for (uint32_t i = 0; i < sizeof(msg_buf); i++) {
        msg_buf[i] = 0;
    }

    ns_msg_init(&msg->hdr, NS_OP_LOOKUP, 99);
    for (uint32_t i = 0; i < sizeof(name) && i < NS_NAME_MAX; i++) {
        msg->name[i] = name[i];
    }

    err = sys_ep_send(ns_ep_cap, msg_buf, 1000);
    if (err == KERN_OK &&
        (msg->hdr.magic != NS_MAGIC ||
         msg->hdr.opcode != NS_OP_LOOKUP ||
         msg->hdr.seq != 99 ||
         msg->hdr.status != KERN_ERR_NOEXIST)) {
        err = KERN_ERR_STATE;
    }

    sys_task_exit((void *)(intptr_t)err);
}

static void nameserver_duplicate_register_client_task(void *arg) {
    (void)arg;
    int ns_ep_cap = sys_cap_self_slot(CAP_OBJ_ENDPOINT, 0);
    cap_id_t service_ep_cap = sys_cap_self_slot(CAP_OBJ_ENDPOINT, 1);
    uint8_t msg_buf[KERN_EP_MSG_SIZE];
    ns_name_msg_t *msg = (ns_name_msg_t *)msg_buf;
    ipc_cap_xfer_t xfers[IPC_CAPS_MAX];
    const char name[] = "svc.echo";
    int err;

    if (ns_ep_cap <= 0 || service_ep_cap <= 0) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_PARAM);
    }
    for (uint32_t i = 0; i < sizeof(msg_buf); i++) {
        msg_buf[i] = 0;
    }
    for (uint32_t i = 0; i < IPC_CAPS_MAX; i++) {
        xfers[i].src_cap = KERN_INVALID_ID;
        xfers[i].rights = 0;
        xfers[i].flags = IPC_CAP_COPY;
    }

    ns_msg_init(&msg->hdr, NS_OP_REGISTER, 100);
    for (uint32_t i = 0; i < sizeof(name) && i < NS_NAME_MAX; i++) {
        msg->name[i] = name[i];
    }
    msg->owner_badge = 0x42U;
    xfers[0].src_cap = service_ep_cap;
    xfers[0].rights = CAP_READ | CAP_WRITE | CAP_TRANSFER;
    xfers[0].flags = IPC_CAP_COPY;

    err = sys_ep_send_caps(ns_ep_cap, msg_buf, xfers, 1, 1000);
    if (err == KERN_OK &&
        (msg->hdr.magic != NS_MAGIC ||
         msg->hdr.opcode != NS_OP_REGISTER ||
         msg->hdr.seq != 100 ||
         msg->hdr.status != KERN_ERR_BUSY)) {
        err = KERN_ERR_STATE;
    }

    sys_task_exit((void *)(intptr_t)err);
}

static void nameserver_unregister_client_task(void *arg) {
    int ns_ep_cap = (int)(uintptr_t)arg;
    const char name[] = "svc.echo";
    int err;

    if (ns_ep_cap <= 0) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_PARAM);
    }

    err = nameserver_unregister(ns_ep_cap, name, 0x42U, 1000);

    sys_task_exit((void *)(intptr_t)err);
}

static void nameserver_bad_owner_unregister_client_task(void *arg) {
    int ns_ep_cap = (int)(uintptr_t)arg;
    const char name[] = "svc.echo";
    int err;

    if (ns_ep_cap <= 0) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_PARAM);
    }

    err = nameserver_unregister(ns_ep_cap, name, 0x99U, 1000);
    if (err == KERN_ERR_PERM) {
        err = KERN_OK;
    } else if (err == KERN_OK) {
        err = KERN_ERR_STATE;
    }

    sys_task_exit((void *)(intptr_t)err);
}

static void nameserver_recycle_client_task(void *arg) {
    (void)arg;
    int ns_ep_cap = sys_cap_self_slot(CAP_OBJ_ENDPOINT, 0);
    cap_id_t service_ep_cap = sys_cap_self_slot(CAP_OBJ_ENDPOINT, 1);
    uint8_t msg_buf[KERN_EP_MSG_SIZE];
    ns_name_msg_t *msg = (ns_name_msg_t *)msg_buf;
    ipc_cap_xfer_t xfers[IPC_CAPS_MAX];
    const char name[] = "svc.recycle";
    int err = KERN_OK;

    if (ns_ep_cap <= 0 || service_ep_cap <= 0) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_PARAM);
    }

    for (uint32_t round = 0; round < 18U && err == KERN_OK; round++) {
        for (uint32_t i = 0; i < sizeof(msg_buf); i++) {
            msg_buf[i] = 0;
        }
        for (uint32_t i = 0; i < IPC_CAPS_MAX; i++) {
            xfers[i].src_cap = KERN_INVALID_ID;
            xfers[i].rights = 0;
            xfers[i].flags = IPC_CAP_COPY;
        }

        ns_msg_init(&msg->hdr, NS_OP_REGISTER, 300U + round);
        for (uint32_t i = 0; i < sizeof(name) && i < NS_NAME_MAX; i++) {
            msg->name[i] = name[i];
        }
        msg->owner_badge = 0x33U;
        xfers[0].src_cap = service_ep_cap;
        xfers[0].rights = CAP_READ | CAP_WRITE | CAP_TRANSFER;
        xfers[0].flags = IPC_CAP_COPY;

        err = sys_ep_send_caps(ns_ep_cap, msg_buf, xfers, 1, 1000);
        if (err == KERN_OK &&
            (msg->hdr.magic != NS_MAGIC ||
             msg->hdr.opcode != NS_OP_REGISTER ||
             msg->hdr.status != KERN_OK)) {
            err = KERN_ERR_STATE;
        }

        if (err != KERN_OK) {
            break;
        }

        for (uint32_t i = 0; i < sizeof(msg_buf); i++) {
            msg_buf[i] = 0;
        }
        ns_msg_init(&msg->hdr, NS_OP_UNREG, 400U + round);
        for (uint32_t i = 0; i < sizeof(name) && i < NS_NAME_MAX; i++) {
            msg->name[i] = name[i];
        }
        msg->owner_badge = 0x33U;

        err = sys_ep_send(ns_ep_cap, msg_buf, 1000);
        if (err == KERN_OK &&
            (msg->hdr.magic != NS_MAGIC ||
             msg->hdr.opcode != NS_OP_UNREG ||
             msg->hdr.status != KERN_OK)) {
            err = KERN_ERR_STATE;
        }
    }

    sys_task_exit((void *)(intptr_t)err);
}

static void nameserver_registry_full_client_task(void *arg) {
    (void)arg;
    int ns_ep_cap = sys_cap_self_slot(CAP_OBJ_ENDPOINT, 0);
    cap_id_t service_ep_cap = sys_cap_self_slot(CAP_OBJ_ENDPOINT, 1);
    uint8_t msg_buf[KERN_EP_MSG_SIZE];
    ns_name_msg_t *msg = (ns_name_msg_t *)msg_buf;
    ipc_cap_xfer_t xfers[IPC_CAPS_MAX];
    int err = KERN_OK;

    if (ns_ep_cap <= 0 || service_ep_cap <= 0) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_PARAM);
    }

    for (uint32_t round = 0; round < NS_REGISTRY_MAX + 1U &&
                            err == KERN_OK; round++) {
        for (uint32_t i = 0; i < sizeof(msg_buf); i++) {
            msg_buf[i] = 0;
        }
        for (uint32_t i = 0; i < IPC_CAPS_MAX; i++) {
            xfers[i].src_cap = KERN_INVALID_ID;
            xfers[i].rights = 0;
            xfers[i].flags = IPC_CAP_COPY;
        }

        ns_msg_init(&msg->hdr, NS_OP_REGISTER, 500U + round);
        msg->name[0] = 's';
        msg->name[1] = 'v';
        msg->name[2] = 'c';
        msg->name[3] = '.';
        msg->name[4] = '0' + (char)(round / 10U);
        msg->name[5] = '0' + (char)(round % 10U);
        msg->name[6] = '\0';
        msg->owner_badge = 0x55U;
        xfers[0].src_cap = service_ep_cap;
        xfers[0].rights = CAP_READ | CAP_WRITE | CAP_TRANSFER;
        xfers[0].flags = IPC_CAP_COPY;

        err = sys_ep_send_caps(ns_ep_cap, msg_buf, xfers, 1, 1000);
        if (err == KERN_OK) {
            int expected = (round < NS_REGISTRY_MAX) ?
                           KERN_OK : KERN_ERR_RESOURCE;
            if (msg->hdr.magic != NS_MAGIC ||
                msg->hdr.opcode != NS_OP_REGISTER ||
                msg->hdr.status != expected) {
                err = KERN_ERR_STATE;
            }
        }
    }

    sys_task_exit((void *)(intptr_t)err);
}

static void ns_test_clear_msg(uint8_t *msg_buf) {
    for (uint32_t i = 0; i < KERN_EP_MSG_SIZE; i++) {
        msg_buf[i] = 0;
    }
}

static void ns_test_copy_name(ns_name_msg_t *msg, const char *name) {
    for (uint32_t i = 0; i < NS_NAME_MAX; i++) {
        msg->name[i] = name[i];
        if (name[i] == '\0') {
            return;
        }
    }
    msg->name[NS_NAME_MAX - 1U] = '\0';
}

static void ns_test_send_expect(ep_id_t ep,
                                uint32_t magic,
                                uint16_t opcode,
                                uint32_t seq,
                                const char *name,
                                int expected_status,
                                const char *label) {
    uint8_t msg_buf[KERN_EP_MSG_SIZE];
    ns_name_msg_t *msg = (ns_name_msg_t *)msg_buf;

    ns_test_clear_msg(msg_buf);
    ns_msg_init(&msg->hdr, opcode, seq);
    msg->hdr.magic = magic;
    if (name != NULL) {
        ns_test_copy_name(msg, name);
    }

    kern_err_t err = endpoint_send(ep, msg_buf, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, label);
    TEST_ASSERT_EQ(expected_status, (int)msg->hdr.status, label);
}

static void ns_test_send_full_name_expect(ep_id_t ep,
                                          uint16_t opcode,
                                          uint32_t seq,
                                          int expected_status,
                                          const char *label) {
    uint8_t msg_buf[KERN_EP_MSG_SIZE];
    ns_name_msg_t *msg = (ns_name_msg_t *)msg_buf;

    ns_test_clear_msg(msg_buf);
    ns_msg_init(&msg->hdr, opcode, seq);
    for (uint32_t i = 0; i < NS_NAME_MAX; i++) {
        msg->name[i] = 'x';
    }

    kern_err_t err = endpoint_send(ep, msg_buf, 1000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, label);
    TEST_ASSERT_EQ(expected_status, (int)msg->hdr.status, label);
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
    TEST_ASSERT_EQ(3, (int)info.cap_count,
                   "root bootstrap records initial caps");

    if (err == KERN_OK && info.cap_count > 2) {
        TEST_ASSERT_EQ((int)CAP_OBJ_TASK, (int)info.caps[0].obj_type,
                       "root bootstrap cap type is task");
        TEST_ASSERT_EQ((int)CAP_FULL, (int)info.caps[0].rights,
                       "root bootstrap task cap has full rights");

        void *resolved = cap_lookup_for(root, info.caps[0].cap,
                                        CAP_OBJ_TASK, CAP_MANAGE);
        TEST_ASSERT_EQ((int)tid, (int)task_id_from_obj(resolved),
                       "root task cap resolves task id object");

        TEST_ASSERT_EQ((int)CAP_OBJ_ENDPOINT, (int)info.caps[1].obj_type,
                       "root bootstrap cap type is endpoint");
        TEST_ASSERT_EQ((int)CAP_FULL, (int)info.caps[1].rights,
                       "root bootstrap endpoint cap has full rights");

        void *ep_obj = cap_lookup_for(root, info.caps[1].cap,
                                      CAP_OBJ_ENDPOINT, CAP_WRITE);
        TEST_ASSERT_NOT_NULL(ep_obj, "root task can resolve initial endpoint cap");
        if (ep_obj != NULL) {
            ep_id_t ep_id = endpoint_id_from_obj(ep_obj);  /* M2-Step3b */
            TEST_ASSERT_EQ((int)info.root_endpoint, (int)ep_id,
                           "root endpoint cap matches bootstrap record");
            TEST_ASSERT(endpoint_exists(ep_id),
                        "root bootstrap endpoint exists");
        }

        TEST_ASSERT_EQ((int)CAP_OBJ_FACTORY, (int)info.caps[2].obj_type,
                       "root bootstrap cap type is Factory");
        TEST_ASSERT_EQ((int)CAP_FULL, (int)info.caps[2].rights,
                       "root bootstrap Factory cap has full rights");
        TEST_ASSERT_EQ((int)info.factory_cap, (int)info.caps[2].cap,
                       "root bootstrap records Factory cap");

        void *factory_obj = cap_lookup_for(root, info.factory_cap,
                                           CAP_OBJ_FACTORY, CAP_WRITE);
        TEST_ASSERT_NOT_NULL(factory_obj,
                             "root task can resolve initial Factory cap");
        TEST_ASSERT_EQ((int)factory_supported_mask(),
                       (int)cap_get_badge(info.factory_cap),
                       "root Factory authorizes supported object types");
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
    TEST_ASSERT_EQ((int)service_id, (int)task_id_from_obj(resolved),
                   "root service cap resolves service task id");

    if (service_id >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(service_id),
                       "root service task deleted");
    }
    if (root_id >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(root_id),
                       "root service test root deleted");
    }
    TEST_ASSERT_EQ((int)cap_free_after_root + 3, (int)cap_free_count(),
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
        task_state_t state = task_get_state(service_id);
        /* In SMP the peer core may run the task between task_start() and this
         * observation.  READY, RUNNING and BLOCKED all prove it left CREATED;
         * requiring exactly READY is a UP-only timing assumption. */
        TEST_ASSERT(state == TASK_STATE_READY ||
                    state == TASK_STATE_RUNNING ||
                    state == TASK_STATE_BLOCKED,
                    "root service task started on SMP");
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
    TEST_ASSERT_EQ((int)cap_free_after_root + 3, (int)cap_free_count(),
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
    TEST_ASSERT_EQ((int)ep_id, (int)endpoint_id_from_obj(root_ep),
                   "root service endpoint cap resolves endpoint");

    void *service_ep = cap_lookup_for(service, service_ep_cap,
                                      CAP_OBJ_ENDPOINT, CAP_READ | CAP_WRITE);
    TEST_ASSERT_EQ((int)ep_id, (int)endpoint_id_from_obj(service_ep),
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
    service_ep = cap_lookup_for(service, service_ep_cap,
                                CAP_OBJ_ENDPOINT, CAP_READ);
    TEST_ASSERT_NULL(service_ep,
                     "service endpoint cap revoked on endpoint delete");
    root_ep = cap_lookup_for(root, root_ep_cap,
                             CAP_OBJ_ENDPOINT, CAP_MANAGE);
    TEST_ASSERT_NULL(root_ep,
                     "root endpoint cap revoked on endpoint delete");

    if (root_id >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(root_id),
                       "service endpoint test root deleted");
    }
    TEST_ASSERT_EQ((int)cap_free_after_root + 3, (int)cap_free_count(),
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
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "service IPC endpoint_send OK");
    TEST_ASSERT_EQ((int)0x7101U, (int)msg[0],
                   "service IPC reply opcode OK");
    TEST_ASSERT_EQ(42, (int)msg[1],
                   "service IPC reply payload OK");

    void *retval = NULL;
    if (service_id >= 0) {
        err = task_join(service_id, &retval, 1000);
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
    TEST_ASSERT_EQ((int)cap_free_after_root + 3, (int)cap_free_count(),
                   "service IPC cleanup restored all caps");
}

static void test_nameserver_protocol_layout(void) {
    test_section("Test 9: name server protocol ABI");

    ns_msg_hdr_t hdr;
    ns_msg_init(&hdr, NS_OP_PING, 7);

    TEST_ASSERT_EQ((int)NS_MAGIC, (int)hdr.magic,
                   "name server magic initialized");
    TEST_ASSERT_EQ((int)NS_OP_PING, (int)hdr.opcode,
                   "name server opcode initialized");
    TEST_ASSERT_EQ((int)NS_FLAG_NONE, (int)hdr.flags,
                   "name server flags initialized");
    TEST_ASSERT_EQ(7, (int)hdr.seq,
                   "name server sequence initialized");
    TEST_ASSERT_EQ(0, (int)hdr.status,
                   "name server status initialized");

    TEST_ASSERT(ns_opcode_valid(NS_OP_REGISTER),
                "name server register opcode valid");
    TEST_ASSERT(ns_opcode_valid(NS_OP_LOOKUP),
                "name server lookup opcode valid");
    TEST_ASSERT(ns_opcode_valid(NS_OP_UNREG),
                "name server unregister opcode valid");
    TEST_ASSERT(ns_opcode_valid(NS_OP_PING),
                "name server ping opcode valid");
    TEST_ASSERT(!ns_opcode_valid(0),
                "name server zero opcode rejected");
    TEST_ASSERT(!ns_opcode_valid(NS_OP_PING + 1U),
                "name server unknown opcode rejected");

    TEST_ASSERT_EQ(24, (int)NS_NAME_MAX,
                   "name server name length fixed");
    TEST_ASSERT_EQ(16, (int)NS_REGISTRY_MAX,
                   "name server registry size fixed");
    TEST_ASSERT(KERN_TASK_CAP_SLOTS > NS_REGISTRY_MAX,
                "task CSpace can hold full name server registry");
    TEST_ASSERT(sizeof(ns_msg_hdr_t) <= KERN_EP_MSG_SIZE,
                "name server header fits endpoint message");
    TEST_ASSERT(sizeof(ns_name_msg_t) <= KERN_EP_MSG_SIZE,
                "name server named message fits endpoint message");
    TEST_ASSERT(sizeof(ns_name_msg_t) ==
                sizeof(ns_msg_hdr_t) + NS_NAME_MAX + sizeof(uint32_t),
                "name server owner badge is part of named message");
}

static void test_nameserver_helper_validation(void) {
    test_section("Test 10: name server helper validation");

    char full_name[NS_NAME_MAX];
    for (uint32_t i = 0; i < NS_NAME_MAX; i++) {
        full_name[i] = 'z';
    }

    cap_id_t out_cap = 123;
    int err = nameserver_ping(0, 1000);
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, err,
                   "helper ping rejects invalid ns cap");

    err = nameserver_register(1, NULL, 1, 0x11U, 1000);
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, err,
                   "helper register rejects NULL name");
    err = nameserver_register(1, "", 1, 0x11U, 1000);
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, err,
                   "helper register rejects empty name");
    err = nameserver_register(1, full_name, 1, 0x11U, 1000);
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, err,
                   "helper register rejects unterminated name");
    err = nameserver_register(0, "svc.helper", 1, 0x11U, 1000);
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, err,
                   "helper register rejects invalid ns cap");
    err = nameserver_register(1, "svc.helper", 0, 0x11U, 1000);
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, err,
                   "helper register rejects invalid service cap");

    err = nameserver_unregister(1, NULL, 0x11U, 1000);
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, err,
                   "helper unregister rejects NULL name");
    err = nameserver_unregister(1, full_name, 0x11U, 1000);
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, err,
                   "helper unregister rejects unterminated name");
    err = nameserver_unregister(0, "svc.helper", 0x11U, 1000);
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, err,
                   "helper unregister rejects invalid ns cap");

    err = nameserver_lookup_begin(1, "", 1, &out_cap, 1000);
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, err,
                   "helper lookup rejects empty name");
    TEST_ASSERT_EQ((int)KERN_INVALID_ID, (int)out_cap,
                   "helper lookup clears out cap before name validation");
    out_cap = 123;
    err = nameserver_lookup_begin(1, full_name, 1, &out_cap, 1000);
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, err,
                   "helper lookup rejects unterminated name");
    TEST_ASSERT_EQ((int)KERN_INVALID_ID, (int)out_cap,
                   "helper lookup clears out cap on long name");
    err = nameserver_lookup_begin(1, "svc.helper", 1, NULL, 1000);
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, err,
                   "helper lookup rejects NULL out cap");
    err = nameserver_lookup_begin(0, "svc.helper", 1, &out_cap, 1000);
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, err,
                   "helper lookup rejects invalid ns cap");
    err = nameserver_lookup_begin(1, "svc.helper", 0, &out_cap, 1000);
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, err,
                   "helper lookup rejects invalid inbox cap");

    err = nameserver_lookup_ack(0);
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, err,
                   "helper lookup ack rejects invalid inbox cap");
}

static void test_nameserver_ping_service(void) {
    test_section("Test 11: name server ping service");

    root_bootstrap_init();

    task_id_t root_id = KERN_INVALID_ID;
    kern_err_t err = root_bootstrap_create("root_ns", root_dummy_task,
                                           NULL, 12, 512, &root_id);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "name server test creates root");
    if (err != KERN_OK || root_id < 0) {
        return;
    }

    uint16_t cap_free_after_root = cap_free_count();
    task_id_t ns_id = KERN_INVALID_ID;
    cap_id_t ns_task_cap = KERN_INVALID_ID;
    err = root_bootstrap_create_service("nameserver", nameserver_ping_task,
                                        NULL, 13, 1536,
                                        &ns_id, &ns_task_cap);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "name server task created by root");
    TEST_ASSERT(ns_id >= 0, "name server task id valid");
    TEST_ASSERT(ns_task_cap >= 0, "root receives name server task cap");

    ep_id_t ns_ep = KERN_INVALID_ID;
    cap_id_t root_ep_cap = KERN_INVALID_ID;
    cap_id_t ns_ep_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = root_bootstrap_create_service_endpoint(ns_task_cap,
                                                     "nameserver_ep",
                                                     KERN_EP_MSG_SIZE,
                                                     2,
                                                     &ns_ep,
                                                     &root_ep_cap,
                                                     &ns_ep_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "name server endpoint created by root");
    TEST_ASSERT(ns_ep >= 0, "name server endpoint id valid");
    TEST_ASSERT(root_ep_cap >= 0, "root receives name server endpoint cap");
    TEST_ASSERT(ns_ep_cap >= 0, "name server receives endpoint cap");

    if (err == KERN_OK) {
        err = root_bootstrap_start_service(ns_task_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "name server task started by root");

    uint8_t msg_buf[KERN_EP_MSG_SIZE];
    for (uint32_t i = 0; i < sizeof(msg_buf); i++) {
        msg_buf[i] = 0;
    }
    ns_msg_hdr_t *hdr = (ns_msg_hdr_t *)msg_buf;
    ns_msg_init(hdr, NS_OP_PING, 42);

    if (err == KERN_OK) {
        err = endpoint_send(ns_ep, msg_buf, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "name server ping endpoint_send OK");
    TEST_ASSERT_EQ((int)NS_MAGIC, (int)hdr->magic,
                   "name server ping reply keeps magic");
    TEST_ASSERT_EQ((int)NS_OP_PING, (int)hdr->opcode,
                   "name server ping reply keeps opcode");
    TEST_ASSERT_EQ(42, (int)hdr->seq,
                   "name server ping reply keeps sequence");
    TEST_ASSERT_EQ((int)KERN_OK, (int)hdr->status,
                   "name server ping status OK");

    void *retval = NULL;
    if (ns_id >= 0) {
        err = task_join(ns_id, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "name server ping task exited OK");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "name server ping service retval OK");
    }

    if (ns_id >= 0 && task_get_state(ns_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(ns_id);
    }
    if (root_id >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(root_id),
                       "name server ping root deleted");
    }
    TEST_ASSERT_EQ((int)cap_free_after_root + 3, (int)cap_free_count(),
                   "name server ping cleanup restored caps");
}

static void test_nameserver_register_service(void) {
    test_section("Test 11: name server register service");

    root_bootstrap_init();

    task_id_t root_id = KERN_INVALID_ID;
    kern_err_t err = root_bootstrap_create("root_ns_reg", root_dummy_task,
                                           NULL, 12, 512, &root_id);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "name server register creates root");
    if (err != KERN_OK || root_id < 0) {
        return;
    }

    uint16_t cap_free_after_root = cap_free_count();
    task_id_t ns_id = KERN_INVALID_ID;
    cap_id_t ns_task_cap = KERN_INVALID_ID;
    err = root_bootstrap_create_service("ns_register",
                                        nameserver_register_once_task,
                                        NULL, 13, 1536,
                                        &ns_id, &ns_task_cap);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "name server register task created");

    ep_id_t ns_ep = KERN_INVALID_ID;
    cap_id_t root_ep_cap = KERN_INVALID_ID;
    cap_id_t ns_ep_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = root_bootstrap_create_service_endpoint(ns_task_cap,
                                                     "ns_register_ep",
                                                     KERN_EP_MSG_SIZE,
                                                     2,
                                                     &ns_ep,
                                                     &root_ep_cap,
                                                     &ns_ep_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "name server register endpoint created");
    TEST_ASSERT(ns_ep >= 0, "name server register endpoint id valid");
    TEST_ASSERT(root_ep_cap >= 0, "root receives register endpoint cap");
    TEST_ASSERT(ns_ep_cap >= 0, "name server receives register endpoint cap");

    if (err == KERN_OK) {
        err = root_bootstrap_start_service(ns_task_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "name server register task started");

    ep_id_t service_ep = endpoint_create("svc_echo", KERN_EP_MSG_SIZE, 2);
    TEST_ASSERT(service_ep >= 0, "service endpoint for registration created");

    task_id_t client_id = KERN_INVALID_ID;
    cap_id_t client_ns_cap = KERN_INVALID_ID;
    cap_id_t client_service_cap = KERN_INVALID_ID;
    if (service_ep >= 0) {
        client_id = task_create_user("ns_reg_client",
                                     nameserver_register_client_task,
                                     NULL, 14, 768);
        TEST_ASSERT(client_id >= 0, "name server register client created");
    }

    tcb_t *client = task_get_tcb(client_id);
    if (client != NULL) {
        client_ns_cap = cap_create_for(client, endpoint_obj_for_cap(ns_ep), CAP_OBJ_ENDPOINT,
                                       CAP_READ | CAP_WRITE);
        client_service_cap = cap_create_for(client, endpoint_obj_for_cap(service_ep), CAP_OBJ_ENDPOINT,
                                            CAP_READ | CAP_WRITE | CAP_TRANSFER);
    }
    TEST_ASSERT(client_ns_cap >= 0,
                "client receives name server endpoint cap");
    TEST_ASSERT(client_service_cap >= 0,
                "client receives service endpoint cap to register");

    if (client_id >= 0 && client_ns_cap >= 0 && client_service_cap >= 0) {
        err = task_start(client_id);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "name server register client started");
    }

    void *client_retval = NULL;
    if (client_id >= 0) {
        err = task_join(client_id, &client_retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "name server register client joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)client_retval,
                       "name server register client retval OK");
    }

    void *ns_retval = NULL;
    if (ns_id >= 0) {
        err = task_join(ns_id, &ns_retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "name server register service joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)ns_retval,
                       "name server register service retval OK");
    }

    if (client_id >= 0 && task_get_state(client_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(client_id);
    }
    if (ns_id >= 0 && task_get_state(ns_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(ns_id);
    }
    if (service_ep >= 0) {
        (void)endpoint_delete(service_ep);
    }
    if (root_id >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(root_id),
                       "name server register root deleted");
    }
    TEST_ASSERT_EQ((int)cap_free_after_root + 3, (int)cap_free_count(),
                   "name server register cleanup restored caps");
}

static void test_nameserver_lookup_service(void) {
    test_section("Test 12: name server lookup service");

    root_bootstrap_init();

    task_id_t root_id = KERN_INVALID_ID;
    kern_err_t err = root_bootstrap_create("root_ns_lookup", root_dummy_task,
                                           NULL, 12, 512, &root_id);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "name server lookup creates root");
    if (err != KERN_OK || root_id < 0) {
        return;
    }

    uint16_t cap_free_after_root = cap_free_count();
    task_id_t ns_id = KERN_INVALID_ID;
    cap_id_t ns_task_cap = KERN_INVALID_ID;
    err = root_bootstrap_create_service("ns_lookup",
                                        nameserver_register_lookup_task,
                                        NULL, 13, 1536,
                                        &ns_id, &ns_task_cap);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "name server lookup task created");

    ep_id_t ns_ep = KERN_INVALID_ID;
    cap_id_t root_ep_cap = KERN_INVALID_ID;
    cap_id_t ns_ep_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = root_bootstrap_create_service_endpoint(ns_task_cap,
                                                     "ns_lookup_ep",
                                                     KERN_EP_MSG_SIZE,
                                                     2,
                                                     &ns_ep,
                                                     &root_ep_cap,
                                                     &ns_ep_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "name server lookup endpoint created");
    TEST_ASSERT(ns_ep >= 0, "name server lookup endpoint id valid");

    if (err == KERN_OK) {
        err = root_bootstrap_start_service(ns_task_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "name server lookup task started");

    ep_id_t service_ep = endpoint_create("svc_echo_lookup",
                                         KERN_EP_MSG_SIZE, 2);
    ep_id_t inbox_ep = endpoint_create("ns_lookup_inbox",
                                       KERN_EP_MSG_SIZE, 2);
    TEST_ASSERT(service_ep >= 0, "lookup service endpoint created");
    TEST_ASSERT(inbox_ep >= 0, "lookup client inbox endpoint created");

    task_id_t reg_client = KERN_INVALID_ID;
    task_id_t lookup_client = KERN_INVALID_ID;
    cap_id_t reg_ns_cap = KERN_INVALID_ID;
    cap_id_t reg_service_cap = KERN_INVALID_ID;
    cap_id_t lookup_ns_cap = KERN_INVALID_ID;
    cap_id_t lookup_inbox_cap = KERN_INVALID_ID;

    if (service_ep >= 0 && inbox_ep >= 0) {
        reg_client = task_create_user("ns_reg2",
                                      nameserver_register_client_task,
                                      NULL, 14, 768);
        lookup_client = task_create_user("ns_lookup_client",
                                         nameserver_lookup_client_task,
                                         NULL, 14, 896);
        TEST_ASSERT(reg_client >= 0, "lookup register client created");
        TEST_ASSERT(lookup_client >= 0, "lookup client created");
    }

    tcb_t *reg_tcb = task_get_tcb(reg_client);
    if (reg_tcb != NULL) {
        reg_ns_cap = cap_create_for(reg_tcb, endpoint_obj_for_cap(ns_ep), CAP_OBJ_ENDPOINT,
                                    CAP_READ | CAP_WRITE);
        reg_service_cap = cap_create_for(reg_tcb, endpoint_obj_for_cap(service_ep), CAP_OBJ_ENDPOINT,
                                         CAP_READ | CAP_WRITE | CAP_TRANSFER);
    }
    tcb_t *lookup_tcb = task_get_tcb(lookup_client);
    if (lookup_tcb != NULL) {
        lookup_ns_cap = cap_create_for(lookup_tcb, endpoint_obj_for_cap(ns_ep), CAP_OBJ_ENDPOINT,
                                       CAP_READ | CAP_WRITE);
        lookup_inbox_cap = cap_create_for(lookup_tcb, endpoint_obj_for_cap(inbox_ep), CAP_OBJ_ENDPOINT,
                                          CAP_READ | CAP_WRITE | CAP_TRANSFER);
    }

    TEST_ASSERT(reg_ns_cap >= 0, "register client receives ns cap");
    TEST_ASSERT(reg_service_cap >= 0, "register client receives service cap");
    TEST_ASSERT(lookup_ns_cap >= 0, "lookup client receives ns cap");
    TEST_ASSERT(lookup_inbox_cap >= 0, "lookup client receives inbox cap");

    if (reg_client >= 0 && reg_ns_cap >= 0 && reg_service_cap >= 0) {
        err = task_start(reg_client);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "lookup register client started");
    }

    void *retval = NULL;
    if (reg_client >= 0) {
        err = task_join(reg_client, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "lookup register client joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "lookup register client retval OK");
    }

    if (lookup_client >= 0 && lookup_ns_cap >= 0 && lookup_inbox_cap >= 0) {
        err = task_start(lookup_client);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "lookup client started");
    }

    retval = NULL;
    if (lookup_client >= 0) {
        err = task_join(lookup_client, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "lookup client joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "lookup client retval OK");
    }

    retval = NULL;
    if (ns_id >= 0) {
        err = task_join(ns_id, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "lookup name server joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "lookup name server retval OK");
    }

    if (reg_client >= 0 &&
        task_get_state(reg_client) != TASK_STATE_TERMINATED) {
        (void)task_delete(reg_client);
    }
    if (lookup_client >= 0 &&
        task_get_state(lookup_client) != TASK_STATE_TERMINATED) {
        (void)task_delete(lookup_client);
    }
    if (ns_id >= 0 && task_get_state(ns_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(ns_id);
    }
    if (inbox_ep >= 0) {
        (void)endpoint_delete(inbox_ep);
    }
    if (service_ep >= 0) {
        (void)endpoint_delete(service_ep);
    }
    if (root_id >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(root_id),
                       "lookup root deleted");
    }
    TEST_ASSERT_EQ((int)cap_free_after_root + 3, (int)cap_free_count(),
                   "lookup cleanup restored caps");
}

static void test_nameserver_negative_paths(void) {
    test_section("Test 13: name server negative paths");

    root_bootstrap_init();

    task_id_t root_id = KERN_INVALID_ID;
    kern_err_t err = root_bootstrap_create("root_ns_neg", root_dummy_task,
                                           NULL, 12, 512, &root_id);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "name server negative creates root");
    if (err != KERN_OK || root_id < 0) {
        return;
    }

    uint16_t cap_free_after_root = cap_free_count();
    task_id_t ns_id = KERN_INVALID_ID;
    cap_id_t ns_task_cap = KERN_INVALID_ID;
    err = root_bootstrap_create_service("ns_negative",
                                        nameserver_negative_task,
                                        NULL, 13, 1536,
                                        &ns_id, &ns_task_cap);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "negative name server task created");

    ep_id_t ns_ep = KERN_INVALID_ID;
    cap_id_t root_ep_cap = KERN_INVALID_ID;
    cap_id_t ns_ep_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = root_bootstrap_create_service_endpoint(ns_task_cap,
                                                     "ns_negative_ep",
                                                     KERN_EP_MSG_SIZE,
                                                     3,
                                                     &ns_ep,
                                                     &root_ep_cap,
                                                     &ns_ep_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "negative name server endpoint created");

    if (err == KERN_OK) {
        err = root_bootstrap_start_service(ns_task_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "negative name server task started");

    ep_id_t service_ep = endpoint_create("svc_echo_neg",
                                         KERN_EP_MSG_SIZE, 2);
    TEST_ASSERT(service_ep >= 0, "negative service endpoint created");

    task_id_t missing_client = KERN_INVALID_ID;
    task_id_t reg_client = KERN_INVALID_ID;
    task_id_t dup_client = KERN_INVALID_ID;
    cap_id_t missing_ns_cap = KERN_INVALID_ID;
    cap_id_t reg_ns_cap = KERN_INVALID_ID;
    cap_id_t reg_service_cap = KERN_INVALID_ID;
    cap_id_t dup_ns_cap = KERN_INVALID_ID;
    cap_id_t dup_service_cap = KERN_INVALID_ID;

    if (service_ep >= 0) {
        missing_client = task_create_user("ns_missing",
                                          nameserver_missing_lookup_client_task,
                                          NULL, 14, 768);
        reg_client = task_create_user("ns_reg_neg",
                                      nameserver_register_client_task,
                                      NULL, 14, 768);
        dup_client = task_create_user("ns_dup",
                                      nameserver_duplicate_register_client_task,
                                      NULL, 14, 768);
        TEST_ASSERT(missing_client >= 0, "missing lookup client created");
        TEST_ASSERT(reg_client >= 0, "negative register client created");
        TEST_ASSERT(dup_client >= 0, "duplicate register client created");
    }

    tcb_t *missing_tcb = task_get_tcb(missing_client);
    if (missing_tcb != NULL) {
        missing_ns_cap = cap_create_for(missing_tcb, endpoint_obj_for_cap(ns_ep), CAP_OBJ_ENDPOINT,
                                        CAP_READ | CAP_WRITE);
    }
    tcb_t *reg_tcb = task_get_tcb(reg_client);
    if (reg_tcb != NULL) {
        reg_ns_cap = cap_create_for(reg_tcb, endpoint_obj_for_cap(ns_ep), CAP_OBJ_ENDPOINT,
                                    CAP_READ | CAP_WRITE);
        reg_service_cap = cap_create_for(reg_tcb, endpoint_obj_for_cap(service_ep), CAP_OBJ_ENDPOINT,
                                         CAP_READ | CAP_WRITE | CAP_TRANSFER);
    }
    tcb_t *dup_tcb = task_get_tcb(dup_client);
    if (dup_tcb != NULL) {
        dup_ns_cap = cap_create_for(dup_tcb, endpoint_obj_for_cap(ns_ep), CAP_OBJ_ENDPOINT,
                                    CAP_READ | CAP_WRITE);
        dup_service_cap = cap_create_for(dup_tcb, endpoint_obj_for_cap(service_ep), CAP_OBJ_ENDPOINT,
                                         CAP_READ | CAP_WRITE | CAP_TRANSFER);
    }

    TEST_ASSERT(missing_ns_cap >= 0, "missing lookup client receives ns cap");
    TEST_ASSERT(reg_ns_cap >= 0, "negative register client receives ns cap");
    TEST_ASSERT(reg_service_cap >= 0,
                "negative register client receives service cap");
    TEST_ASSERT(dup_ns_cap >= 0, "duplicate client receives ns cap");
    TEST_ASSERT(dup_service_cap >= 0, "duplicate client receives service cap");

    if (missing_client >= 0 && missing_ns_cap >= 0) {
        if (missing_tcb != NULL && missing_tcb->sp != NULL) {
            uint32_t *stacked_r0 =
                (uint32_t *)((uint8_t *)missing_tcb->sp + 32U);
            *stacked_r0 = (uint32_t)missing_ns_cap;
        }
        err = task_start(missing_client);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "missing lookup client started");
    }

    void *retval = NULL;
    if (missing_client >= 0) {
        err = task_join(missing_client, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "missing lookup client joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "missing lookup returned NOEXIST");
    }

    if (reg_client >= 0 && reg_ns_cap >= 0 && reg_service_cap >= 0) {
        err = task_start(reg_client);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "negative register client started");
    }

    retval = NULL;
    if (reg_client >= 0) {
        err = task_join(reg_client, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "negative register client joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "negative register client retval OK");
    }

    if (dup_client >= 0 && dup_ns_cap >= 0 && dup_service_cap >= 0) {
        err = task_start(dup_client);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "duplicate register client started");
    }

    retval = NULL;
    if (dup_client >= 0) {
        err = task_join(dup_client, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "duplicate register client joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "duplicate register returned BUSY");
    }

    retval = NULL;
    if (ns_id >= 0) {
        err = task_join(ns_id, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "negative name server joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "negative name server retval OK");
    }

    if (missing_client >= 0 &&
        task_get_state(missing_client) != TASK_STATE_TERMINATED) {
        (void)task_delete(missing_client);
    }
    if (reg_client >= 0 &&
        task_get_state(reg_client) != TASK_STATE_TERMINATED) {
        (void)task_delete(reg_client);
    }
    if (dup_client >= 0 &&
        task_get_state(dup_client) != TASK_STATE_TERMINATED) {
        (void)task_delete(dup_client);
    }
    if (ns_id >= 0 && task_get_state(ns_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(ns_id);
    }
    if (service_ep >= 0) {
        (void)endpoint_delete(service_ep);
    }
    if (root_id >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(root_id),
                       "negative root deleted");
    }
    TEST_ASSERT_EQ((int)cap_free_after_root + 3, (int)cap_free_count(),
                   "negative cleanup restored caps");
}

static void test_nameserver_unregister_service(void) {
    test_section("Test 14: name server unregister service");

    root_bootstrap_init();

    task_id_t root_id = KERN_INVALID_ID;
    kern_err_t err = root_bootstrap_create("root_ns_unreg", root_dummy_task,
                                           NULL, 12, 512, &root_id);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "unregister creates root");
    if (err != KERN_OK || root_id < 0) {
        return;
    }

    uint16_t cap_free_after_root = cap_free_count();
    task_id_t ns_id = KERN_INVALID_ID;
    cap_id_t ns_task_cap = KERN_INVALID_ID;
    err = root_bootstrap_create_service("ns_unregister",
                                        nameserver_unregister_task,
                                        NULL, 13, 1536,
                                        &ns_id, &ns_task_cap);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "unregister name server task created");

    ep_id_t ns_ep = KERN_INVALID_ID;
    cap_id_t root_ep_cap = KERN_INVALID_ID;
    cap_id_t ns_ep_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = root_bootstrap_create_service_endpoint(ns_task_cap,
                                                     "ns_unreg_ep",
                                                     KERN_EP_MSG_SIZE,
                                                     3,
                                                     &ns_ep,
                                                     &root_ep_cap,
                                                     &ns_ep_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "unregister name server endpoint created");

    if (err == KERN_OK) {
        err = root_bootstrap_start_service(ns_task_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "unregister name server task started");

    ep_id_t service_ep = endpoint_create("svc_echo_unreg",
                                         KERN_EP_MSG_SIZE, 2);
    TEST_ASSERT(service_ep >= 0, "unregister service endpoint created");

    task_id_t reg_client = KERN_INVALID_ID;
    task_id_t unreg_client = KERN_INVALID_ID;
    task_id_t missing_client = KERN_INVALID_ID;
    cap_id_t reg_ns_cap = KERN_INVALID_ID;
    cap_id_t reg_service_cap = KERN_INVALID_ID;
    cap_id_t unreg_ns_cap = KERN_INVALID_ID;
    cap_id_t missing_ns_cap = KERN_INVALID_ID;

    if (service_ep >= 0) {
        reg_client = task_create_user("ns_reg_unreg",
                                      nameserver_register_client_task,
                                      NULL, 14, 768);
        unreg_client = task_create_user("ns_unreg_client",
                                        nameserver_unregister_client_task,
                                        NULL, 14, 768);
        missing_client = task_create_user("ns_lookup_after_unreg",
                                          nameserver_missing_lookup_client_task,
                                          NULL, 14, 768);
        TEST_ASSERT(reg_client >= 0, "unregister register client created");
        TEST_ASSERT(unreg_client >= 0, "unregister client created");
        TEST_ASSERT(missing_client >= 0, "post-unregister lookup client created");
    }

    tcb_t *reg_tcb = task_get_tcb(reg_client);
    if (reg_tcb != NULL) {
        reg_ns_cap = cap_create_for(reg_tcb, endpoint_obj_for_cap(ns_ep), CAP_OBJ_ENDPOINT,
                                    CAP_READ | CAP_WRITE);
        reg_service_cap = cap_create_for(reg_tcb, endpoint_obj_for_cap(service_ep), CAP_OBJ_ENDPOINT,
                                         CAP_READ | CAP_WRITE | CAP_TRANSFER);
    }
    tcb_t *unreg_tcb = task_get_tcb(unreg_client);
    if (unreg_tcb != NULL) {
        unreg_ns_cap = cap_create_for(unreg_tcb, endpoint_obj_for_cap(ns_ep), CAP_OBJ_ENDPOINT,
                                      CAP_READ | CAP_WRITE);
    }
    tcb_t *missing_tcb = task_get_tcb(missing_client);
    if (missing_tcb != NULL) {
        missing_ns_cap = cap_create_for(missing_tcb, endpoint_obj_for_cap(ns_ep), CAP_OBJ_ENDPOINT,
                                        CAP_READ | CAP_WRITE);
    }

    TEST_ASSERT(reg_ns_cap >= 0, "unregister register client receives ns cap");
    TEST_ASSERT(reg_service_cap >= 0,
                "unregister register client receives service cap");
    TEST_ASSERT(unreg_ns_cap >= 0, "unregister client receives ns cap");
    TEST_ASSERT(missing_ns_cap >= 0, "post-unregister lookup receives ns cap");

    if (reg_client >= 0 && reg_ns_cap >= 0 && reg_service_cap >= 0) {
        err = task_start(reg_client);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "unregister register client started");
    }

    void *retval = NULL;
    if (reg_client >= 0) {
        err = task_join(reg_client, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "unregister register client joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "unregister register client retval OK");
    }

    if (unreg_client >= 0 && unreg_ns_cap >= 0) {
        if (unreg_tcb != NULL && unreg_tcb->sp != NULL) {
            uint32_t *stacked_r0 =
                (uint32_t *)((uint8_t *)unreg_tcb->sp + 32U);
            *stacked_r0 = (uint32_t)unreg_ns_cap;
        }
        err = task_start(unreg_client);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "unregister client started");
    }

    retval = NULL;
    if (unreg_client >= 0) {
        err = task_join(unreg_client, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "unregister client joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "unregister client retval OK");
    }

    if (missing_client >= 0 && missing_ns_cap >= 0) {
        if (missing_tcb != NULL && missing_tcb->sp != NULL) {
            uint32_t *stacked_r0 =
                (uint32_t *)((uint8_t *)missing_tcb->sp + 32U);
            *stacked_r0 = (uint32_t)missing_ns_cap;
        }
        err = task_start(missing_client);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "post-unregister lookup client started");
    }

    retval = NULL;
    if (missing_client >= 0) {
        err = task_join(missing_client, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "post-unregister lookup client joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "post-unregister lookup returned NOEXIST");
    }

    retval = NULL;
    if (ns_id >= 0) {
        err = task_join(ns_id, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "unregister name server joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "unregister name server retval OK");
    }

    if (reg_client >= 0 &&
        task_get_state(reg_client) != TASK_STATE_TERMINATED) {
        (void)task_delete(reg_client);
    }
    if (unreg_client >= 0 &&
        task_get_state(unreg_client) != TASK_STATE_TERMINATED) {
        (void)task_delete(unreg_client);
    }
    if (missing_client >= 0 &&
        task_get_state(missing_client) != TASK_STATE_TERMINATED) {
        (void)task_delete(missing_client);
    }
    if (ns_id >= 0 && task_get_state(ns_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(ns_id);
    }
    if (service_ep >= 0) {
        (void)endpoint_delete(service_ep);
    }
    if (root_id >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(root_id),
                       "unregister root deleted");
    }
    TEST_ASSERT_EQ((int)cap_free_after_root + 3, (int)cap_free_count(),
                   "unregister cleanup restored caps");
}

static void test_nameserver_protocol_error_service(void) {
    test_section("Test 15: name server protocol error service");

    root_bootstrap_init();

    task_id_t root_id = KERN_INVALID_ID;
    kern_err_t err = root_bootstrap_create("root_ns_errors", root_dummy_task,
                                           NULL, 12, 512, &root_id);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "protocol errors create root");
    if (err != KERN_OK || root_id < 0) {
        return;
    }

    uint16_t cap_free_after_root = cap_free_count();
    task_id_t ns_id = KERN_INVALID_ID;
    cap_id_t ns_task_cap = KERN_INVALID_ID;
    err = root_bootstrap_create_service("ns_errors",
                                        nameserver_protocol_errors_task,
                                        NULL, 13, 1536,
                                        &ns_id, &ns_task_cap);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "protocol errors name server task created");

    ep_id_t ns_ep = KERN_INVALID_ID;
    cap_id_t root_ep_cap = KERN_INVALID_ID;
    cap_id_t ns_ep_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = root_bootstrap_create_service_endpoint(ns_task_cap,
                                                     "ns_errors_ep",
                                                     KERN_EP_MSG_SIZE,
                                                     4,
                                                     &ns_ep,
                                                     &root_ep_cap,
                                                     &ns_ep_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "protocol errors endpoint created");

    if (err == KERN_OK) {
        err = root_bootstrap_start_service(ns_task_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "protocol errors name server task started");

    if (err == KERN_OK && ns_ep >= 0) {
        ns_test_send_expect(ns_ep, 0x12345678U, NS_OP_PING, 200,
                            NULL, KERN_ERR_PARAM,
                            "bad magic rejected");
        ns_test_send_expect(ns_ep, NS_MAGIC, NS_OP_REGISTER, 201,
                            NULL, KERN_ERR_PARAM,
                            "empty register name rejected");
        ns_test_send_full_name_expect(ns_ep, NS_OP_REGISTER, 202,
                                      KERN_ERR_PARAM,
                                      "unterminated register name rejected");
        ns_test_send_expect(ns_ep, NS_MAGIC, NS_OP_REGISTER, 203,
                            "svc.badreg", KERN_ERR_CAP,
                            "register without cap rejected");
        ns_test_send_expect(ns_ep, NS_MAGIC, NS_OP_UNREG, 204,
                            "svc.missing", KERN_ERR_NOEXIST,
                            "unregister missing service rejected");
        ns_test_send_expect(ns_ep, NS_MAGIC, NS_OP_PING, 205,
                            NULL, KERN_OK,
                            "ping after protocol errors OK");
    }

    void *retval = NULL;
    if (ns_id >= 0) {
        err = task_join(ns_id, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "protocol errors name server joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "protocol errors name server retval OK");
    }

    if (ns_id >= 0 && task_get_state(ns_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(ns_id);
    }
    if (root_id >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(root_id),
                       "protocol errors root deleted");
    }
    TEST_ASSERT_EQ((int)cap_free_after_root + 3, (int)cap_free_count(),
                   "protocol errors cleanup restored caps");
}

static void test_nameserver_cap_recycle_service(void) {
    test_section("Test 16: name server cap recycle service");

    root_bootstrap_init();

    task_id_t root_id = KERN_INVALID_ID;
    kern_err_t err = root_bootstrap_create("root_ns_recycle", root_dummy_task,
                                           NULL, 12, 512, &root_id);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "cap recycle creates root");
    if (err != KERN_OK || root_id < 0) {
        return;
    }

    uint16_t cap_free_after_root = cap_free_count();
    task_id_t ns_id = KERN_INVALID_ID;
    cap_id_t ns_task_cap = KERN_INVALID_ID;
    err = root_bootstrap_create_service("ns_recycle",
                                        nameserver_recycle_task,
                                        NULL, 13, 1536,
                                        &ns_id, &ns_task_cap);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "cap recycle name server task created");

    ep_id_t ns_ep = KERN_INVALID_ID;
    cap_id_t root_ep_cap = KERN_INVALID_ID;
    cap_id_t ns_ep_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = root_bootstrap_create_service_endpoint(ns_task_cap,
                                                     "ns_recycle_ep",
                                                     KERN_EP_MSG_SIZE,
                                                     4,
                                                     &ns_ep,
                                                     &root_ep_cap,
                                                     &ns_ep_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "cap recycle endpoint created");

    if (err == KERN_OK) {
        err = root_bootstrap_start_service(ns_task_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "cap recycle name server task started");

    ep_id_t service_ep = endpoint_create("svc_recycle", KERN_EP_MSG_SIZE, 2);
    TEST_ASSERT(service_ep >= 0, "cap recycle service endpoint created");

    task_id_t client_id = KERN_INVALID_ID;
    cap_id_t client_ns_cap = KERN_INVALID_ID;
    cap_id_t client_service_cap = KERN_INVALID_ID;
    if (service_ep >= 0) {
        client_id = task_create_user("ns_recycle_client",
                                     nameserver_recycle_client_task,
                                     NULL, 14, 896);
        TEST_ASSERT(client_id >= 0, "cap recycle client created");
    }

    tcb_t *client_tcb = task_get_tcb(client_id);
    if (client_tcb != NULL) {
        client_ns_cap = cap_create_for(client_tcb, endpoint_obj_for_cap(ns_ep), CAP_OBJ_ENDPOINT,
                                       CAP_READ | CAP_WRITE);
        client_service_cap = cap_create_for(client_tcb, endpoint_obj_for_cap(service_ep), CAP_OBJ_ENDPOINT,
                                            CAP_READ | CAP_WRITE | CAP_TRANSFER);
    }
    TEST_ASSERT(client_ns_cap >= 0, "cap recycle client receives ns cap");
    TEST_ASSERT(client_service_cap >= 0,
                "cap recycle client receives service cap");

    if (client_id >= 0 && client_ns_cap >= 0 && client_service_cap >= 0) {
        err = task_start(client_id);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "cap recycle client started");
    }

    void *retval = NULL;
    if (client_id >= 0) {
        err = task_join(client_id, &retval, 2000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "cap recycle client joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "cap recycle client retval OK");
    }

    retval = NULL;
    if (ns_id >= 0) {
        err = task_join(ns_id, &retval, 2000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "cap recycle name server joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "cap recycle name server retval OK");
    }

    if (client_id >= 0 && task_get_state(client_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(client_id);
    }
    if (ns_id >= 0 && task_get_state(ns_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(ns_id);
    }
    if (service_ep >= 0) {
        (void)endpoint_delete(service_ep);
    }
    if (root_id >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(root_id),
                       "cap recycle root deleted");
    }
    TEST_ASSERT_EQ((int)cap_free_after_root + 3, (int)cap_free_count(),
                   "cap recycle cleanup restored caps");
}

static void test_nameserver_registry_full_service(void) {
    test_section("Test 17: name server registry full service");

    root_bootstrap_init();

    task_id_t root_id = KERN_INVALID_ID;
    kern_err_t err = root_bootstrap_create("root_ns_full", root_dummy_task,
                                           NULL, 12, 512, &root_id);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "registry full creates root");
    if (err != KERN_OK || root_id < 0) {
        return;
    }

    uint16_t cap_free_after_root = cap_free_count();
    task_id_t ns_id = KERN_INVALID_ID;
    cap_id_t ns_task_cap = KERN_INVALID_ID;
    err = root_bootstrap_create_service("ns_full",
                                        nameserver_registry_full_task,
                                        NULL, 13, 1536,
                                        &ns_id, &ns_task_cap);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "registry full name server task created");

    ep_id_t ns_ep = KERN_INVALID_ID;
    cap_id_t root_ep_cap = KERN_INVALID_ID;
    cap_id_t ns_ep_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = root_bootstrap_create_service_endpoint(ns_task_cap,
                                                     "ns_full_ep",
                                                     KERN_EP_MSG_SIZE,
                                                     4,
                                                     &ns_ep,
                                                     &root_ep_cap,
                                                     &ns_ep_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "registry full endpoint created");

    if (err == KERN_OK) {
        err = root_bootstrap_start_service(ns_task_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "registry full name server task started");

    ep_id_t service_ep = endpoint_create("svc_full", KERN_EP_MSG_SIZE, 2);
    TEST_ASSERT(service_ep >= 0, "registry full service endpoint created");

    task_id_t client_id = KERN_INVALID_ID;
    cap_id_t client_ns_cap = KERN_INVALID_ID;
    cap_id_t client_service_cap = KERN_INVALID_ID;
    if (service_ep >= 0) {
        client_id = task_create_user("ns_full_client",
                                     nameserver_registry_full_client_task,
                                     NULL, 14, 896);
        TEST_ASSERT(client_id >= 0, "registry full client created");
    }

    tcb_t *client_tcb = task_get_tcb(client_id);
    if (client_tcb != NULL) {
        client_ns_cap = cap_create_for(client_tcb, endpoint_obj_for_cap(ns_ep), CAP_OBJ_ENDPOINT,
                                       CAP_READ | CAP_WRITE);
        client_service_cap = cap_create_for(client_tcb, endpoint_obj_for_cap(service_ep), CAP_OBJ_ENDPOINT,
                                            CAP_READ | CAP_WRITE | CAP_TRANSFER);
    }
    TEST_ASSERT(client_ns_cap >= 0, "registry full client receives ns cap");
    TEST_ASSERT(client_service_cap >= 0,
                "registry full client receives service cap");

    if (client_id >= 0 && client_ns_cap >= 0 && client_service_cap >= 0) {
        err = task_start(client_id);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "registry full client started");
    }

    void *retval = NULL;
    if (client_id >= 0) {
        err = task_join(client_id, &retval, 2000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "registry full client joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "registry full client retval OK");
    }

    retval = NULL;
    if (ns_id >= 0) {
        err = task_join(ns_id, &retval, 2000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "registry full name server joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "registry full name server retval OK");
    }

    if (client_id >= 0 && task_get_state(client_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(client_id);
    }
    if (ns_id >= 0 && task_get_state(ns_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(ns_id);
    }
    if (service_ep >= 0) {
        (void)endpoint_delete(service_ep);
    }
    if (root_id >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(root_id),
                       "registry full root deleted");
    }
    TEST_ASSERT_EQ((int)cap_free_after_root + 3, (int)cap_free_count(),
                   "registry full cleanup restored caps");
}

static void test_nameserver_unregister_owner_service(void) {
    test_section("Test 18: name server unregister owner service");

    root_bootstrap_init();

    task_id_t root_id = KERN_INVALID_ID;
    kern_err_t err = root_bootstrap_create("root_ns_owner", root_dummy_task,
                                           NULL, 12, 512, &root_id);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "owner unregister creates root");
    if (err != KERN_OK || root_id < 0) {
        return;
    }

    uint16_t cap_free_after_root = cap_free_count();
    task_id_t ns_id = KERN_INVALID_ID;
    cap_id_t ns_task_cap = KERN_INVALID_ID;
    err = root_bootstrap_create_service("ns_owner",
                                        nameserver_owner_task,
                                        NULL, 13, 1536,
                                        &ns_id, &ns_task_cap);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "owner unregister name server task created");

    ep_id_t ns_ep = KERN_INVALID_ID;
    cap_id_t root_ep_cap = KERN_INVALID_ID;
    cap_id_t ns_ep_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = root_bootstrap_create_service_endpoint(ns_task_cap,
                                                     "ns_owner_ep",
                                                     KERN_EP_MSG_SIZE,
                                                     4,
                                                     &ns_ep,
                                                     &root_ep_cap,
                                                     &ns_ep_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "owner unregister endpoint created");

    if (err == KERN_OK) {
        err = root_bootstrap_start_service(ns_task_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "owner unregister name server task started");

    ep_id_t service_ep = endpoint_create("svc_owner", KERN_EP_MSG_SIZE, 2);
    TEST_ASSERT(service_ep >= 0, "owner unregister service endpoint created");

    task_id_t reg_client = KERN_INVALID_ID;
    task_id_t bad_client = KERN_INVALID_ID;
    task_id_t good_client = KERN_INVALID_ID;
    cap_id_t reg_ns_cap = KERN_INVALID_ID;
    cap_id_t reg_service_cap = KERN_INVALID_ID;
    cap_id_t bad_ns_cap = KERN_INVALID_ID;
    cap_id_t good_ns_cap = KERN_INVALID_ID;

    if (service_ep >= 0) {
        reg_client = task_create_user("ns_owner_reg",
                                      nameserver_register_client_task,
                                      NULL, 14, 768);
        bad_client = task_create_user("ns_bad_unreg",
                                      nameserver_bad_owner_unregister_client_task,
                                      NULL, 14, 768);
        good_client = task_create_user("ns_good_unreg",
                                       nameserver_unregister_client_task,
                                       NULL, 14, 768);
        TEST_ASSERT(reg_client >= 0, "owner register client created");
        TEST_ASSERT(bad_client >= 0, "bad owner unregister client created");
        TEST_ASSERT(good_client >= 0, "good owner unregister client created");
    }

    tcb_t *reg_tcb = task_get_tcb(reg_client);
    if (reg_tcb != NULL) {
        reg_ns_cap = cap_create_for(reg_tcb, endpoint_obj_for_cap(ns_ep), CAP_OBJ_ENDPOINT,
                                    CAP_READ | CAP_WRITE);
        reg_service_cap = cap_create_for(reg_tcb, endpoint_obj_for_cap(service_ep), CAP_OBJ_ENDPOINT,
                                         CAP_READ | CAP_WRITE | CAP_TRANSFER);
    }
    tcb_t *bad_tcb = task_get_tcb(bad_client);
    if (bad_tcb != NULL) {
        bad_ns_cap = cap_create_for(bad_tcb, endpoint_obj_for_cap(ns_ep), CAP_OBJ_ENDPOINT,
                                    CAP_READ | CAP_WRITE);
    }
    tcb_t *good_tcb = task_get_tcb(good_client);
    if (good_tcb != NULL) {
        good_ns_cap = cap_create_for(good_tcb, endpoint_obj_for_cap(ns_ep), CAP_OBJ_ENDPOINT,
                                     CAP_READ | CAP_WRITE);
    }
    TEST_ASSERT(reg_ns_cap >= 0, "owner register client receives ns cap");
    TEST_ASSERT(reg_service_cap >= 0,
                "owner register client receives service cap");
    TEST_ASSERT(bad_ns_cap >= 0, "bad owner client receives ns cap");
    TEST_ASSERT(good_ns_cap >= 0, "good owner client receives ns cap");

    if (reg_client >= 0 && reg_ns_cap >= 0 && reg_service_cap >= 0) {
        err = task_start(reg_client);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "owner register client started");
    }

    void *retval = NULL;
    if (reg_client >= 0) {
        err = task_join(reg_client, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "owner register client joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "owner register client retval OK");
    }

    if (bad_client >= 0 && bad_ns_cap >= 0) {
        if (bad_tcb != NULL && bad_tcb->sp != NULL) {
            uint32_t *stacked_r0 =
                (uint32_t *)((uint8_t *)bad_tcb->sp + 32U);
            *stacked_r0 = (uint32_t)bad_ns_cap;
        }
        err = task_start(bad_client);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "bad owner unregister client started");
    }

    retval = NULL;
    if (bad_client >= 0) {
        err = task_join(bad_client, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "bad owner unregister client joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "bad owner unregister rejected");
    }

    if (good_client >= 0 && good_ns_cap >= 0) {
        if (good_tcb != NULL && good_tcb->sp != NULL) {
            uint32_t *stacked_r0 =
                (uint32_t *)((uint8_t *)good_tcb->sp + 32U);
            *stacked_r0 = (uint32_t)good_ns_cap;
        }
        err = task_start(good_client);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "good owner unregister client started");
    }

    retval = NULL;
    if (good_client >= 0) {
        err = task_join(good_client, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "good owner unregister client joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "good owner unregister retval OK");
    }

    retval = NULL;
    if (ns_id >= 0) {
        err = task_join(ns_id, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "owner unregister name server joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "owner unregister name server retval OK");
    }

    if (reg_client >= 0 &&
        task_get_state(reg_client) != TASK_STATE_TERMINATED) {
        (void)task_delete(reg_client);
    }
    if (bad_client >= 0 &&
        task_get_state(bad_client) != TASK_STATE_TERMINATED) {
        (void)task_delete(bad_client);
    }
    if (good_client >= 0 &&
        task_get_state(good_client) != TASK_STATE_TERMINATED) {
        (void)task_delete(good_client);
    }
    if (ns_id >= 0 && task_get_state(ns_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(ns_id);
    }
    if (service_ep >= 0) {
        (void)endpoint_delete(service_ep);
    }
    if (root_id >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(root_id),
                       "owner unregister root deleted");
    }
    TEST_ASSERT_EQ((int)cap_free_after_root + 3, (int)cap_free_count(),
                   "owner unregister cleanup restored caps");
}

static void test_fs_server_basic_session(void) {
    test_section("Test 24: FS server basic read/write/readdir session");

    root_bootstrap_init();

    task_id_t root_id = KERN_INVALID_ID;
    kern_err_t err = root_bootstrap_create("root_fs", root_dummy_task,
                                           NULL, 12, 512, &root_id);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "FS server test creates root");
    if (err != KERN_OK || root_id < 0) {
        return;
    }

    uint16_t cap_free_after_root = cap_free_count();
    (void)service_model_tmp_unlink("fssvc");

    ep_id_t fs_ep = endpoint_create("fs_svc", KERN_EP_MSG_SIZE, 2);
    TEST_ASSERT(fs_ep >= 0, "FS server endpoint created");

    task_id_t fs_id = KERN_INVALID_ID;
    task_id_t client_id = KERN_INVALID_ID;
    cap_id_t fs_service_cap = KERN_INVALID_ID;
    cap_id_t client_fs_cap = KERN_INVALID_ID;

    if (fs_ep >= 0) {
        fs_id = task_create_user("fs_svc",
                                 fs_service_session_task,
                                 NULL, 13, 2048);
        client_id = task_create_user("fs_client",
                                     fs_client_session_task,
                                     NULL, 14, 1024);
    }
    TEST_ASSERT(fs_id >= 0, "FS server task created");
    TEST_ASSERT(client_id >= 0, "FS client task created");

    tcb_t *fs_tcb = task_get_tcb(fs_id);
    if (fs_tcb != NULL) {
        fs_service_cap = cap_create_for(fs_tcb, endpoint_obj_for_cap(fs_ep), CAP_OBJ_ENDPOINT,
                                        CAP_READ | CAP_WRITE);
    }
    tcb_t *client_tcb = task_get_tcb(client_id);
    if (client_tcb != NULL) {
        client_fs_cap = cap_create_for(client_tcb, endpoint_obj_for_cap(fs_ep), CAP_OBJ_ENDPOINT,
                                       CAP_READ | CAP_WRITE);
    }
    TEST_ASSERT(fs_service_cap >= 0, "FS server receives endpoint cap");
    TEST_ASSERT(client_fs_cap >= 0, "FS client receives endpoint cap");

    if (fs_id >= 0 && fs_service_cap >= 0 && fs_tcb != NULL &&
        fs_tcb->sp != NULL) {
        uint32_t *stacked_r0 = (uint32_t *)((uint8_t *)fs_tcb->sp + 32U);
        *stacked_r0 = (uint32_t)fs_service_cap;
        err = task_start(fs_id);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "FS server task started");
    }

    if (client_id >= 0 && client_fs_cap >= 0 && client_tcb != NULL &&
        client_tcb->sp != NULL) {
        uint32_t *stacked_r0 =
            (uint32_t *)((uint8_t *)client_tcb->sp + 32U);
        *stacked_r0 = (uint32_t)client_fs_cap;
        err = task_start(client_id);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "FS client task started");
    }

    void *retval = NULL;
    if (client_id >= 0) {
        err = task_join(client_id, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "FS client task joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "FS client session retval OK");
    }

    retval = NULL;
    if (fs_id >= 0) {
        err = task_join(fs_id, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "FS server task joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "FS server session retval OK");
    }

    if (client_id >= 0 &&
        task_get_state(client_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(client_id);
    }
    if (fs_id >= 0 && task_get_state(fs_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(fs_id);
    }
    if (fs_ep >= 0) {
        (void)endpoint_delete(fs_ep);
    }
    err = service_model_tmp_unlink("fssvc");
    TEST_ASSERT(err == KERN_OK || err == KERN_ERR_NOEXIST,
                "FS server test file removed");
    err = service_model_tmp_unlink("fsdir");
    TEST_ASSERT(err == KERN_OK || err == KERN_ERR_NOEXIST,
                "FS server test directory removed");
    if (root_id >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(root_id),
                       "FS server root deleted");
    }
    TEST_ASSERT_EQ((int)cap_free_after_root + 3, (int)cap_free_count(),
                   "FS server cleanup restored caps");
}

static void test_fs_server_negative_protocol(void) {
    test_section("Test 25: FS server protocol and error paths");

    root_bootstrap_init();

    task_id_t root_id = KERN_INVALID_ID;
    kern_err_t err = root_bootstrap_create("root_fsneg", root_dummy_task,
                                           NULL, 12, 512, &root_id);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "FS negative test creates root");
    if (err != KERN_OK || root_id < 0) {
        return;
    }

    uint16_t cap_free_after_root = cap_free_count();
    ep_id_t fs_ep = endpoint_create("fs_neg", KERN_EP_MSG_SIZE, 2);
    TEST_ASSERT(fs_ep >= 0, "FS negative endpoint created");

    task_id_t fs_id = KERN_INVALID_ID;
    cap_id_t fs_service_cap = KERN_INVALID_ID;
    if (fs_ep >= 0) {
        fs_id = task_create_user("fs_neg",
                                 fs_service_negative_task,
                                 NULL, 13, 1536);
    }
    TEST_ASSERT(fs_id >= 0, "FS negative server task created");

    tcb_t *fs_tcb = task_get_tcb(fs_id);
    if (fs_tcb != NULL) {
        fs_service_cap = cap_create_for(fs_tcb, endpoint_obj_for_cap(fs_ep), CAP_OBJ_ENDPOINT,
                                        CAP_READ | CAP_WRITE);
    }
    TEST_ASSERT(fs_service_cap >= 0,
                "FS negative server receives endpoint cap");

    if (fs_id >= 0 && fs_service_cap >= 0 && fs_tcb != NULL &&
        fs_tcb->sp != NULL) {
        uint32_t *stacked_r0 = (uint32_t *)((uint8_t *)fs_tcb->sp + 32U);
        *stacked_r0 = (uint32_t)fs_service_cap;
        err = task_start(fs_id);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "FS negative server task started");
    }

    fs_msg_t msg;
    fs_msg_init(&msg, FS_OP_PING, 1);
    msg.magic = 0x12345678U;
    fs_test_send_expect(fs_ep, &msg, KERN_ERR_PARAM,
                        "FS rejects bad magic");

    fs_msg_init(&msg, 0xffffU, 2);
    fs_test_send_expect(fs_ep, &msg, KERN_ERR_PARAM,
                        "FS rejects bad opcode");

    fs_msg_init(&msg, FS_OP_OPEN, 3);
    msg.flags = O_RDONLY;
    for (uint32_t i = 0; i < FS_PATH_MAX; i++) {
        msg.path[i] = 'x';
    }
    fs_test_send_expect(fs_ep, &msg, KERN_ERR_PARAM,
                        "FS rejects unterminated path");

    fs_msg_init(&msg, FS_OP_OPEN, 4);
    msg.flags = O_RDONLY;
    fs_test_copy_path(&msg, "/tmp");
    fs_test_send_expect(fs_ep, &msg, KERN_OK,
                        "FS opens directory for negative checks");
    int dir_fd = msg.result;
    TEST_ASSERT(dir_fd > 0, "FS directory fd token valid");

    fs_msg_init(&msg, FS_OP_READ, 5);
    msg.fd = dir_fd;
    msg.length = FS_PAYLOAD_MAX;
    fs_test_send_expect(fs_ep, &msg, KERN_ERR_ISDIR,
                        "FS rejects read from directory");

    fs_msg_init(&msg, FS_OP_READ, 6);
    msg.fd = 99;
    msg.length = 1;
    fs_test_send_expect(fs_ep, &msg, KERN_ERR_PARAM,
                        "FS rejects bad fd token");

    fs_msg_init(&msg, FS_OP_READ, 7);
    msg.fd = dir_fd;
    msg.length = FS_PAYLOAD_MAX + 1U;
    fs_test_send_expect(fs_ep, &msg, KERN_ERR_PARAM,
                        "FS rejects oversized read");

    fs_msg_init(&msg, FS_OP_CLOSE, 8);
    msg.fd = dir_fd;
    fs_test_send_expect(fs_ep, &msg, KERN_OK,
                        "FS closes directory fd after negative checks");

    void *retval = NULL;
    if (fs_id >= 0) {
        err = task_join(fs_id, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "FS negative server task joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "FS negative server retval OK");
    }

    if (fs_id >= 0 && task_get_state(fs_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(fs_id);
    }
    if (fs_ep >= 0) {
        (void)endpoint_delete(fs_ep);
    }
    if (root_id >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(root_id),
                       "FS negative root deleted");
    }
    TEST_ASSERT_EQ((int)cap_free_after_root + 3, (int)cap_free_count(),
                   "FS negative cleanup restored caps");
}

#endif

void test_service_model_module(void) {
#if TEST_ENABLE && CAP_ENABLE && MPU_ENABLE
    test_supervisor_service_stats();
    test_root_bootstrap_rejects_invalid_tasks();
    test_root_bootstrap_initial_task_cap();
    test_root_bootstrap_create_validation();
    test_root_bootstrap_start_policy();
    test_root_bootstrap_create_service();
    test_root_bootstrap_start_service();
    test_root_bootstrap_service_endpoint();
    test_root_bootstrap_service_ipc();
    test_nameserver_protocol_layout();
    test_nameserver_helper_validation();
    test_nameserver_ping_service();
    test_nameserver_register_service();
    test_nameserver_lookup_service();
    test_nameserver_negative_paths();
    test_nameserver_unregister_service();
    test_nameserver_protocol_error_service();
    test_nameserver_cap_recycle_service();
    test_nameserver_registry_full_service();
    test_nameserver_unregister_owner_service();
    test_fs_runtime_state_helpers();
    test_fs_runtime_lookup_release_caps();
    test_fs_server_basic_session();
    test_fs_server_negative_protocol();
#else
    test_print("Service model tests disabled\r\n");
#endif
}

TEST_K_MODULE(service_model, test_service_model_module);
