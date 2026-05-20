/**
 * @file test_capability.c
 * @brief 能力系统测试 — 令牌生命周期 + 权限校验
 */

#include "test_framework.h"
#include "kernel.h"
#include "ipc_transfer.h"
#include "mem.h"
#include <string.h>

#if CAP_ENABLE && TEST_MODULE_CAP

static void cap_test_task(void *arg) {
    (void)arg;
}

/*============================================================================
 * Test 1: cap_create returns valid non-zero token
 *============================================================================*/

static void test_cap_create_basic(void) {
    test_section("Test 1: cap_create basic");
    int test_obj = 42;

    cap_id_t cap = cap_create(&test_obj, CAP_OBJ_SEMAPHORE,
                              CAP_FULL, 1);
    TEST_ASSERT(cap != ((cap_id_t)-1), "cap_create returns valid token");
    TEST_ASSERT(cap != 0, "cap_create token is non-zero");

    cap_delete(cap);
}

/*============================================================================
 * Test 2: cap_create → pool full returns CAP_INVALID
 *============================================================================*/

static void test_cap_create_pool_full(void) {
    test_section("Test 2: cap_create pool full");
    int dummy = 0;
    cap_id_t caps[CAP_MAX_COUNT];
    uint16_t free_before = cap_free_count();

    TEST_ASSERT(free_before <= CAP_MAX_COUNT, "cap free count in range");

    /* fill the pool */
    for (uint16_t i = 0; i < free_before; i++) {
        caps[i] = cap_create(&dummy, CAP_OBJ_SEMAPHORE, CAP_FULL, 1);
        TEST_ASSERT(caps[i] != ((cap_id_t)-1), "cap_create during fill");
    }

    /* should fail */
    cap_id_t over = cap_create(&dummy, CAP_OBJ_SEMAPHORE, CAP_FULL, 1);
    TEST_ASSERT(over == ((cap_id_t)-1), "cap_create returns invalid when full");

    /* clean up */
    for (uint16_t i = 0; i < free_before; i++) cap_delete(caps[i]);
}

/*============================================================================
 * Test 3: cap_resolve correct token + rights → returns object
 *============================================================================*/

static void test_cap_resolve_valid(void) {
    test_section("Test 3: cap_resolve valid");

    int obj = 123;
    cap_id_t cap = cap_create(&obj, CAP_OBJ_MUTEX, CAP_READ | CAP_WRITE, 1);
    TEST_ASSERT(cap != ((cap_id_t)-1), "create for resolve");

    void *ptr = cap_resolve(cap, CAP_OBJ_MUTEX, CAP_READ);
    TEST_ASSERT(ptr == &obj, "cap_resolve returns correct object");

    cap_delete(cap);
}

/*============================================================================
 * Test 4: cap_resolve wrong token → NULL
 *============================================================================*/

static void test_cap_resolve_bad_token(void) {
    test_section("Test 4: cap_resolve bad token");

    cap_id_t bogus = (cap_id_t)0xBEEF;
    void *ptr = cap_resolve(bogus, CAP_OBJ_SEMAPHORE, CAP_READ);
    TEST_ASSERT(ptr == NULL, "cap_resolve bad token returns NULL");
}

/*============================================================================
 * Test 5: cap_resolve type mismatch → NULL
 *============================================================================*/

static void test_cap_resolve_wrong_type(void) {
    test_section("Test 5: cap_resolve wrong type");

    int obj = 42;
    cap_id_t cap = cap_create(&obj, CAP_OBJ_SEMAPHORE, CAP_FULL, 1);
    TEST_ASSERT(cap != ((cap_id_t)-1), "create sem for type check");

    void *ptr = cap_resolve(cap, CAP_OBJ_MUTEX, CAP_READ);
    TEST_ASSERT(ptr == NULL, "cap_resolve wrong type returns NULL");

    cap_delete(cap);
}

/*============================================================================
 * Test 6: cap_resolve insufficient rights → NULL
 *============================================================================*/

static void test_cap_resolve_no_rights(void) {
    test_section("Test 6: cap_resolve insufficient rights");

    int obj = 55;
    cap_id_t cap = cap_create(&obj, CAP_OBJ_EVENT, CAP_READ, 1);
    TEST_ASSERT(cap != ((cap_id_t)-1), "create read-only");

    void *ptr = cap_resolve(cap, CAP_OBJ_EVENT, CAP_WRITE);
    TEST_ASSERT(ptr == NULL, "cap_resolve no write rights returns NULL");

    cap_delete(cap);
}

