/**
 * @file test_ipc_upgrade.c
 * @brief Phase 5 IPC 升级测试 — Endpoint + Channel + syscall 补全
 */

#include "test_framework.h"
#include "kernel.h"
#include "task.h"
#include "kernel_config.h"
#include "semaphore.h"
#include "mutex.h"
#include "mqueue.h"
#include "event.h"
#include "endpoint.h"
#include "channel.h"
#include "mem.h"

#if TEST_ENABLE

/*============================================================================
 * Test 1: mutex_delete 正常工作
 *============================================================================*/

static void test_mutex_delete(void) {
    test_section("Test 1: mutex_delete");

    mutex_id_t mid = mutex_create();
    TEST_ASSERT(mid >= 0, "mutex created");

    kern_err_t err = mutex_delete(mid);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "mutex_delete returns OK");

    /* 删除后操作应失败 */
    err = mutex_lock(mid, 0);
    TEST_ASSERT(err != KERN_OK, "lock deleted mutex fails");
}

/*============================================================================
 * Test 2: mqueue_delete 正常工作
 *============================================================================*/

static void test_mqueue_delete(void) {
    test_section("Test 2: mqueue_delete");

    queue_id_t qid = mqueue_create(4, 8);
    TEST_ASSERT(qid >= 0, "mqueue created");

    kern_err_t err = mqueue_delete(qid);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "mqueue_delete returns OK");

    /* 删除后操作应失败 */
    uint32_t msg = 0x1234;
    err = mqueue_send(qid, &msg, 0);
    TEST_ASSERT(err != KERN_OK, "send to deleted queue fails");
}

/*============================================================================
 * Test 2b: mqueue KERN_WAIT_FOREVER 使用无期限调度等待
 *============================================================================*/

static queue_id_t test_mq_forever;
static volatile kern_err_t test_mq_forever_result;
static volatile uint32_t test_mq_forever_value;

static void mqueue_forever_waiter(void *arg) {
    (void)arg;

    uint32_t value = 0;
    test_mq_forever_result = mqueue_recv(test_mq_forever, &value,
                                         KERN_WAIT_FOREVER);
    test_mq_forever_value = value;
}

static void test_mqueue_wait_forever(void) {
    test_section("Test 2b: mqueue wait forever");

    test_mq_forever = mqueue_create(sizeof(uint32_t), 1);
    TEST_ASSERT(test_mq_forever >= 0, "mqueue created for forever wait");
    if (test_mq_forever < 0) {
        return;
    }

    test_mq_forever_result = KERN_ERR_TIMEOUT;
    test_mq_forever_value = 0;

    task_id_t waiter = task_create("mq_forever", mqueue_forever_waiter,
                                   NULL, 10, 0);
    TEST_ASSERT(waiter >= 0, "mqueue forever waiter created");
    if (waiter < 0) {
        (void)mqueue_delete(test_mq_forever);
        return;
    }

    (void)task_start(waiter);
    task_delay(5);

    tcb_t *waiter_tcb = task_get_tcb(waiter);
    TEST_ASSERT(waiter_tcb != NULL, "mqueue forever waiter TCB exists");
    if (waiter_tcb != NULL) {
        TEST_ASSERT_EQ((int)TASK_STATE_BLOCKED, (int)waiter_tcb->state,
                       "mqueue forever waiter remains blocked");
        TEST_ASSERT_EQ(0, (int)waiter_tcb->cont.deadline,
                       "mqueue forever waiter has no deadline");
    }

    uint32_t value = 0xA55A1234u;
    kern_err_t err = mqueue_send(test_mq_forever, &value, 0);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "send wakes mqueue forever waiter");

    err = task_join(waiter, NULL, 100);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "mqueue forever waiter joined");
    TEST_ASSERT_EQ((int)KERN_OK, (int)test_mq_forever_result,
                   "mqueue forever receive succeeds");
    TEST_ASSERT_EQ((int)value, (int)test_mq_forever_value,
                   "mqueue forever receive copies message");

    (void)mqueue_delete(test_mq_forever);
}

/*============================================================================
 * Test 3: event_delete 正常工作
 *============================================================================*/

static void test_event_delete(void) {
    test_section("Test 3: event_delete");

    event_id_t eid = event_create(0);
    TEST_ASSERT(eid >= 0, "event created");

    kern_err_t err = event_delete(eid);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "event_delete returns OK");

    /* 删除后操作应失败 */
    err = event_set(eid, 0x01);
    TEST_ASSERT(err != KERN_OK, "set deleted event fails");
}

/*============================================================================
 * Test 4: event_clear 正常工作
 *============================================================================*/

static void test_event_clear(void) {
    test_section("Test 4: event_clear");

    event_id_t eid = event_create(0xFF);
    TEST_ASSERT(eid >= 0, "event created with 0xFF");

    kern_err_t err = event_clear(eid, 0x0F);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "event_clear returns OK");

    uint32_t flags = event_get(eid);
    TEST_ASSERT_EQ((int)0xF0, (int)flags, "flags after clear = 0xF0");

    event_delete(eid);
}