/*============================================================================
 * Test 7: cap_delete → resolve returns NULL
 *============================================================================*/

static void test_cap_delete_resolve(void) {
    test_section("Test 7: cap_delete then resolve");

    int obj = 77;
    cap_id_t cap = cap_create(&obj, CAP_OBJ_TIMER, CAP_FULL, 1);
    TEST_ASSERT(cap != ((cap_id_t)-1), "create before delete");

    cap_delete(cap);
    void *ptr = cap_resolve(cap, CAP_OBJ_TIMER, CAP_READ);
    TEST_ASSERT(ptr == NULL, "resolve after delete returns NULL");
}

/*============================================================================
 * Test 8: cap_derive creates child with subset rights
 *============================================================================*/

static void test_cap_derive_subset(void) {
    test_section("Test 8: cap_derive subset");

    int obj = 99;
    cap_id_t parent = cap_create(&obj, CAP_OBJ_MQUEUE,
                                 CAP_READ | CAP_WRITE | CAP_GRANT, 1);
    TEST_ASSERT(parent != ((cap_id_t)-1), "create parent");

    cap_id_t child = cap_derive(parent, CAP_READ);
    TEST_ASSERT(child != ((cap_id_t)-1), "derive child with read");
    TEST_ASSERT(child != parent, "child token differs from parent");

    /* child should have read access */
    void *ptr = cap_resolve(child, CAP_OBJ_MQUEUE, CAP_READ);
    TEST_ASSERT(ptr == &obj, "child has read access");

    /* child should NOT have write access */
    ptr = cap_resolve(child, CAP_OBJ_MQUEUE, CAP_WRITE);
    TEST_ASSERT(ptr == NULL, "child has no write access");

    cap_delete(child);
    cap_delete(parent);
}

/*============================================================================
 * Test 9: cap_derive superset rights fails
 *============================================================================*/

static void test_cap_derive_superset_fails(void) {
    test_section("Test 9: cap_derive superset fails");

    int obj = 11;
    cap_id_t parent = cap_create(&obj, CAP_OBJ_EVENT, CAP_READ, 1);
    TEST_ASSERT(parent != ((cap_id_t)-1), "create parent read-only");

    cap_id_t child = cap_derive(parent, CAP_READ | CAP_WRITE);
    TEST_ASSERT(child == ((cap_id_t)-1), "derive superset rights fails");

    cap_delete(parent);
}

/*============================================================================
 * Test 10: cap_derive without GRANT fails
 *============================================================================*/

static void test_cap_derive_no_grant(void) {
    test_section("Test 10: cap_derive without GRANT");

    int obj = 22;
    cap_id_t parent = cap_create(&obj, CAP_OBJ_TIMER, CAP_READ | CAP_WRITE, 1);
    TEST_ASSERT(parent != ((cap_id_t)-1), "create without GRANT");

    cap_id_t child = cap_derive(parent, CAP_READ);
    TEST_ASSERT(child == ((cap_id_t)-1), "derive without GRANT fails");

    cap_delete(parent);
}

/*============================================================================
 * Test 11: cap_revoke_all clears all owner's caps
 *============================================================================*/

static void test_cap_revoke_all(void) {
    test_section("Test 11: cap_revoke_all");

    int obj1 = 33, obj2 = 44;
    cap_id_t c1 = cap_create(&obj1, CAP_OBJ_SEMAPHORE, CAP_FULL, 5);
    cap_id_t c2 = cap_create(&obj2, CAP_OBJ_MUTEX, CAP_FULL, 5);

    TEST_ASSERT(c1 != ((cap_id_t)-1), "create c1");
    TEST_ASSERT(c2 != ((cap_id_t)-1), "create c2");

    cap_revoke_all(5);

    TEST_ASSERT(cap_resolve(c1, CAP_OBJ_SEMAPHORE, CAP_READ) == NULL,
                "c1 revoked");
    TEST_ASSERT(cap_resolve(c2, CAP_OBJ_MUTEX, CAP_READ) == NULL,
                "c2 revoked");
}

/*============================================================================
 * Test 12: cap_revoke single cap
 *============================================================================*/

static void test_cap_revoke_single(void) {
    test_section("Test 12: cap_revoke single");

    int obj = 66;
    cap_id_t cap = cap_create(&obj, CAP_OBJ_EVENT, CAP_FULL, 1);
    TEST_ASSERT(cap != ((cap_id_t)-1), "create for revoke");

    kern_err_t r = cap_revoke(cap);
    TEST_ASSERT(r == KERN_OK, "cap_revoke succeeds");

    TEST_ASSERT(cap_resolve(cap, CAP_OBJ_EVENT, CAP_READ) == NULL,
                "resolved after revoke is NULL");
}

/*============================================================================
 * Test 13: cap_create NULL object → allowed (for IPC raw ID 0)
 *============================================================================*/

static void test_cap_create_null(void) {
    test_section("Test 13: cap_create NULL");

    cap_id_t cap = cap_create(NULL, CAP_OBJ_SEMAPHORE, CAP_FULL, 1);
    TEST_ASSERT(cap != ((cap_id_t)-1), "cap_create NULL returns valid token");
    cap_delete(cap);
}

/*============================================================================
 * Test 14: cap_resolve with CAP_INVALID → NULL
 *============================================================================*/

static void test_cap_resolve_invalid(void) {
    test_section("Test 14: cap_resolve invalid cap");

    void *ptr = cap_resolve((cap_id_t)-1, CAP_OBJ_SEMAPHORE, CAP_READ);
    TEST_ASSERT(ptr == NULL, "resolve invalid cap returns NULL");
}

/*============================================================================
 * Test 15: IPC ID 0 roundtrip (simulates syscall path: id+1 offset)
 *============================================================================*/

static void test_cap_id0_roundtrip(void) {
    test_section("Test 15: IPC ID 0 roundtrip");

    /* Simulate sem_create returning raw ID 0 */
    int raw_id = 0;
    void *stored = (void *)(uintptr_t)(raw_id + 1);
    cap_id_t cap = cap_create(stored, CAP_OBJ_SEMAPHORE, CAP_FULL, 1);
    TEST_ASSERT(cap >= 0, "create with offset object OK");

    void *obj = cap_resolve(cap, CAP_OBJ_SEMAPHORE, CAP_WRITE);
    TEST_ASSERT(obj != NULL, "resolve finds cap");
    TEST_ASSERT(obj == stored, "resolve returns offset object");

    int recovered = (int)((uintptr_t)obj - 1);
    TEST_ASSERT(recovered == raw_id, "recovered raw ID matches");

    cap_delete(cap);
}

/*============================================================================
 * Test 16: 无权限操作返回失败 (KERN_ERR_CAP 路径)
 *
 * 验证 cap_resolve 在权限不足时返回 NULL，
 * 这是 syscall 返回 KERN_ERR_CAP 的底层机制。
 *============================================================================*/

static void test_cap_no_permission(void) {
    test_section("Test 16: no permission → denied");

    /* 创建只有 READ 权限的能力 */
    int obj = 99;
    cap_id_t cap = cap_create(&obj, CAP_OBJ_SEMAPHORE, CAP_READ, 1);
    TEST_ASSERT(cap >= 0, "create read-only cap");

    /* READ 应该成功 */
    void *ptr = cap_resolve(cap, CAP_OBJ_SEMAPHORE, CAP_READ);
    TEST_ASSERT(ptr == &obj, "read permission granted");

    /* WRITE 应该失败 (返回 NULL) */
    ptr = cap_resolve(cap, CAP_OBJ_SEMAPHORE, CAP_WRITE);
    TEST_ASSERT(ptr == NULL, "write permission denied");

    /* GRANT 应该失败 */
    ptr = cap_resolve(cap, CAP_OBJ_SEMAPHORE, CAP_GRANT);
    TEST_ASSERT(ptr == NULL, "grant permission denied");

    /* 全部权限 (FULL) 应该失败 */
    ptr = cap_resolve(cap, CAP_OBJ_SEMAPHORE, CAP_FULL);
    TEST_ASSERT(ptr == NULL, "full permission denied");

    /* 尝试 derive — 没有 GRANT 权限应该失败 */
    cap_id_t child = cap_derive(cap, CAP_READ);
    TEST_ASSERT(child == ((cap_id_t)-1), "derive without GRANT fails");

    cap_delete(cap);

    /* 创建无权限的能力 (rights=0) */
    cap_id_t none = cap_create(&obj, CAP_OBJ_MUTEX, 0, 2);
    TEST_ASSERT(none >= 0, "create zero-rights cap");

    ptr = cap_resolve(none, CAP_OBJ_MUTEX, CAP_READ);
    TEST_ASSERT(ptr == NULL, "zero-rights resolve fails");

    cap_delete(none);
}