/*============================================================================
 * Test 5: event_get 正常工作
 *============================================================================*/

static void test_event_get(void) {
    test_section("Test 5: event_get");

    event_id_t eid = event_create(0xAB);
    TEST_ASSERT(eid >= 0, "event created with 0xAB");

    uint32_t flags = event_get(eid);
    TEST_ASSERT_EQ((int)0xAB, (int)flags, "event_get returns 0xAB");

    event_delete(eid);
}

/*============================================================================
 * Test 6: Endpoint C/S 基本通信
 *============================================================================*/

static ep_id_t test_ep_id;
static volatile int ep_server_done;
static volatile int ep_client_done;
static volatile uint32_t ep_reply_val;

static void ep_server_task(void *arg) {
    (void)arg;
    uint32_t req = 0;
    kern_err_t err = endpoint_recv(test_ep_id, &req, 1000);
    if (err == KERN_OK) {
        uint32_t reply = req * 2;
        endpoint_reply(test_ep_id, &reply);
    }
    ep_server_done = 1;
}

static void ep_client_task(void *arg) {
    (void)arg;
    uint32_t msg = 21;
    kern_err_t err = endpoint_send(test_ep_id, &msg, 1000);
    if (err == KERN_OK) {
        ep_reply_val = msg;  /* reply 写回同一缓冲区 */
    }
    ep_client_done = 1;
}

static void test_endpoint_cs(void) {
    test_section("Test 6: Endpoint C/S basic");

    test_ep_id = endpoint_create("test_ep", sizeof(uint32_t), 4);
    TEST_ASSERT(test_ep_id >= 0, "endpoint created");

    ep_server_done = 0;
    ep_client_done = 0;
    ep_reply_val = 0;

    task_id_t server = task_create("ep_srv", ep_server_task, NULL, 10, 0);
    task_id_t client = task_create("ep_cli", ep_client_task, NULL, 11, 0);
    TEST_ASSERT(server >= 0 && client >= 0, "tasks created");

    task_start(server);
    task_start(client);

    task_delay(100);

    TEST_ASSERT(ep_server_done == 1, "server done");
    TEST_ASSERT(ep_client_done == 1, "client done");
    TEST_ASSERT_EQ((int)42, (int)ep_reply_val, "reply = 21*2 = 42");

    endpoint_delete(test_ep_id);
}

/*============================================================================
 * Test 7: Endpoint 多客户端
 *============================================================================*/

static ep_id_t test_ep_multi;
static volatile int ep_multi_count;

static void ep_multi_server(void *arg) {
    (void)arg;
    for (int i = 0; i < 3; i++) {
        uint32_t req = 0;
        if (endpoint_recv(test_ep_multi, &req, 1000) == KERN_OK) {
            uint32_t reply = req + 100;
            endpoint_reply(test_ep_multi, &reply);
        }
    }
}

static void ep_multi_client(void *arg) {
    uint32_t id = (uint32_t)(uintptr_t)arg;
    uint32_t msg = id;
    if (endpoint_send(test_ep_multi, &msg, 1000) == KERN_OK) {
        if (msg == id + 100) {
            ep_multi_count++;
        }
    }
}

static void test_endpoint_multi_client(void) {
    test_section("Test 7: Endpoint multi-client");

    test_ep_multi = endpoint_create("ep_multi", sizeof(uint32_t), 4);
    TEST_ASSERT(test_ep_multi >= 0, "endpoint created");

    ep_multi_count = 0;

    task_id_t server = task_create("ep_msrv", ep_multi_server, NULL, 12, 0);
    task_start(server);

    task_id_t clients[3];
    for (int i = 0; i < 3; i++) {
        clients[i] = task_create("ep_mcli", ep_multi_client,
                                  (void *)(uintptr_t)(i + 1), 13, 0);
        task_start(clients[i]);
    }

    task_delay(200);

    TEST_ASSERT_EQ((int)3, (int)ep_multi_count, "all 3 clients got correct reply");

    endpoint_delete(test_ep_multi);
}

/*============================================================================
 * Test 8: Endpoint 超时
 *============================================================================*/

static void test_endpoint_timeout(void) {
    test_section("Test 8: Endpoint timeout");

    ep_id_t ep = endpoint_create("ep_to", sizeof(uint32_t), 4);
    TEST_ASSERT(ep >= 0, "endpoint created");

    /* recv 无服务端发送，应超时 */
    uint32_t buf = 0;
    kern_err_t err = endpoint_recv(ep, &buf, 10);
    TEST_ASSERT_EQ((int)KERN_ERR_TIMEOUT, (int)err, "recv timeout returns TIMEOUT");

    endpoint_delete(ep);
}

/*============================================================================
 * Test 8b: Endpoint reply authority is recv-scoped
 *============================================================================*/