/*============================================================================
 * Test 17: slot reuse rejects stale generation
 *============================================================================*/

static void test_cap_stale_generation(void) {
    test_section("Test 17: stale generation rejected");

    int obj1 = 101;
    int obj2 = 202;
    cap_id_t old_cap = cap_create(&obj1, CAP_OBJ_MUTEX, CAP_FULL, 1);
    TEST_ASSERT(old_cap != ((cap_id_t)-1), "create original cap");

    cap_delete(old_cap);

    cap_id_t new_cap = cap_create(&obj2, CAP_OBJ_MUTEX, CAP_FULL, 1);
    TEST_ASSERT(new_cap != ((cap_id_t)-1), "create replacement cap");
    TEST_ASSERT(new_cap != old_cap, "replacement cap has new generation");

    void *ptr = cap_resolve(old_cap, CAP_OBJ_MUTEX, CAP_READ);
    TEST_ASSERT(ptr == NULL, "old generation no longer resolves");

    ptr = cap_resolve(new_cap, CAP_OBJ_MUTEX, CAP_READ);
    TEST_ASSERT(ptr == &obj2, "new generation resolves");

    cap_delete(new_cap);
}

/*============================================================================
 * Test 18: parent revoke cascades to derived children
 *============================================================================*/

static void test_cap_revoke_cascade(void) {
    test_section("Test 18: revoke cascades to children");

    int obj = 303;
    cap_id_t parent = cap_create(&obj, CAP_OBJ_CHANNEL,
                                 CAP_READ | CAP_WRITE | CAP_GRANT, 1);
    TEST_ASSERT(parent != ((cap_id_t)-1), "create grant parent");

    cap_id_t child = cap_derive(parent, CAP_READ);
    TEST_ASSERT(child != ((cap_id_t)-1), "derive child");

    kern_err_t err = cap_revoke(parent);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "revoke parent OK");

    void *ptr = cap_resolve(parent, CAP_OBJ_CHANNEL, CAP_READ);
    TEST_ASSERT(ptr == NULL, "parent revoked");

    ptr = cap_resolve(child, CAP_OBJ_CHANNEL, CAP_READ);
    TEST_ASSERT(ptr == NULL, "child revoked by cascade");
}

/*============================================================================
 * Test 18b: cap_delete removes only the selected cap
 *============================================================================*/

static void test_cap_delete_preserves_children(void) {
    test_section("Test 18b: delete preserves children");

    int obj = 404;
    cap_id_t parent = cap_create(&obj, CAP_OBJ_CHANNEL,
                                 CAP_READ | CAP_WRITE | CAP_GRANT, 1);
    TEST_ASSERT(parent != ((cap_id_t)-1), "create delete parent");

    cap_id_t child = cap_derive(parent, CAP_READ);
    TEST_ASSERT(child != ((cap_id_t)-1), "derive delete child");

    cap_delete(parent);

    void *ptr = cap_resolve(parent, CAP_OBJ_CHANNEL, CAP_READ);
    TEST_ASSERT(ptr == NULL, "deleted parent invalid");

    ptr = cap_resolve(child, CAP_OBJ_CHANNEL, CAP_READ);
    TEST_ASSERT(ptr == &obj, "child survives parent delete");

    cap_delete(child);
}

/*============================================================================
 * Test 19: user lookup requires a local CSpace entry
 *============================================================================*/

static void test_cap_cspace_required_for_user(void) {
    test_section("Test 19: user lookup requires CSpace entry");

    int obj = 404;
    cap_id_t cap = cap_create(&obj, CAP_OBJ_ENDPOINT, CAP_READ, 9);
    TEST_ASSERT(cap != ((cap_id_t)-1), "create uninstalled cap");

    tcb_t fake_user;
    memset(&fake_user, 0, sizeof(fake_user));
    fake_user.id = 9;
    fake_user.attrs = TASK_ATTR_USER;

    void *ptr = cap_lookup_for(&fake_user, cap, CAP_OBJ_ENDPOINT, CAP_READ);
    TEST_ASSERT(ptr == NULL, "user without CSpace slot is denied");

    cap_delete(cap);
}

/*============================================================================
 * Test 20: cap_create_for installs into task CSpace and revoke clears it
 *============================================================================*/

static void test_cap_cspace_install_and_revoke(void) {
    test_section("Test 20: create_for installs and revoke clears CSpace");

    task_id_t tid = task_create("capcs", cap_test_task, NULL, 10, 512);
    TEST_ASSERT(tid >= 0, "create task for CSpace test");

    tcb_t *tcb = task_get_tcb(tid);
    TEST_ASSERT(tcb != NULL, "get CSpace task TCB");
    tcb->attrs = TASK_ATTR_USER;

    int obj = 505;
    cap_id_t cap = cap_create_for(tcb, &obj, CAP_OBJ_CHANNEL, CAP_READ);
    TEST_ASSERT(cap != ((cap_id_t)-1), "cap_create_for succeeds");
    TEST_ASSERT(tcb->capabilities != 0, "cap installed in task CSpace");

    void *ptr = cap_lookup_for(tcb, cap, CAP_OBJ_CHANNEL, CAP_READ);
    TEST_ASSERT(ptr == &obj, "installed user cap resolves");

    kern_err_t err = cap_revoke_for(tcb, cap);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "revoke installed cap OK");
    TEST_ASSERT(tcb->capabilities == 0, "revoke clears task CSpace slot");

    ptr = cap_lookup_for(tcb, cap, CAP_OBJ_CHANNEL, CAP_READ);
    TEST_ASSERT(ptr == NULL, "revoked user cap no longer resolves");

    (void)task_delete(tid);
}

/*============================================================================
 * Test 21: cap_copy_to installs a reduced child cap into another CSpace
 *============================================================================*/

static void test_cap_copy_to_task(void) {
    test_section("Test 21: cap_copy_to task");

    task_id_t src_id = task_create("capsrc", cap_test_task, NULL, 10, 512);
    task_id_t dst_id = task_create("capdst", cap_test_task, NULL, 10, 512);
    TEST_ASSERT(src_id >= 0 && dst_id >= 0, "create src/dst tasks");

    tcb_t *src = task_get_tcb(src_id);
    tcb_t *dst = task_get_tcb(dst_id);
    TEST_ASSERT(src != NULL && dst != NULL, "get src/dst TCBs");
    src->attrs = TASK_ATTR_USER;
    dst->attrs = TASK_ATTR_USER;

    int obj = 606;
    cap_id_t parent = cap_create_for(src, &obj, CAP_OBJ_ENDPOINT,
                                     CAP_READ | CAP_WRITE | CAP_TRANSFER);
    TEST_ASSERT(parent != ((cap_id_t)-1), "create transferable parent");

    cap_id_t copied = cap_copy_to(src, parent, dst, CAP_READ);
    TEST_ASSERT(copied != ((cap_id_t)-1), "copy reduced cap to dst");
    TEST_ASSERT(copied != parent, "copy uses distinct cap id");

    void *ptr = cap_lookup_for(src, parent, CAP_OBJ_ENDPOINT, CAP_WRITE);
    TEST_ASSERT(ptr == &obj, "source keeps parent cap");

    ptr = cap_lookup_for(dst, copied, CAP_OBJ_ENDPOINT, CAP_READ);
    TEST_ASSERT(ptr == &obj, "dst receives copied cap");

    ptr = cap_lookup_for(dst, copied, CAP_OBJ_ENDPOINT, CAP_WRITE);
    TEST_ASSERT(ptr == NULL, "copied cap has reduced rights");

    kern_err_t err = cap_revoke_for(src, parent);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "revoke parent OK");

    ptr = cap_lookup_for(dst, copied, CAP_OBJ_ENDPOINT, CAP_READ);
    TEST_ASSERT(ptr == NULL, "parent revoke cascades to copied cap");

    (void)task_delete(src_id);
    (void)task_delete(dst_id);
}

/*============================================================================
 * Test 22: cap_move_to transfers a cap between CSpaces
 *============================================================================*/