static ep_id_t test_ep_reply_scope;
static volatile int ep_scope_server_done;
static volatile int ep_scope_client_done;
static volatile kern_err_t ep_scope_bad_reply;
static volatile uint32_t ep_scope_reply_val;

static void ep_scope_server(void *arg) {
    (void)arg;
    uint32_t req = 0;

    if (endpoint_recv(test_ep_reply_scope, &req, 1000) == KERN_OK) {
        task_delay(20);
        req += 10;
        (void)endpoint_reply(test_ep_reply_scope, &req);
    }

    ep_scope_server_done = 1;
}

static void ep_scope_bad_replier(void *arg) {
    (void)arg;
    task_delay(5);
    uint32_t reply = 0xBAD;
    ep_scope_bad_reply = endpoint_reply(test_ep_reply_scope, &reply);
}

static void ep_scope_client(void *arg) {
    (void)arg;
    uint32_t msg = 7;
    kern_err_t err = endpoint_send(test_ep_reply_scope, &msg, 1000);
    if (err == KERN_OK) {
        ep_scope_reply_val = msg;
    }
    ep_scope_client_done = 1;
}

static void test_endpoint_reply_scoped_to_receiver(void) {
    test_section("Test 8b: Endpoint reply scoped to receiver");

    test_ep_reply_scope = endpoint_create("ep_scope", sizeof(uint32_t), 2);
    TEST_ASSERT(test_ep_reply_scope >= 0, "reply scope endpoint created");
    if (test_ep_reply_scope < 0) return;

    ep_scope_server_done = 0;
    ep_scope_client_done = 0;
    ep_scope_bad_reply = KERN_OK;
    ep_scope_reply_val = 0;

    task_id_t server = task_create("ep_ssrv", ep_scope_server, NULL, 10, 0);
    task_id_t bad = task_create("ep_sbad", ep_scope_bad_replier, NULL, 11, 0);
    task_id_t client = task_create("ep_scli", ep_scope_client, NULL, 12, 0);
    TEST_ASSERT(server >= 0 && bad >= 0 && client >= 0, "reply scope tasks created");
    if (server < 0 || bad < 0 || client < 0) {
        endpoint_delete(test_ep_reply_scope);
        return;
    }

    task_start(server);
    task_start(client);
    task_start(bad);

    task_delay(120);

    TEST_ASSERT_EQ((int)KERN_ERR_STATE, (int)ep_scope_bad_reply,
                   "non-receiver reply rejected");
    TEST_ASSERT_EQ(1, ep_scope_server_done, "receiver server completed");
    TEST_ASSERT_EQ(1, ep_scope_client_done, "scoped client completed");
    TEST_ASSERT_EQ(17, (int)ep_scope_reply_val, "valid receiver reply delivered");

    endpoint_delete(test_ep_reply_scope);
}

/*============================================================================
 * Test 8c: Endpoint reply after client timeout fails
 *============================================================================*/

static ep_id_t test_ep_reply_timeout;
static volatile kern_err_t ep_timeout_client_err;
static volatile kern_err_t ep_timeout_reply_err;
static volatile int ep_timeout_server_done;

static void ep_timeout_server(void *arg) {
    (void)arg;
    uint32_t req = 0;

    if (endpoint_recv(test_ep_reply_timeout, &req, 1000) == KERN_OK) {
        task_delay(40);
        req += 1;
        ep_timeout_reply_err = endpoint_reply(test_ep_reply_timeout, &req);
    }
    ep_timeout_server_done = 1;
}

static void ep_timeout_client(void *arg) {
    (void)arg;
    uint32_t msg = 11;
    ep_timeout_client_err = endpoint_send(test_ep_reply_timeout, &msg, 10);
}

static void test_endpoint_reply_after_timeout(void) {
    test_section("Test 8c: Endpoint reply after timeout");

    test_ep_reply_timeout = endpoint_create("ep_rto", sizeof(uint32_t), 2);
    TEST_ASSERT(test_ep_reply_timeout >= 0, "reply timeout endpoint created");
    if (test_ep_reply_timeout < 0) return;

    ep_timeout_client_err = KERN_OK;
    ep_timeout_reply_err = KERN_OK;
    ep_timeout_server_done = 0;

    task_id_t server = task_create("ep_tsrv", ep_timeout_server, NULL, 10, 0);
    task_id_t client = task_create("ep_tcli", ep_timeout_client, NULL, 11, 0);
    TEST_ASSERT(server >= 0 && client >= 0, "reply timeout tasks created");
    if (server < 0 || client < 0) {
        endpoint_delete(test_ep_reply_timeout);
        return;
    }

    task_start(server);
    task_start(client);
    task_delay(120);

    TEST_ASSERT_EQ((int)KERN_ERR_TIMEOUT, (int)ep_timeout_client_err,
                   "client send timed out");
    TEST_ASSERT_EQ(1, ep_timeout_server_done, "timeout server completed");
    TEST_ASSERT_EQ((int)KERN_ERR_NOEXIST, (int)ep_timeout_reply_err,
                   "reply after timeout rejected");

    endpoint_delete(test_ep_reply_timeout);
}