static void test_cap_move_to_task(void) {
    test_section("Test 22: cap_move_to task");

    task_id_t src_id = task_create("capmvs", cap_test_task, NULL, 10, 512);
    task_id_t dst_id = task_create("capmvd", cap_test_task, NULL, 10, 512);
    TEST_ASSERT(src_id >= 0 && dst_id >= 0, "create src/dst tasks");

    tcb_t *src = task_get_tcb(src_id);
    tcb_t *dst = task_get_tcb(dst_id);
    TEST_ASSERT(src != NULL && dst != NULL, "get src/dst TCBs");
    src->attrs = TASK_ATTR_USER;
    dst->attrs = TASK_ATTR_USER;

    int obj = 707;
    cap_id_t cap = cap_create_for(src, &obj, CAP_OBJ_CHANNEL,
                                  CAP_READ | CAP_TRANSFER);
    TEST_ASSERT(cap != ((cap_id_t)-1), "create movable cap");

    cap_id_t moved = (cap_id_t)-1;
    kern_err_t err = cap_move_to(src, cap, dst, &moved);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "move cap OK");
    TEST_ASSERT_EQ((int)cap, (int)moved, "move preserves cap id for now");

    void *ptr = cap_lookup_for(src, cap, CAP_OBJ_CHANNEL, CAP_READ);
    TEST_ASSERT(ptr == NULL, "source no longer has moved cap");

    ptr = cap_lookup_for(dst, moved, CAP_OBJ_CHANNEL, CAP_READ);
    TEST_ASSERT(ptr == &obj, "dst has moved cap");

    err = cap_revoke_for(dst, moved);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "dst revoke moved cap OK");

    (void)task_delete(src_id);
    (void)task_delete(dst_id);
}

/*============================================================================
 * Test 23: IPC cap transfer rolls back partial copies on failure
 *============================================================================*/

static void test_ipc_cap_transfer_rollback(void) {
    test_section("Test 23: IPC cap transfer rollback");

    task_id_t src_id = task_create("ipcxs", cap_test_task, NULL, 10, 512);
    task_id_t dst_id = task_create("ipcxd", cap_test_task, NULL, 10, 512);
    TEST_ASSERT(src_id >= 0 && dst_id >= 0, "create src/dst tasks");

    tcb_t *src = task_get_tcb(src_id);
    tcb_t *dst = task_get_tcb(dst_id);
    TEST_ASSERT(src != NULL && dst != NULL, "get src/dst TCBs");
    src->attrs = TASK_ATTR_USER;
    dst->attrs = TASK_ATTR_USER;

    int good_obj = 808;
    int bad_obj = 909;
    cap_id_t good = cap_create_for(src, &good_obj, CAP_OBJ_ENDPOINT,
                                   CAP_READ | CAP_TRANSFER);
    cap_id_t bad = cap_create_for(src, &bad_obj, CAP_OBJ_CHANNEL, CAP_READ);
    TEST_ASSERT(good != ((cap_id_t)-1), "create transferable cap");
    TEST_ASSERT(bad != ((cap_id_t)-1), "create non-transferable cap");

    ipc_cap_xfer_t xfers[2];
    xfers[0].src_cap = good;
    xfers[0].rights = CAP_READ;
    xfers[0].flags = IPC_CAP_COPY;
    xfers[1].src_cap = bad;
    xfers[1].rights = CAP_READ;
    xfers[1].flags = IPC_CAP_COPY;

    cap_id_t out[2] = { (cap_id_t)-1, (cap_id_t)-1 };
    kern_err_t err = ipc_transfer_caps(src, dst, xfers, 2, out);
    TEST_ASSERT_EQ((int)KERN_ERR_CAP, (int)err,
                   "transfer fails on non-transferable cap");
    TEST_ASSERT(out[0] == ((cap_id_t)-1), "failed transfer publishes no caps");

    void *ptr = cap_lookup_for(dst, out[0], CAP_OBJ_ENDPOINT, CAP_READ);
    TEST_ASSERT(ptr == NULL, "partial copied cap rolled back");

    ptr = cap_lookup_for(src, good, CAP_OBJ_ENDPOINT, CAP_READ);
    TEST_ASSERT(ptr == &good_obj, "source good cap still valid");

    ptr = cap_lookup_for(src, bad, CAP_OBJ_CHANNEL, CAP_READ);
    TEST_ASSERT(ptr == &bad_obj, "source bad cap still valid");

    (void)cap_revoke_for(src, good);
    (void)cap_revoke_for(src, bad);
    (void)task_delete(src_id);
    (void)task_delete(dst_id);
}

/*============================================================================
 * Test 24: IPC cap move rollback keeps source cap on later failure
 *============================================================================*/

static void test_ipc_cap_move_rollback(void) {
    test_section("Test 24: IPC cap move rollback");

    task_id_t src_id = task_create("ipcms", cap_test_task, NULL, 10, 512);
    task_id_t dst_id = task_create("ipcmd", cap_test_task, NULL, 10, 512);
    TEST_ASSERT(src_id >= 0 && dst_id >= 0, "create src/dst tasks");

    tcb_t *src = task_get_tcb(src_id);
    tcb_t *dst = task_get_tcb(dst_id);
    TEST_ASSERT(src != NULL && dst != NULL, "get src/dst TCBs");
    src->attrs = TASK_ATTR_USER;
    dst->attrs = TASK_ATTR_USER;

    int move_obj = 1001;
    int bad_obj = 1002;
    cap_id_t movable = cap_create_for(src, &move_obj, CAP_OBJ_ENDPOINT,
                                      CAP_READ | CAP_TRANSFER);
    cap_id_t bad = cap_create_for(src, &bad_obj, CAP_OBJ_CHANNEL, CAP_READ);
    TEST_ASSERT(movable != ((cap_id_t)-1), "create movable cap");
    TEST_ASSERT(bad != ((cap_id_t)-1), "create non-transferable cap");

    ipc_cap_xfer_t xfers[2];
    xfers[0].src_cap = movable;
    xfers[0].rights = CAP_READ;
    xfers[0].flags = IPC_CAP_MOVE;
    xfers[1].src_cap = bad;
    xfers[1].rights = CAP_READ;
    xfers[1].flags = IPC_CAP_COPY;

    cap_id_t out[2] = { (cap_id_t)-1, (cap_id_t)-1 };
    kern_err_t err = ipc_transfer_caps(src, dst, xfers, 2, out);
    TEST_ASSERT_EQ((int)KERN_ERR_CAP, (int)err,
                   "transfer fails after staged move");

    void *ptr = cap_lookup_for(src, movable, CAP_OBJ_ENDPOINT, CAP_READ);
    TEST_ASSERT(ptr == &move_obj, "failed move leaves source cap valid");

    ptr = cap_lookup_for(dst, out[0], CAP_OBJ_ENDPOINT, CAP_READ);
    TEST_ASSERT(ptr == NULL, "failed move leaves no dst cap");

    (void)cap_revoke_for(src, movable);
    (void)cap_revoke_for(src, bad);
    (void)task_delete(src_id);
    (void)task_delete(dst_id);
}

/*============================================================================
 * Test 25: object refcount tracks caps for the same kernel object
 *============================================================================*/

static void test_cap_object_refcount(void) {
    test_section("Test 25: cap object refcount");

    task_id_t src_id = task_create("caprefs", cap_test_task, NULL, 10, 512);
    task_id_t dst_id = task_create("caprefd", cap_test_task, NULL, 10, 512);
    TEST_ASSERT(src_id >= 0 && dst_id >= 0, "create refcount tasks");

    tcb_t *src = task_get_tcb(src_id);
    tcb_t *dst = task_get_tcb(dst_id);
    TEST_ASSERT(src != NULL && dst != NULL, "get refcount TCBs");
    src->attrs = TASK_ATTR_USER;
    dst->attrs = TASK_ATTR_USER;

    int obj = 1101;
    cap_id_t parent = cap_create_for(src, &obj, CAP_OBJ_ENDPOINT,
                                     CAP_READ | CAP_TRANSFER | CAP_GRANT);
    TEST_ASSERT(parent != ((cap_id_t)-1), "create parent cap");
    TEST_ASSERT_EQ(1, (int)cap_object_refcount(&obj, CAP_OBJ_ENDPOINT),
                   "one cap references object");

    cap_id_t derived = cap_derive_for(src, parent, CAP_READ);
    TEST_ASSERT(derived != ((cap_id_t)-1), "derive second cap");
    TEST_ASSERT_EQ(2, (int)cap_object_refcount(&obj, CAP_OBJ_ENDPOINT),
                   "derive increments object refs");

    cap_id_t copied = cap_copy_to(src, parent, dst, CAP_READ);
    TEST_ASSERT(copied != ((cap_id_t)-1), "copy third cap");
    TEST_ASSERT_EQ(3, (int)cap_object_refcount(&obj, CAP_OBJ_ENDPOINT),
                   "copy increments object refs");

    kern_err_t err = cap_revoke_for(src, derived);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "revoke derived cap");
    TEST_ASSERT_EQ(2, (int)cap_object_refcount(&obj, CAP_OBJ_ENDPOINT),
                   "revoke decrements object refs");

    err = cap_revoke_for(src, parent);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "revoke parent cap");
    TEST_ASSERT_EQ(0, (int)cap_object_refcount(&obj, CAP_OBJ_ENDPOINT),
                   "parent cascade clears all refs");

    (void)task_delete(src_id);
    (void)task_delete(dst_id);
}

/*============================================================================
 * Test 26: object cleanup callback fires when last cap is revoked
 *============================================================================*/