/*============================================================================
 * Test 9: Endpoint capability copy transfer
 *============================================================================*/

static ep_id_t test_ep_cap;
static cap_id_t ep_cap_src;
static volatile int ep_cap_server_done;
static volatile int ep_cap_client_done;
static volatile int ep_cap_server_ok;
static volatile int ep_cap_client_ok;
static int ep_cap_object;
static cap_id_t ep_shm_src;
static volatile int ep_shm_server_done;
static volatile int ep_shm_client_done;
static volatile int ep_shm_server_ok;
static volatile int ep_shm_client_ok;

static void ep_cap_server(void *arg) {
    (void)arg;

    uint32_t msg = 0;
    cap_id_t caps[IPC_CAPS_MAX];
    uint8_t cap_count = 0;

    kern_err_t err = endpoint_recv_caps(test_ep_cap, &msg,
                                        caps, &cap_count, 1000);
    if (err == KERN_OK && cap_count == 1) {
        void *ptr = cap_lookup_for(sched_get_current(), caps[0],
                                   CAP_OBJ_ENDPOINT, CAP_READ);
        if (ptr == &ep_cap_object) {
            ep_cap_server_ok = 1;
        }
        uint32_t reply = msg + 1;
        endpoint_reply(test_ep_cap, &reply);
    }
    ep_cap_server_done = 1;
}

static void ep_cap_client(void *arg) {
    (void)arg;

    ipc_cap_xfer_t xfer;
    xfer.src_cap = ep_cap_src;
    xfer.rights = CAP_READ;
    xfer.flags = IPC_CAP_COPY;

    uint32_t msg = 40;
    kern_err_t err = endpoint_send_caps(test_ep_cap, &msg, &xfer, 1, 1000);
    if (err == KERN_OK && msg == 41) {
        void *ptr = cap_lookup_for(sched_get_current(), ep_cap_src,
                                   CAP_OBJ_ENDPOINT, CAP_TRANSFER);
        if (ptr == &ep_cap_object) {
            ep_cap_client_ok = 1;
        }
    }
    ep_cap_client_done = 1;
}

static void test_endpoint_cap_transfer(void) {
    test_section("Test 9: Endpoint capability transfer");

    test_ep_cap = endpoint_create("ep_cap", sizeof(uint32_t), 4);
    TEST_ASSERT(test_ep_cap >= 0, "endpoint created");

    ep_cap_object = 1234;
    ep_cap_server_done = 0;
    ep_cap_client_done = 0;
    ep_cap_server_ok = 0;
    ep_cap_client_ok = 0;

    task_id_t server = task_create("ep_caps", ep_cap_server, NULL, 10, 0);
    task_id_t client = task_create("ep_capc", ep_cap_client, NULL, 11, 0);
    TEST_ASSERT(server >= 0 && client >= 0, "tasks created");

    tcb_t *client_tcb = task_get_tcb(client);
    TEST_ASSERT(client_tcb != NULL, "client TCB available");
    ep_cap_src = cap_create_for(client_tcb, &ep_cap_object, CAP_OBJ_ENDPOINT,
                                CAP_READ | CAP_TRANSFER);
    TEST_ASSERT(ep_cap_src != ((cap_id_t)-1), "source cap created");

    task_start(server);
    task_start(client);

    task_delay(100);

    TEST_ASSERT(ep_cap_server_done == 1, "server done");
    TEST_ASSERT(ep_cap_client_done == 1, "client done");
    TEST_ASSERT(ep_cap_server_ok == 1, "server received copied cap");
    TEST_ASSERT(ep_cap_client_ok == 1, "client kept source cap");

    endpoint_delete(test_ep_cap);
}

static void ep_shm_server(void *arg) {
    (void)arg;

    uint32_t msg = 0;
    cap_id_t caps[IPC_CAPS_MAX];
    uint8_t cap_count = 0;

    kern_err_t err = endpoint_recv_caps(test_ep_cap, &msg,
                                        caps, &cap_count, 1000);
    if (err == KERN_OK && cap_count == 1) {
        void *base = NULL;
        size_t size = 0;
        void *range = NULL;
        kern_err_t read_err = kshm_get_bounds(caps[0], &base, &size);
        kern_err_t range_err = kshm_get_range(caps[0], CAP_READ, 8, 8, &range);
        kern_err_t write_err = kshm_get_range(caps[0], CAP_WRITE, 0, 4, &range);
        if (read_err == KERN_OK && size == 96 && base != NULL &&
            range_err == KERN_OK && write_err == KERN_ERR_CAP) {
            ep_shm_server_ok = 1;
        }
        uint32_t reply = msg + 2;
        endpoint_reply(test_ep_cap, &reply);
    }
    ep_shm_server_done = 1;
}

static void ep_shm_client(void *arg) {
    (void)arg;

    ipc_cap_xfer_t xfer;
    xfer.src_cap = ep_shm_src;
    xfer.rights = CAP_READ;
    xfer.flags = IPC_CAP_COPY;

    uint32_t msg = 50;
    kern_err_t err = endpoint_send_caps(test_ep_cap, &msg, &xfer, 1, 1000);
    if (err == KERN_OK && msg == 52) {
        void *range = NULL;
        kern_err_t write_err = kshm_get_range(ep_shm_src, CAP_WRITE, 0, 4, &range);
        if (write_err == KERN_OK) {
            ep_shm_client_ok = 1;
        }
    }
    ep_shm_client_done = 1;
}

static void test_endpoint_shm_cap_transfer(void) {
    test_section("Test 9b: Endpoint SHM capability transfer");

    uint32_t outstanding = mem_get_outstanding_allocs();
    test_ep_cap = endpoint_create("ep_shm", sizeof(uint32_t), 4);
    TEST_ASSERT(test_ep_cap >= 0, "shm endpoint created");

    ep_shm_server_done = 0;
    ep_shm_client_done = 0;
    ep_shm_server_ok = 0;
    ep_shm_client_ok = 0;
    ep_shm_src = KERN_INVALID_ID;

    task_id_t server = task_create("ep_shms", ep_shm_server, NULL, 10, 0);
    task_id_t client = task_create("ep_shmc", ep_shm_client, NULL, 11, 0);
    TEST_ASSERT(server >= 0 && client >= 0, "shm tasks created");

    tcb_t *client_tcb = task_get_tcb(client);
    TEST_ASSERT(client_tcb != NULL, "shm client TCB available");

    cap_id_t root = kshm_create_cap(96,
                                    CAP_READ | CAP_WRITE |
                                    CAP_MANAGE | CAP_TRANSFER);
    TEST_ASSERT(root >= 0, "shm root cap created");
    if (root >= 0 && client_tcb != NULL) {
        ep_shm_src = cap_copy_to(NULL, root, client_tcb,
                                 CAP_READ | CAP_WRITE | CAP_TRANSFER);
    }
    TEST_ASSERT(ep_shm_src >= 0, "shm source cap copied to client");

    if (server >= 0) task_start(server);
    if (client >= 0) task_start(client);

    task_delay(100);

    TEST_ASSERT(ep_shm_server_done == 1, "shm server done");
    TEST_ASSERT(ep_shm_client_done == 1, "shm client done");
    TEST_ASSERT(ep_shm_server_ok == 1, "server received read-only shm cap");
    TEST_ASSERT(ep_shm_client_ok == 1, "client kept writable shm source cap");

    if (root >= 0) {
        kern_err_t err = kshm_delete_cap(root);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err, "shm root revoke OK");
    }
    if (ep_shm_src >= 0) {
        void *base = NULL;
        size_t size = 0;
        kern_err_t err = kshm_get_bounds(ep_shm_src, &base, &size);
        TEST_ASSERT_EQ((int)KERN_ERR_CAP, (int)err,
                       "shm copied source invalid after root revoke");
    }

    endpoint_delete(test_ep_cap);
    TEST_ASSERT_EQ((int)outstanding, (int)mem_get_outstanding_allocs(),
                   "shm endpoint transfer cleanup restored outstanding");
}

/*============================================================================
 * Test 10: Channel P2P 双向通信
 *============================================================================*/

static ch_id_t test_ch_id;
static volatile int ch_a_done;
static volatile int ch_b_done;
static volatile uint32_t ch_b_received;

static void ch_peer_a(void *arg) {
    (void)arg;
    /* A 发送 10 给 B */
    uint32_t msg = 10;
    kern_err_t err = channel_send(test_ch_id, &msg, 1000);
    if (err == KERN_OK) {
        /* A 接收 B 的回复 */
        uint32_t reply = 0;
        err = channel_recv(test_ch_id, &reply, 1000);
        if (err == KERN_OK && reply == 20) {
            ch_a_done = 1;
        }
    }
}

static void ch_peer_b(void *arg) {
    (void)arg;
    /* B 接收 A 的消息 */
    uint32_t msg = 0;
    kern_err_t err = channel_recv(test_ch_id, &msg, 1000);
    if (err == KERN_OK) {
        ch_b_received = msg;
        /* B 发送回复 20 给 A */
        uint32_t reply = 20;
        channel_send(test_ch_id, &reply, 1000);
    }
    ch_b_done = 1;
}

static void test_channel_p2p(void) {
    test_section("Test 10: Channel P2P bidirectional");

    test_ch_id = channel_create(sizeof(uint32_t), 0);
    TEST_ASSERT(test_ch_id >= 0, "channel created");

    ch_a_done = 0;
    ch_b_done = 0;
    ch_b_received = 0;

    task_id_t a = task_create("ch_a", ch_peer_a, NULL, 10, 0);
    task_id_t b = task_create("ch_b", ch_peer_b, NULL, 11, 0);
    TEST_ASSERT(a >= 0 && b >= 0, "tasks created");

    /* 连接 peer */
    channel_connect(test_ch_id, a, b);

    task_start(a);
    task_start(b);

    task_delay(100);

    TEST_ASSERT_EQ((int)10, (int)ch_b_received, "B received 10 from A");
    TEST_ASSERT(ch_a_done == 1, "A done");
    TEST_ASSERT(ch_b_done == 1, "B done");

    channel_delete(test_ch_id);
}