static volatile int cap_cleanup_count;

static void cap_test_cleanup(void *object, uint8_t obj_type) {
    (void)object;
    if (obj_type == CAP_OBJ_MEMBLOCK) {
        cap_cleanup_count++;
    }
}

static void test_cap_cleanup_callback(void) {
    test_section("Test 26: cap object cleanup callback");

    int obj = 1201;
    cap_cleanup_count = 0;
    kern_err_t err = cap_register_cleanup(CAP_OBJ_MEMBLOCK, cap_test_cleanup);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "register cleanup callback");

    cap_id_t parent = cap_create(&obj, CAP_OBJ_MEMBLOCK,
                                 CAP_READ | CAP_GRANT, 1);
    TEST_ASSERT(parent != ((cap_id_t)-1), "create cleanup parent");
    cap_id_t child = cap_derive(parent, CAP_READ);
    TEST_ASSERT(child != ((cap_id_t)-1), "derive cleanup child");

    err = cap_revoke(child);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "revoke child");
    TEST_ASSERT_EQ(0, cap_cleanup_count, "cleanup not called while parent remains");

    err = cap_revoke(parent);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "revoke last cap");
    TEST_ASSERT_EQ(1, cap_cleanup_count, "cleanup called at last ref");

    (void)cap_register_cleanup(CAP_OBJ_MEMBLOCK, NULL);
}

/*============================================================================
 * Test 27: MMIO cap lifecycle
 *============================================================================*/

static void test_mmio_cap_lifecycle(void) {
    test_section("Test 27: MMIO cap lifecycle");

#if MEM_DYNAMIC
    uint32_t outstanding = mem_get_outstanding_allocs();
    cap_id_t cap = KERN_INVALID_ID;
    kern_err_t err = kmmio_create_cap(0x40000000UL, 16, 4,
                                      CAP_READ | CAP_WRITE | CAP_MANAGE,
                                      &cap);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "kmmio_create_cap OK");
    TEST_ASSERT(cap >= 0, "kmmio_create_cap returns cap");
    TEST_ASSERT_EQ((int)outstanding,
                   (int)mem_get_outstanding_allocs(),
                   "kmmio static metadata keeps heap unchanged");

    uintptr_t base = 0;
    size_t size = 0;
    uint8_t width = 0;
    err = kmmio_get_bounds(cap, &base, &size, &width);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "kmmio_get_bounds OK");
    TEST_ASSERT_EQ((int)0x40000000UL, (int)base, "kmmio base recorded");
    TEST_ASSERT_EQ(16, (int)size, "kmmio size recorded");
    TEST_ASSERT_EQ(4, (int)width, "kmmio width recorded");

    err = kmmio_delete_cap(cap);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "kmmio_delete_cap OK");
    err = kmmio_get_bounds(cap, &base, &size, &width);
    TEST_ASSERT_EQ((int)KERN_ERR_CAP, (int)err,
                   "deleted kmmio cap no longer resolves");
    TEST_ASSERT_EQ((int)outstanding,
                   (int)mem_get_outstanding_allocs(),
                   "kmmio delete keeps heap unchanged");
#else
    test_skip("dynamic memory disabled");
#endif
}

/*============================================================================
 * Module registration
 *============================================================================*/

static void test_capability_module(void) {
    test_cap_create_basic();
    test_cap_create_pool_full();
    test_cap_resolve_valid();
    test_cap_resolve_bad_token();
    test_cap_resolve_wrong_type();
    test_cap_resolve_no_rights();
    test_cap_delete_resolve();
    test_cap_derive_subset();
    test_cap_derive_superset_fails();
    test_cap_derive_no_grant();
    test_cap_revoke_all();
    test_cap_revoke_single();
    test_cap_create_null();
    test_cap_resolve_invalid();
    test_cap_id0_roundtrip();
    test_cap_no_permission();
    test_cap_stale_generation();
    test_cap_revoke_cascade();
    test_cap_delete_preserves_children();
    test_cap_cspace_required_for_user();
    test_cap_cspace_install_and_revoke();
    test_cap_copy_to_task();
    test_cap_move_to_task();
    test_ipc_cap_transfer_rollback();
    test_ipc_cap_move_rollback();
    test_cap_object_refcount();
    test_cap_cleanup_callback();
    test_mmio_cap_lifecycle();
}

TEST_MODULE_REGISTER(capability, test_capability_module);

#endif /* CAP_ENABLE && TEST_MODULE_CAP */