/*============================================================================
 * Test 11: Channel capability copy transfer
 *============================================================================*/

static ch_id_t test_ch_cap;
static cap_id_t ch_cap_src;
static volatile int ch_cap_a_done;
static volatile int ch_cap_b_done;
static volatile int ch_cap_a_ok;
static volatile int ch_cap_b_ok;
static int ch_cap_object;

static void ch_cap_peer_a(void *arg) {
    (void)arg;

    ipc_cap_xfer_t xfer;
    xfer.src_cap = ch_cap_src;
    xfer.rights = CAP_READ;
    xfer.flags = IPC_CAP_COPY;

    uint32_t msg = 77;
    kern_err_t err = channel_send_caps(test_ch_cap, &msg, &xfer, 1, 1000);
    if (err == KERN_OK) {
        for (int i = 0; i < 100 && ch_cap_b_done == 0; i++) {
            task_delay(1);
        }
        void *ptr = cap_lookup_for(sched_get_current(), ch_cap_src,
                                   CAP_OBJ_ENDPOINT, CAP_TRANSFER);
        if (ptr == &ch_cap_object) {
            ch_cap_a_ok = 1;
        }
    }
    ch_cap_a_done = 1;
}

static void ch_cap_peer_b(void *arg) {
    (void)arg;

    uint32_t msg = 0;
    cap_id_t caps[IPC_CAPS_MAX];
    uint8_t cap_count = 0;

    kern_err_t err = channel_recv_caps(test_ch_cap, &msg,
                                       caps, &cap_count, 1000);
    if (err == KERN_OK && msg == 77 && cap_count == 1) {
        void *ptr = cap_lookup_for(sched_get_current(), caps[0],
                                   CAP_OBJ_ENDPOINT, CAP_READ);
        if (ptr == &ch_cap_object) {
            ch_cap_b_ok = 1;
        }
    }
    ch_cap_b_done = 1;
}

static void test_channel_cap_transfer(void) {
    test_section("Test 11: Channel capability transfer");

    test_ch_cap = channel_create(sizeof(uint32_t), 0);
    TEST_ASSERT(test_ch_cap >= 0, "channel created");

    ch_cap_object = 5678;
    ch_cap_a_done = 0;
    ch_cap_b_done = 0;
    ch_cap_a_ok = 0;
    ch_cap_b_ok = 0;

    task_id_t a = task_create("ch_capa", ch_cap_peer_a, NULL, 10, 0);
    task_id_t b = task_create("ch_capb", ch_cap_peer_b, NULL, 11, 0);
    TEST_ASSERT(a >= 0 && b >= 0, "tasks created");

    tcb_t *a_tcb = task_get_tcb(a);
    TEST_ASSERT(a_tcb != NULL, "peer A TCB available");
    ch_cap_src = cap_create_for(a_tcb, &ch_cap_object, CAP_OBJ_ENDPOINT,
                                CAP_READ | CAP_TRANSFER);
    TEST_ASSERT(ch_cap_src != ((cap_id_t)-1), "source cap created");

    kern_err_t err = channel_connect(test_ch_cap, a, b);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "channel connected");

    task_start(b);
    task_start(a);

    task_delay(100);

    TEST_ASSERT(ch_cap_a_done == 1, "peer A done");
    TEST_ASSERT(ch_cap_b_done == 1, "peer B done");
    TEST_ASSERT(ch_cap_a_ok == 1, "peer A kept source cap");
    TEST_ASSERT(ch_cap_b_ok == 1, "peer B received copied cap");

    channel_delete(test_ch_cap);
}

/*============================================================================
 * Test 12: Channel 共享内存
 *============================================================================*/

static void test_channel_shm(void) {
    test_section("Test 12: Channel shared memory");

    ch_id_t ch = channel_create(sizeof(uint32_t), 64);
    TEST_ASSERT(ch >= 0, "channel created with shm");

    void *shm = channel_get_shm(ch);
    TEST_ASSERT_NOT_NULL(shm, "shm pointer not null");

    /* 写入共享内存 */
    volatile uint32_t *p = (volatile uint32_t *)shm;
    *p = 0xDEADBEEF;
    TEST_ASSERT_EQ((int)0xDEADBEEF, (int)*p, "shm readback matches");

    channel_delete(ch);
}

/*============================================================================
 * Test 13: Channel peer permission checks
 *============================================================================*/

static void ch_dummy_peer(void *arg) {
    (void)arg;
}

static void test_channel_peer_permissions(void) {
    test_section("Test 13: Channel peer permissions");

    ch_id_t ch = channel_create(sizeof(uint32_t), 0);
    TEST_ASSERT(ch >= 0, "channel created");

    uint32_t msg = 1;
    kern_err_t err = channel_send(ch, &msg, 0);
    TEST_ASSERT_EQ((int)KERN_ERR_STATE, (int)err,
                   "send before connect rejected");

    err = channel_recv(ch, &msg, 0);
    TEST_ASSERT_EQ((int)KERN_ERR_STATE, (int)err,
                   "recv before connect rejected");

    task_id_t a = task_create("ch_pa", ch_dummy_peer, NULL, 14, 512);
    task_id_t b = task_create("ch_pb", ch_dummy_peer, NULL, 14, 512);
    TEST_ASSERT(a >= 0 && b >= 0, "peer tasks created");

    err = channel_connect(ch, a, b);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "channel connected");

    err = channel_send(ch, &msg, 0);
    TEST_ASSERT_EQ((int)KERN_ERR_PERM, (int)err,
                   "non-peer send rejected");

    err = channel_recv(ch, &msg, 0);
    TEST_ASSERT_EQ((int)KERN_ERR_PERM, (int)err,
                   "non-peer recv rejected");

    (void)task_delete(a);
    (void)task_delete(b);
    channel_delete(ch);
}

/*============================================================================
 * Test 14: Endpoint delete 唤醒等待者
 *============================================================================*/

static ep_id_t test_ep_del;
static volatile kern_err_t ep_del_err;

static void ep_del_waiter(void *arg) {
    (void)arg;
    uint32_t buf;
    ep_del_err = endpoint_recv(test_ep_del, &buf, 1000);
}

static void test_endpoint_delete_wakes(void) {
    test_section("Test 14: Endpoint delete wakes waiters");

    test_ep_del = endpoint_create("ep_del", sizeof(uint32_t), 4);
    TEST_ASSERT(test_ep_del >= 0, "endpoint created");

    ep_del_err = KERN_OK;
    task_id_t waiter = task_create("ep_delw", ep_del_waiter, NULL, 10, 0);
    task_start(waiter);

    task_delay(20);  /* 让 waiter 阻塞 */

    kern_err_t err = endpoint_delete(test_ep_del);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "delete returns OK");

    task_delay(20);
    TEST_ASSERT(ep_del_err != KERN_OK, "waiter woken with error");
}

/*============================================================================
 * Test 15: Channel delete 唤醒等待者
 *============================================================================*/

static ch_id_t test_ch_del;
static volatile kern_err_t ch_del_err;

static void ch_del_waiter(void *arg) {
    (void)arg;
    uint32_t buf;
    ch_del_err = channel_recv(test_ch_del, &buf, 1000);
}

static void test_channel_delete_wakes(void) {
    test_section("Test 15: Channel delete wakes waiters");

    test_ch_del = channel_create(sizeof(uint32_t), 0);
    TEST_ASSERT(test_ch_del >= 0, "channel created");

    ch_del_err = KERN_OK;
    task_id_t waiter = task_create("ch_delw", ch_del_waiter, NULL, 10, 0);

    /* 连接 peer */
    channel_connect(test_ch_del, waiter, waiter);
    task_start(waiter);

    task_delay(20);

    kern_err_t err = channel_delete(test_ch_del);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "delete returns OK");

    task_delay(20);
    TEST_ASSERT(ch_del_err != KERN_OK, "waiter woken with error");
}

/*============================================================================
 * Test 16: Channel delete 唤醒阻塞发送者
 *============================================================================*/

static ch_id_t test_ch_send_del;
static volatile kern_err_t ch_send_del_first;
static volatile kern_err_t ch_send_del_second;
static volatile int ch_send_del_done;

static void ch_send_del_sender(void *arg) {
    (void)arg;

    uint32_t msg = 0xAA55;
    ch_send_del_first = channel_send(test_ch_send_del, &msg, 1000);

    msg = 0x55AA;
    ch_send_del_second = channel_send(test_ch_send_del, &msg, 1000);
    ch_send_del_done = 1;
}

static void test_channel_delete_wakes_sender(void) {
    test_section("Test 16: Channel delete wakes sender");

    test_ch_send_del = channel_create(sizeof(uint32_t), 0);
    TEST_ASSERT(test_ch_send_del >= 0, "channel created");

    ch_send_del_first = KERN_ERR;
    ch_send_del_second = KERN_OK;
    ch_send_del_done = 0;

    task_id_t sender = task_create("ch_sdel", ch_send_del_sender, NULL, 10, 0);
    task_id_t peer = task_create("ch_speer", ch_dummy_peer, NULL, 14, 512);
    TEST_ASSERT(sender >= 0 && peer >= 0, "sender and peer created");

    kern_err_t err = channel_connect(test_ch_send_del, sender, peer);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "channel connected");

    task_start(sender);
    task_delay(20);

    TEST_ASSERT_EQ((int)KERN_OK, (int)ch_send_del_first,
                   "first send fills slot");
    TEST_ASSERT(ch_send_del_done == 0, "sender blocked on second send");

    err = channel_delete(test_ch_send_del);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "delete returns OK");

    task_delay(20);
    TEST_ASSERT(ch_send_del_done == 1, "sender woke after delete");
    TEST_ASSERT_EQ((int)KERN_ERR_NOEXIST, (int)ch_send_del_second,
                   "blocked send returns NOEXIST");

    (void)task_delete(peer);
}

/*============================================================================
 * Test 17: Endpoint client death is visible to server reply
 *============================================================================*/

static ep_id_t test_ep_death;
static volatile int ep_death_server_got;
static volatile kern_err_t ep_death_reply_err;

static void ep_death_server(void *arg) {
    (void)arg;

    uint32_t msg = 0;
    kern_err_t err = endpoint_recv(test_ep_death, &msg, 1000);
    if (err == KERN_OK) {
        ep_death_server_got = 1;
        task_delay(50);
        msg++;
        ep_death_reply_err = endpoint_reply(test_ep_death, &msg);
    }
}

static void ep_death_client(void *arg) {
    (void)arg;

    uint32_t msg = 12;
    (void)endpoint_send(test_ep_death, &msg, 1000);
}

static void test_endpoint_client_death_reply(void) {
    test_section("Test 17: Endpoint client death notification");

    test_ep_death = endpoint_create("ep_dead", sizeof(uint32_t), 2);
    TEST_ASSERT(test_ep_death >= 0, "endpoint created");

    ep_death_server_got = 0;
    ep_death_reply_err = KERN_OK;

    task_id_t server = task_create("ep_dsrv", ep_death_server, NULL, 10, 0);
    task_id_t client = task_create("ep_dcli", ep_death_client, NULL, 11, 0);
    TEST_ASSERT(server >= 0 && client >= 0, "death tasks created");

    task_start(server);
    task_start(client);
    for (int i = 0; i < 50 && ep_death_server_got == 0; i++) {
        task_delay(1);
    }

    TEST_ASSERT(ep_death_server_got == 1, "server received request");
    kern_err_t err = task_delete(client);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "delete blocked client");

    task_delay(100);
    TEST_ASSERT_EQ((int)KERN_ERR_NOEXIST, (int)ep_death_reply_err,
                   "server reply sees dead client");

    endpoint_delete(test_ep_death);
}

/*============================================================================
 * Test 18: Channel peer death is visible to sender
 *============================================================================*/

static ch_id_t test_ch_peer_dead;
static volatile kern_err_t ch_peer_dead_err;

static void ch_peer_dead_sender(void *arg) {
    (void)arg;

    uint32_t msg = 0x12345678;
    ch_peer_dead_err = channel_send(test_ch_peer_dead, &msg, 0);
}

static void test_channel_peer_death(void) {
    test_section("Test 18: Channel peer death notification");

    test_ch_peer_dead = channel_create(sizeof(uint32_t), 0);
    TEST_ASSERT(test_ch_peer_dead >= 0, "channel created");

    ch_peer_dead_err = KERN_OK;
    task_id_t sender = task_create("ch_pdead", ch_peer_dead_sender, NULL, 10, 0);
    task_id_t peer = task_create("ch_pgone", ch_dummy_peer, NULL, 11, 512);
    TEST_ASSERT(sender >= 0 && peer >= 0, "peer-death tasks created");

    kern_err_t err = channel_connect(test_ch_peer_dead, sender, peer);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "channel connected");

    err = task_delete(peer);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "peer deleted before send");

    task_start(sender);
    task_delay(20);
    TEST_ASSERT_EQ((int)KERN_ERR_NOEXIST, (int)ch_peer_dead_err,
                   "send sees dead peer");

    channel_delete(test_ch_peer_dead);
}

/*============================================================================
 * Module registration
 *============================================================================*/

static void test_ipc_upgrade_module(void) {
    /* syscall 补全 */
    test_mutex_delete();
    test_mqueue_delete();
    test_mqueue_wait_forever();
    test_event_delete();
    test_event_clear();
    test_event_get();

    /* Endpoint */
    test_endpoint_cs();
    test_endpoint_multi_client();
    test_endpoint_timeout();
    test_endpoint_reply_scoped_to_receiver();
    test_endpoint_reply_after_timeout();
    test_endpoint_cap_transfer();
    test_endpoint_shm_cap_transfer();
    test_endpoint_delete_wakes();

    /* Channel */
    test_channel_p2p();
    test_channel_cap_transfer();
    test_channel_shm();
    test_channel_peer_permissions();
    test_channel_delete_wakes();
    test_channel_delete_wakes_sender();
    test_endpoint_client_death_reply();
    test_channel_peer_death();
}

TEST_K_MODULE(ipc_upgrade, test_ipc_upgrade_module);

#endif /* TEST_ENABLE */
