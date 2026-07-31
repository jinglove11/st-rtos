/**
 * @file test_capability.c
 * @brief 能力系统测试 — 令牌生命周期 + 权限校验
 */

#include "test_framework.h"
#include "kernel.h"
#include "ipc_transfer.h"
#include "mem.h"
#include "semaphore.h"   /* M2-Step3a: sem_create/sem_obj_for_cap */
#include "spinlock.h"
#include <string.h>
#if CAP_RESTART_SUBSET
#include "cap_subset.h"
#endif

#if CAP_ENABLE && TEST_MODULE_CAP

static void cap_test_task(void *arg) {
    (void)arg;
}

static uint64_t test_cspace_occupied(tcb_t *task) {
    cnode_t *cnode = cap_space_of(task);
    return cnode != NULL ? cnode->occupied : 0;
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
 * Test 17a: 32-bit generation boundary retires a slot instead of wrapping
 *============================================================================*/

static void test_cap_generation_exhaustion_retires_slot(void) {
    test_section("Test 17a: generation exhaustion retires slot");

    int obj = 0x32434150;
    uint16_t free_before = cap_free_count();
    cap_id_t original = cap_create(&obj, CAP_OBJ_SYSTEM, CAP_FULL, 1);
    TEST_ASSERT(original != KERN_INVALID_ID, "create cap for generation boundary");
    if (original == KERN_INVALID_ID) {
        return;
    }

    cap_id_t boundary = KERN_INVALID_ID;
    uint16_t retired_slot = UINT16_MAX;
    uint32_t limit = cap_test_generation_limit();
    kern_err_t err = cap_test_force_generation(original, limit, &boundary,
                                                &retired_slot);
    TEST_ASSERT_EQ(KERN_OK, err, "force cap generation to positive-handle limit");
    if (err != KERN_OK) {
        cap_delete(original);
        return;
    }

    TEST_ASSERT(sizeof(cap_id_t) == sizeof(int32_t), "cap handle ABI is 32-bit");
    TEST_ASSERT(boundary > INT16_MAX, "cap handle retains generation above 16-bit ABI");
    TEST_ASSERT(cap_resolve(original, CAP_OBJ_SYSTEM, CAP_READ) == NULL,
                "pre-boundary handle becomes stale");
    TEST_ASSERT(cap_resolve(boundary, CAP_OBJ_SYSTEM, CAP_READ) == &obj,
                "boundary generation handle resolves");

    cap_delete(boundary);
    TEST_ASSERT(cap_resolve(boundary, CAP_OBJ_SYSTEM, CAP_READ) == NULL,
                "retired handle no longer resolves");
    TEST_ASSERT_EQ((int)free_before - 1, (int)cap_free_count(),
                   "exhausted slot is not returned to free pool");

    cap_id_t replacement = cap_create(&obj, CAP_OBJ_SYSTEM, CAP_FULL, 1);
    TEST_ASSERT(replacement != KERN_INVALID_ID,
                "another slot remains allocatable after retirement");
    if (replacement != KERN_INVALID_ID) {
        uint16_t replacement_slot = UINT16_MAX;
        uint32_t replacement_generation = 0;
        err = cap_test_handle_info(replacement, &replacement_slot,
                                   &replacement_generation);
        TEST_ASSERT_EQ(KERN_OK, err, "decode replacement handle");
        TEST_ASSERT(replacement_slot != retired_slot,
                    "allocator never reuses retired slot");
        TEST_ASSERT(replacement_generation > 0 &&
                    replacement_generation <= limit,
                    "replacement slot has a valid non-retired generation");
        cap_delete(replacement);
    }

    /* Restore the artificial retirement so the module's resource audit still
     * checks the real implementation for leaks rather than this test fixture. */
    err = cap_test_reset_retired_slot(retired_slot);
    TEST_ASSERT_EQ(KERN_OK, err, "test-only retired slot reset succeeds");
    TEST_ASSERT_EQ((int)free_before, (int)cap_free_count(),
                   "generation boundary test restores capability pool");
}

static void test_object_generation_exhaustion_retires_slot(void) {
    test_section("Test 17aa: object generation exhaustion retires slot");

    sem_id_t retired_id = sem_create(0, 1);
    TEST_ASSERT(retired_id != KERN_INVALID_ID,
                "create semaphore for object generation boundary");
    if (retired_id == KERN_INVALID_ID) {
        return;
    }

    kobject_header_t *retired_hdr =
        (kobject_header_t *)sem_obj_for_cap(retired_id);
    retired_hdr->generation = KOBJ_GENERATION_MAX;
    kern_err_t err = sem_delete(retired_id);
    TEST_ASSERT_EQ(KERN_OK, err, "delete object at final valid generation");
    TEST_ASSERT(kobj_generation_is_retired(retired_hdr->generation),
                "object generation advances to retired sentinel");

    cap_id_t impossible = cap_create_for_gen(NULL, retired_hdr,
                                             CAP_OBJ_SEMAPHORE, CAP_FULL,
                                             retired_hdr->generation);
    TEST_ASSERT(impossible == KERN_INVALID_ID,
                "retired object cannot receive a capability");

    sem_id_t replacement_id = sem_create(0, 1);
    TEST_ASSERT(replacement_id != KERN_INVALID_ID,
                "another object slot remains allocatable");
    TEST_ASSERT(replacement_id != retired_id,
                "object allocator skips retired slot");
    if (replacement_id != KERN_INVALID_ID) {
        (void)sem_delete(replacement_id);
    }

    /* Undo the artificial boundary, then prove the allocator can see the
     * slot again.  Production code has no path that clears this sentinel. */
    retired_hdr->generation = KOBJ_GENERATION_INITIAL;
    sem_id_t restored_id = sem_create(0, 1);
    TEST_ASSERT_EQ((int)retired_id, (int)restored_id,
                   "test fixture restores retired object slot");
    if (restored_id != KERN_INVALID_ID) {
        (void)sem_delete(restored_id);
    }
}

/*============================================================================
 * Test 17b: M2-Step3a — 对象 slot 复用后旧 cap 失效 (stale-cap-on-reuse)
 *
 * 这是 M2 验收 #1: "旧 task cap 在 task id 复用后必须返回 stale-cap 错误"
 * 在 sem 对象上的具体化。test_cap_stale_generation 测的是 cap slot
 * generation (cap_id 复用),本测试测的是对象 generation (sem_id 复用)。
 *
 * 场景:
 *   1. 创建 sem_A → cap_A (cap.object = &sem_pool[id], obj_generation = 1)
 *   2. 删除 sem_A → cap_A 被 cap_revoke_object 撤销, sem_pool[id].generation = 2
 *   3. 创建 sem_B (slot 复用同一 id) → cap_B (obj_generation = 2)
 *   4. 即使我们手工把 cap_A 重新插入 cap_pool (绕过 revoke),它也无效 ——
 *      cap_get_entry cross-check 检测到 obj_generation(1) != hdr.generation(2)
 *============================================================================*/
static void test_cap_object_stale_on_reuse(void) {
    test_section("Test 17b: object slot reuse invalidates stale cap (M2-Step3a)");

#if CAP_ENABLE
    /* 用 sem 池的第一个 slot,便于控制复用。
     * 注意:测试运行时其他模块可能占用 sem 池,所以不能假定 id=0;
     * 改用"删除后再创建必复用同一 slot"的不变量。 */
    sem_id_t sem_a = sem_create(0, 1);
    TEST_ASSERT(sem_a >= 0, "M2-Step3a: sem A created");
    if (sem_a < 0) return;

    /* 通过 cap_create_for_gen 直接拿带 obj_generation 的 cap,模拟 sys_sem_create */
    void *obj_a = sem_obj_for_cap(sem_a);
    TEST_ASSERT(obj_a != NULL, "M2-Step3a: sem A obj pointer non-null");
    uint32_t gen_a = ((const kobject_header_t *)obj_a)->generation;
    TEST_ASSERT_EQ(1, (int)gen_a, "M2-Step3a: sem A generation = 1 (first alloc)");

    cap_id_t cap_a = cap_create_for_gen(NULL, obj_a, CAP_OBJ_SEMAPHORE,
                                        CAP_FULL, gen_a);
    TEST_ASSERT(cap_a != ((cap_id_t)-1), "M2-Step3a: cap A created with obj_generation");

    /* cap_a 应该能正常 resolve */
    void *resolved = cap_resolve(cap_a, CAP_OBJ_SEMAPHORE, CAP_READ);
    TEST_ASSERT(resolved == obj_a, "M2-Step3a: cap A resolves to sem A");

    /* 删除 sem A: cap_revoke_object 撤销 cap_a, sem.hdr.generation bump 到 2 */
    kern_err_t err = sem_delete(sem_a);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "M2-Step3a: sem A deleted");

    /* cap_a 已被 cap_revoke_object 撤销,cap_resolve 直接返回 NULL */
    resolved = cap_resolve(cap_a, CAP_OBJ_SEMAPHORE, CAP_READ);
    TEST_ASSERT(resolved == NULL, "M2-Step3a: cap A revoked after sem A delete");

    /* 创建 sem B: 必复用同一 slot (sem_a), generation = 2 */
    sem_id_t sem_b = sem_create(0, 1);
    TEST_ASSERT(sem_b >= 0, "M2-Step3a: sem B created");
    /* 验证 slot 复用 */
    TEST_ASSERT_EQ((int)sem_a, (int)sem_b,
                   "M2-Step3a: sem B reuses sem A slot (id identical)");

    void *obj_b = sem_obj_for_cap(sem_b);
    uint32_t gen_b = ((const kobject_header_t *)obj_b)->generation;
    TEST_ASSERT_EQ(2, (int)gen_b,
                   "M2-Step3a: sem B generation = 2 (after bump on delete)");

    /* 模拟"漏撤"场景:手工造一个用旧 gen_a 的 cap (跟 cap_a 同 object 指针,
     * 但 obj_generation=1)。即使 cap slot 是新的,cap_get_entry cross-check
     * 必须拒绝 (obj_generation 1 != hdr.generation 2)。
     * 这就是 M2 验收 #1 防御的攻击面:cap_revoke_object 跑过一次后,攻击者
     * 拿到旧 cap 副本 (比如 derived child 的拷贝) 仍不能越权访问新对象。 */
    cap_id_t stale_cap = cap_create_for_gen(NULL, obj_b, CAP_OBJ_SEMAPHORE,
                                            CAP_FULL, gen_a);
    TEST_ASSERT(stale_cap != ((cap_id_t)-1),
                "M2-Step3a: stale_cap created (simulating missed revoke)");
    void *stale_resolved = cap_resolve(stale_cap, CAP_OBJ_SEMAPHORE, CAP_READ);
    TEST_ASSERT(stale_resolved == NULL,
                "M2-Step3a: stale cap (obj_generation=1) rejected on gen-2 object");

    /* 对照:正确 obj_generation 的 cap 能 resolve */
    cap_id_t fresh_cap = cap_create_for_gen(NULL, obj_b, CAP_OBJ_SEMAPHORE,
                                            CAP_FULL, gen_b);
    TEST_ASSERT(fresh_cap != ((cap_id_t)-1), "M2-Step3a: fresh cap created");
    void *fresh_resolved = cap_resolve(fresh_cap, CAP_OBJ_SEMAPHORE, CAP_READ);
    TEST_ASSERT(fresh_resolved == obj_b,
                "M2-Step3a: fresh cap (obj_generation=2) resolves to sem B");

    /* cleanup */
    cap_delete(fresh_cap);
    /* stale_cap 仍可 cap_delete (它本身 cap slot 有效,只是 cross-check 拒绝) */
    cap_delete(stale_cap);
    sem_delete(sem_b);
#else
    test_skip("CAP_ENABLE off");
#endif
}

/*============================================================================
 * Test 17c: M2-#7 — mint 派生 cap 衰减 rights + 设 badge
 *
 * mint = derive + badge。验证:
 * - child rights ⊆ parent rights
 * - child.badge = 调用方指定值
 * - child 共享 parent 的 object (不创建新对象)
 * - superset rights 被拒绝
 *============================================================================*/
static void test_cap_mint_badge(void) {
    test_section("Test 17c: mint with rights subset + badge (M2-#7)");

#if CAP_ENABLE
    int obj = 0xAB;
    cap_id_t parent = cap_create(&obj, CAP_OBJ_ENDPOINT,
                                 CAP_READ | CAP_WRITE | CAP_GRANT, 1);
    TEST_ASSERT(parent != ((cap_id_t)-1), "M2-#7: parent cap created");

    /* mint: 衰减到 CAP_READ,设 badge=0x1234 */
    cap_id_t child = cap_mint_for(NULL, parent, CAP_READ, 0x1234U);
    TEST_ASSERT(child != ((cap_id_t)-1), "M2-#7: mint child created");
    TEST_ASSERT(child != parent, "M2-#7: mint child is distinct cap");

    /* rights 应该是 CAP_READ (衰减后) */
    uint8_t child_rights = 0;
    TEST_ASSERT_EQ((int)KERN_OK,
                   (int)cap_get_rights(child, &child_rights),
                   "M2-#7: child rights queryable");
    TEST_ASSERT_EQ((int)CAP_READ, (int)child_rights,
                   "M2-#7: child rights = CAP_READ (subset)");

    /* badge 应该是 0x1234 */
    TEST_ASSERT_EQ(0x1234U, cap_get_badge(child),
                   "M2-#7: child badge = 0x1234");

    /* child 应该能 resolve 到同一 object */
    void *p1 = cap_resolve(parent, CAP_OBJ_ENDPOINT, CAP_READ);
    void *p2 = cap_resolve(child, CAP_OBJ_ENDPOINT, CAP_READ);
    TEST_ASSERT(p1 == p2 && p1 == &obj,
                "M2-#7: child shares parent object");

    /* parent badge 应该是 0 (mint 不影响 parent) */
    TEST_ASSERT_EQ(0U, cap_get_badge(parent),
                   "M2-#7: parent badge stays 0");

    /* superset rights 被拒绝 */
    cap_id_t bad = cap_mint_for(NULL, parent,
                                CAP_READ | CAP_MANAGE, 0U);
    TEST_ASSERT(bad == ((cap_id_t)-1),
                "M2-#7: mint rejects superset rights");

    /* 没 CAP_GRANT 的 parent 不能 mint */
    cap_id_t no_grant = cap_create(&obj, CAP_OBJ_ENDPOINT, CAP_READ, 1);
    cap_id_t bad2 = cap_mint_for(NULL, no_grant, CAP_READ, 0U);
    TEST_ASSERT(bad2 == ((cap_id_t)-1),
                "M2-#7: mint rejected without CAP_GRANT");

    cap_delete(child);
    cap_delete(parent);
    cap_delete(no_grant);
#else
    test_skip("CAP_ENABLE off");
#endif
}

/*============================================================================
 * Test 17d: M2 验收 A1 — 旧 task cap 在 task id 复用后必须 stale
 *
 * 这是验收 #1 的字面条目:task 而非 sem。
 * 流程: task_A 拿 task_cap_A → task_delete(A) → task_B 复用 A 的 id →
 *       旧 task_cap_A 必须失效 (cap_resolve 返回 NULL)。
 *============================================================================*/
static void cap_test_task_A1(void *arg) { (void)arg; }

static void test_cap_task_id_reuse_stale(void) {
    test_section("Test 17d: task id reuse invalidates stale task cap (A1)");

#if CAP_ENABLE
    task_id_t a = task_create("A1_a", cap_test_task_A1, NULL, 10, 512);
    TEST_ASSERT(a >= 0, "A1: task A created");
    if (a < 0) return;

    /* 取 task A 的真实 generation cap (跟 sys_task_create 一致) */
    void *obj_a = task_obj_for_cap(a);
    uint32_t gen_a = ((const kobject_header_t *)obj_a)->generation;
    cap_id_t cap_a = cap_create_for_gen(NULL, obj_a, CAP_OBJ_TASK, CAP_FULL, gen_a);
    TEST_ASSERT(cap_a > 0, "A1: task cap A created");

    TEST_ASSERT(cap_resolve(cap_a, CAP_OBJ_TASK, CAP_MANAGE) == obj_a,
                "A1: task cap A resolves initially");

    /* 删除 task A: cap_revoke_object 撤销 cap_a + hdr.generation bump */
    kern_err_t err = task_delete(a);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "A1: task A deleted");

    /* cap_a 已被 revoke,resolve 失败 */
    TEST_ASSERT(cap_resolve(cap_a, CAP_OBJ_TASK, CAP_MANAGE) == NULL,
                "A1: task cap A revoked after delete");

    /* 创建 task B: 必须复用 A 的 id (task_pool 按 bitmap first-fit) */
    task_id_t b = task_create("A1_b", cap_test_task_A1, NULL, 10, 512);
    TEST_ASSERT(b >= 0, "A1: task B created");
    TEST_ASSERT_EQ((int)a, (int)b,
                   "A1: task B reuses task A's id (slot recycle)");

    void *obj_b = task_obj_for_cap(b);
    uint32_t gen_b = ((const kobject_header_t *)obj_b)->generation;
    TEST_ASSERT(gen_b != gen_a, "A1: task B has bumped generation");

    /* 模拟"漏撤":构造一个旧 gen_a 的 cap 指向 obj_b。
     * 即使 cap slot 有效,cross-check 必须拒绝 (obj_gen 不匹配)。 */
    cap_id_t stale = cap_create_for_gen(NULL, obj_b, CAP_OBJ_TASK, CAP_FULL, gen_a);
    TEST_ASSERT(stale > 0, "A1: stale cap (old gen) created");
    TEST_ASSERT(cap_resolve(stale, CAP_OBJ_TASK, CAP_MANAGE) == NULL,
                "A1: stale task cap (old generation) rejected on reused id");

    /* 对照:新 gen 的 cap 正常 resolve */
    cap_id_t fresh = cap_create_for_gen(NULL, obj_b, CAP_OBJ_TASK, CAP_FULL, gen_b);
    TEST_ASSERT(fresh > 0, "A1: fresh cap (new gen) created");
    TEST_ASSERT(cap_resolve(fresh, CAP_OBJ_TASK, CAP_MANAGE) == obj_b,
                "A1: fresh task cap resolves to task B");

    cap_delete(fresh);
    cap_delete(stale);
    (void)task_delete(b);
#else
    test_skip("CAP_ENABLE off");
#endif
}

/*============================================================================
 * Test 17d: M2-#8 — copy/move 事务原子性 (CSpace 满时回滚)
 *
 * 验收 #2: "CSpace 满时 cap transfer 原子失败,源和目标均无半提交状态"。
 * 场景: dst task 的 CSpace 填满,再 cap_copy_to 应失败,且:
 *   - dst 没多出 cap (cap_set 未被污染)
 *   - src 的原 cap 仍可用 (未被删/移)
 *   - cap_pool 没泄漏 slot (失败的 child slot 被回滚)
 *============================================================================*/
static void test_cap_copy_atomic_on_full(void) {
    test_section("Test 17d: copy atomic on CSpace full (M2-#8)");

#if CAP_ENABLE
    /* src task + 源 cap */
    task_id_t src_id = task_create("capsrc", cap_test_task, NULL, 10, 512);
    task_id_t dst_id = task_create("capdst", cap_test_task, NULL, 10, 512);
    TEST_ASSERT(src_id >= 0 && dst_id >= 0, "M2-#8: src/dst tasks created");
    if (src_id < 0 || dst_id < 0) return;

    tcb_t *src = task_get_tcb(src_id);
    tcb_t *dst = task_get_tcb(dst_id);
    src->attrs = TASK_ATTR_USER;
    dst->attrs = TASK_ATTR_USER;

    int obj = 0x77;
    cap_id_t src_cap = cap_create_for(src, &obj, CAP_OBJ_ENDPOINT,
                                      CAP_READ | CAP_WRITE | CAP_TRANSFER | CAP_GRANT);
    TEST_ASSERT(src_cap > 0, "M2-#8: src cap created");
    cap_id_t dst_cnode = cap_cnode_cap_create(src, dst, CAP_WRITE);
    TEST_ASSERT(dst_cnode > 0, "M2-#8: destination CNode authority created");

    /* 填满 dst 的 CSpace */
    cap_id_t filler[KERN_TASK_CAP_SLOTS];
    int filled = 0;
    for (int i = 0; i < KERN_TASK_CAP_SLOTS; i++) {
        filler[i] = cap_create_for(dst, &obj, CAP_OBJ_ENDPOINT, CAP_READ);
        if (filler[i] < 0) break;
        filled++;
    }
    TEST_ASSERT_EQ(KERN_TASK_CAP_SLOTS, filled,
                   "M2-#8: dst CSpace filled to capacity");

    uint16_t free_before = cap_free_count();

    /* 试图 copy 到满的 dst — 应失败 */
    cap_id_t bad = cap_copy_to(src, src_cap, dst, CAP_READ);
    TEST_ASSERT(bad == ((cap_id_t)-1),
                "M2-#8: cap_copy_to fails when dst CSpace full");
    cap_id_t bad_cnode = cap_cnode_copy(src, src_cap, dst_cnode, CAP_READ);
    TEST_ASSERT(bad_cnode < 0,
                "M2-#8: CNode Copy fails when destination is full");
    cap_id_t moved = KERN_INVALID_ID;
    kern_err_t move_err = cap_cnode_move(src, src_cap, dst_cnode, &moved);
    TEST_ASSERT_EQ((int)KERN_ERR_RESOURCE, (int)move_err,
                   "M2-#8: CNode Move fails atomically when target is full");

    /* 验证原子性:
     * - cap_pool 没泄漏 (失败的 child slot 被回滚)
     * - src_cap 仍可用 (未被移除)
     * - dst 没多出 cap (仍是 KERN_TASK_CAP_SLOTS 个) */
    TEST_ASSERT_EQ((int)free_before, (int)cap_free_count(),
                   "M2-#8: no cap_pool slot leaked on failed copy");

    void *p = cap_lookup_for(src, src_cap, CAP_OBJ_ENDPOINT, CAP_READ);
    TEST_ASSERT(p == &obj,
                "M2-#8: src cap still resolves after failed copy");

    /* cleanup */
    cap_delete(dst_cnode);
    cap_delete(src_cap);
    for (int i = 0; i < filled; i++) {
        cap_delete(filler[i]);
    }
    (void)task_delete(src_id);
    (void)task_delete(dst_id);
#else
    test_skip("CAP_ENABLE off");
#endif
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
    TEST_ASSERT(test_cspace_occupied(tcb) != 0,
                "cap installed in task CSpace");

    void *ptr = cap_lookup_for(tcb, cap, CAP_OBJ_CHANNEL, CAP_READ);
    TEST_ASSERT(ptr == &obj, "installed user cap resolves");

    kern_err_t err = cap_revoke_for(tcb, cap);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "revoke installed cap OK");
    TEST_ASSERT(test_cspace_occupied(tcb) == 0,
                "revoke clears task CSpace slot");

    ptr = cap_lookup_for(tcb, cap, CAP_OBJ_CHANNEL, CAP_READ);
    TEST_ASSERT(ptr == NULL, "revoked user cap no longer resolves");

    (void)task_delete(tid);
}

/*============================================================================
 * Test 21: user CSpace holds more than the legacy 16 slots
 *============================================================================*/

static void test_cap_cspace_extended_slots(void) {
    test_section("Test 21: extended user CSpace slots");

    task_id_t tid = task_create("capcs32", cap_test_task, NULL, 10, 512);
    TEST_ASSERT(tid >= 0, "create task for extended CSpace test");

    tcb_t *tcb = task_get_tcb(tid);
    TEST_ASSERT(tcb != NULL, "get extended CSpace task TCB");
    if (tcb == NULL) {
        return;
    }
    tcb->attrs = TASK_ATTR_USER;

    int objects[20];
    cap_id_t caps[20];
    for (uint32_t i = 0; i < 20U; i++) {
        objects[i] = 600 + (int)i;
        caps[i] = cap_create_for(tcb, &objects[i],
                                 CAP_OBJ_CHANNEL, CAP_READ);
        TEST_ASSERT(caps[i] >= 0, "extended CSpace cap created");
        void *ptr = cap_lookup_for(tcb, caps[i], CAP_OBJ_CHANNEL, CAP_READ);
        TEST_ASSERT(ptr == &objects[i], "extended CSpace cap resolves");
    }

    TEST_ASSERT((test_cspace_occupied(tcb) & (UINT64_C(1) << 16)) != 0,
                "extended CSpace uses slot 16");
    TEST_ASSERT((test_cspace_occupied(tcb) & (UINT64_C(1) << 19)) != 0,
                "extended CSpace uses slot 19");

    for (uint32_t i = 0; i < 20U; i++) {
        kern_err_t err = cap_revoke_for(tcb, caps[i]);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "extended CSpace cap revoked");
    }
    TEST_ASSERT_EQ(0, (int)test_cspace_occupied(tcb),
                   "extended CSpace fully cleared");

    (void)task_delete(tid);
}

/*============================================================================
 * Test 21b: M2-#4 — CSpace 扩容到 64 slot,验证 >32 的 slot 可用
 *
 * 历史 CSpace 是 32 slot (uint32 bitmap)。M2-#4 扩到 64 (uint64 bitmap)。
 * 本测试填充 48 个 slot (跨过旧的 32 边界),验证 slot 33-47 可用,
 * 且 CNode occupied 位图的高 32 位 (bit 32-63) 正确置位。
 *============================================================================*/
static void test_cap_cspace_over_32_slots(void) {
    test_section("Test 21b: CSpace slots beyond 32 (M2-#4 64-slot)");

    /* KERN_TASK_CAP_SLOTS 必须 >= 48 才能跑这个测试 */
    if (KERN_TASK_CAP_SLOTS < 48) {
        test_skip("KERN_TASK_CAP_SLOTS < 48");
        return;
    }

    task_id_t tid = task_create("capcs48", cap_test_task, NULL, 10, 512);
    TEST_ASSERT(tid >= 0, "create task for >32 slot test");
    tcb_t *tcb = task_get_tcb(tid);
    TEST_ASSERT(tcb != NULL, "get >32 slot task TCB");
    if (tcb == NULL) {
        (void)task_delete(tid);
        return;
    }
    /* cap_task_add 只对 USER 任务登记 CNode;手工设 attrs 走完整路径。 */
    tcb->attrs = TASK_ATTR_USER;

    /* 填充 48 个 slot (绕过 32 边界)。cap_pool 上限是 CAP_MAX_COUNT=128,够。 */
    int dummy_obj = 0;
    cap_id_t caps[48];
    int filled = 0;
    for (int i = 0; i < 48; i++) {
        caps[i] = cap_create_for(tcb, &dummy_obj, CAP_OBJ_SEMAPHORE, CAP_READ);
        if (caps[i] == ((cap_id_t)-1)) {
            break;
        }
        filled++;
    }
    TEST_ASSERT_EQ(48, filled, "M2-#4: filled 48 slots (cross 32 boundary)");

    /* 验证高 32 位被置位 (bit 32-47)。occupied 是 uint64,高 32 位
     * 应该有 (1<<0)|(1<<1)|...|(1<<15) = 0xFFFF 在高 word。 */
    TEST_ASSERT((test_cspace_occupied(tcb) >> 32) != 0,
                "M2-#4: high 32 bits of CNode bitmap set");

    /* slot 40 应该被填充 */
    TEST_ASSERT((test_cspace_occupied(tcb) & (UINT64_C(1) << 40)) != 0,
                "M2-#4: slot 40 populated");

    /* cap_resolve 应能 resolve slot 40 的 cap */
    void *ptr = cap_lookup_for(tcb, caps[40], CAP_OBJ_SEMAPHORE, CAP_READ);
    TEST_ASSERT(ptr == &dummy_obj, "M2-#4: slot 40 cap resolves");

    /* cleanup */
    for (int i = 0; i < filled; i++) {
        cap_delete(caps[i]);
    }
    TEST_ASSERT_EQ(0, (int)test_cspace_occupied(tcb),
                   "M2-#4: all slots cleared after delete");

    (void)task_delete(tid);
}

/*============================================================================
 * Test 22: revoke all caps for one object without touching another object
 *============================================================================*/

static void test_cap_revoke_object(void) {
    test_section("Test 22: cap_revoke_object");

    task_id_t a_id = task_create("capobj_a", cap_test_task, NULL, 10, 512);
    task_id_t b_id = task_create("capobj_b", cap_test_task, NULL, 10, 512);
    TEST_ASSERT(a_id >= 0 && b_id >= 0, "create object revoke tasks");

    tcb_t *a = task_get_tcb(a_id);
    tcb_t *b = task_get_tcb(b_id);
    TEST_ASSERT(a != NULL && b != NULL, "get object revoke TCBs");
    if (a == NULL || b == NULL) {
        return;
    }
    a->attrs = TASK_ATTR_USER;
    b->attrs = TASK_ATTR_USER;

    int obj = 700;
    int other = 701;
    cap_id_t root = cap_create_for(a, &obj, CAP_OBJ_ENDPOINT,
                                   CAP_FULL);
    cap_id_t copy = cap_copy_to(a, root, b, CAP_READ);
    cap_id_t direct = cap_create_for(b, &obj, CAP_OBJ_ENDPOINT,
                                     CAP_READ | CAP_WRITE);
    cap_id_t other_cap = cap_create_for(a, &other, CAP_OBJ_ENDPOINT,
                                        CAP_READ);
    TEST_ASSERT(root >= 0, "object revoke root cap created");
    TEST_ASSERT(copy >= 0, "object revoke copied cap created");
    TEST_ASSERT(direct >= 0, "object revoke direct cap created");
    TEST_ASSERT(other_cap >= 0, "object revoke other cap created");
    TEST_ASSERT_EQ(3, (int)cap_object_refcount(&obj, CAP_OBJ_ENDPOINT),
                   "object revoke sees all refs");

    kern_err_t err = cap_revoke_object(&obj, CAP_OBJ_ENDPOINT);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "cap_revoke_object OK");
    TEST_ASSERT_EQ(0, (int)cap_object_refcount(&obj, CAP_OBJ_ENDPOINT),
                   "object revoke clears all refs");

    void *ptr = cap_lookup_for(a, root, CAP_OBJ_ENDPOINT, CAP_READ);
    TEST_ASSERT_NULL(ptr, "object revoke clears owner root cap");
    ptr = cap_lookup_for(b, copy, CAP_OBJ_ENDPOINT, CAP_READ);
    TEST_ASSERT_NULL(ptr, "object revoke clears copied child cap");
    ptr = cap_lookup_for(b, direct, CAP_OBJ_ENDPOINT, CAP_READ);
    TEST_ASSERT_NULL(ptr, "object revoke clears direct cap");
    ptr = cap_lookup_for(a, other_cap, CAP_OBJ_ENDPOINT, CAP_READ);
    TEST_ASSERT_EQ((uintptr_t)&other, (uintptr_t)ptr,
                   "object revoke preserves other object cap");

    cap_delete(other_cap);
    (void)task_delete(a_id);
    (void)task_delete(b_id);
}

/*============================================================================
 * Test 23: cap_copy_to installs a reduced child cap into another CSpace
 *============================================================================*/

static void test_cap_copy_to_task(void) {
    test_section("Test 23: cap_copy_to task");

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
    TEST_ASSERT(moved != cap && cap_is_local_cptr(moved),
                "move returns destination-local CPtr");

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
 * Test 24b: IPC cap MOVE 成功后 dst cap 仍有效 (Phase H3 bug 修复验证)
 *===========================================================================*/

static void test_ipc_cap_move_success(void) {
    test_section("Test 24b: IPC cap move success (dst keeps cap)");

    task_id_t src_id = task_create("ipcms2", cap_test_task, NULL, 10, 512);
    task_id_t dst_id = task_create("ipcmd2", cap_test_task, NULL, 10, 512);
    TEST_ASSERT(src_id >= 0 && dst_id >= 0, "create src/dst tasks");

    tcb_t *src = task_get_tcb(src_id);
    tcb_t *dst = task_get_tcb(dst_id);
    TEST_ASSERT(src != NULL && dst != NULL, "get src/dst TCBs");
    src->attrs = TASK_ATTR_USER;
    dst->attrs = TASK_ATTR_USER;

    int move_obj = 2001;
    cap_id_t movable = cap_create_for(src, &move_obj, CAP_OBJ_ENDPOINT,
                                      CAP_READ | CAP_TRANSFER);
    TEST_ASSERT(movable != ((cap_id_t)-1), "create movable cap");

    ipc_cap_xfer_t xfers[1];
    xfers[0].src_cap = movable;
    xfers[0].rights = CAP_READ;
    xfers[0].flags = IPC_CAP_MOVE;

    cap_id_t out[1] = { (cap_id_t)-1 };
    kern_err_t err = ipc_transfer_caps(src, dst, xfers, 1, out);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "ipc move succeeds");

    /* Phase H3 核心:dst 收到的 cap 必须仍然有效 (之前 bug 导致立即失效) */
    void *ptr = cap_lookup_for(dst, out[0], CAP_OBJ_ENDPOINT, CAP_READ);
    TEST_ASSERT(ptr == &move_obj, "dst cap valid after MOVE (H3 fix)");

    /* src 不再持有 (MOVE 转移了所有权) */
    ptr = cap_lookup_for(src, movable, CAP_OBJ_ENDPOINT, CAP_READ);
    TEST_ASSERT(ptr == NULL, "src lost cap after MOVE");

    (void)cap_revoke_for(dst, out[0]);
    (void)task_delete(src_id);
    (void)task_delete(dst_id);
}

/*============================================================================
 * Test 24c: explicit prepare/commit transaction publishes one atomic batch
 *============================================================================*/

static void test_cap_explicit_transaction(void) {
    test_section("Test 24c: explicit cap transaction commit");

    task_id_t src_id = task_create("captxs", cap_test_task, NULL, 10, 512);
    task_id_t dst_id = task_create("captxd", cap_test_task, NULL, 10, 512);
    TEST_ASSERT(src_id >= 0 && dst_id >= 0, "transaction tasks created");
    if (src_id < 0 || dst_id < 0) {
        return;
    }

    tcb_t *src = task_get_tcb(src_id);
    tcb_t *dst = task_get_tcb(dst_id);
    src->attrs = TASK_ATTR_USER;
    dst->attrs = TASK_ATTR_USER;

    int copy_obj = 2101;
    int move_obj = 2102;
    int revoke_obj = 2103;
    cap_id_t copy_cap = cap_create_for(
        src, &copy_obj, CAP_OBJ_ENDPOINT,
        CAP_READ | CAP_WRITE | CAP_TRANSFER);
    cap_id_t move_cap = cap_create_for(
        src, &move_obj, CAP_OBJ_CHANNEL,
        CAP_READ | CAP_WRITE | CAP_TRANSFER);
    cap_id_t revoke_cap = cap_create_for(
        src, &revoke_obj, CAP_OBJ_EVENT, CAP_READ);
    TEST_ASSERT(copy_cap >= 0 && move_cap >= 0 && revoke_cap >= 0,
                "transaction source caps created");

    cap_transaction_t txn;
    cap_txn_begin(&txn);
    TEST_ASSERT_EQ((int)KERN_OK,
                   (int)cap_txn_prepare_copy(&txn, src, copy_cap, dst,
                                             CAP_READ),
                   "transaction COPY prepared");
    TEST_ASSERT_EQ((int)KERN_OK,
                   (int)cap_txn_prepare_move(&txn, src, move_cap, dst,
                                             CAP_READ),
                   "transaction MOVE prepared");
    TEST_ASSERT_EQ((int)KERN_OK,
                   (int)cap_txn_prepare_revoke(&txn, src, revoke_cap),
                   "transaction REVOKE prepared");
    TEST_ASSERT_EQ((int)KERN_OK, (int)cap_txn_commit(&txn),
                   "transaction batch committed");
    TEST_ASSERT_EQ((int)CAP_TXN_STATE_COMMITTED, (int)txn.state,
                   "transaction records committed state");

    TEST_ASSERT(cap_lookup_for(src, copy_cap, CAP_OBJ_ENDPOINT, CAP_READ) ==
                    &copy_obj,
                "COPY preserves source cap");
    TEST_ASSERT(cap_lookup_for(dst, txn.results[0], CAP_OBJ_ENDPOINT,
                               CAP_READ) == &copy_obj,
                "COPY publishes destination cap");
    TEST_ASSERT(cap_lookup_for(dst, txn.results[0], CAP_OBJ_ENDPOINT,
                               CAP_WRITE) == NULL,
                "COPY attenuates destination rights");

    TEST_ASSERT(cap_lookup_for(src, move_cap, CAP_OBJ_CHANNEL, CAP_READ) == NULL,
                "MOVE retires source-visible CPtr");
    TEST_ASSERT(cap_lookup_for(dst, txn.results[1], CAP_OBJ_CHANNEL,
                               CAP_READ) == &move_obj,
                "MOVE publishes destination cap");
    TEST_ASSERT(cap_lookup_for(dst, txn.results[1], CAP_OBJ_CHANNEL,
                               CAP_WRITE) == NULL,
                "MOVE attenuates destination rights");
    TEST_ASSERT(cap_lookup_for(src, revoke_cap, CAP_OBJ_EVENT, CAP_READ) == NULL,
                "REVOKE removes source cap in same commit");

    TEST_ASSERT_EQ((int)KERN_ERR_STATE, (int)cap_txn_commit(&txn),
                   "committed transaction cannot commit twice");
    TEST_ASSERT_EQ((int)KERN_ERR_STATE, (int)cap_txn_rollback(&txn),
                   "committed transaction cannot roll back");

    (void)cap_revoke_for(src, copy_cap);
    (void)cap_revoke_for(dst, txn.results[0]);
    (void)cap_revoke_for(dst, txn.results[1]);
    (void)task_delete(src_id);
    (void)task_delete(dst_id);
}

/*============================================================================
 * Test 24d: failed prepare validation consumes no cap/CNode generations
 *============================================================================*/

static void test_cap_transaction_failure_is_read_only(void) {
    test_section("Test 24d: failed cap transaction is read-only");

    task_id_t src_id = task_create("captxfs", cap_test_task, NULL, 10, 512);
    task_id_t dst_id = task_create("captxfd", cap_test_task, NULL, 10, 512);
    TEST_ASSERT(src_id >= 0 && dst_id >= 0,
                "failed-transaction tasks created");
    if (src_id < 0 || dst_id < 0) {
        return;
    }

    tcb_t *src = task_get_tcb(src_id);
    tcb_t *dst = task_get_tcb(dst_id);
    src->attrs = TASK_ATTR_USER;
    dst->attrs = TASK_ATTR_USER;

    int good_obj = 2201;
    int bad_obj = 2202;
    cap_id_t good = cap_create_for(src, &good_obj, CAP_OBJ_ENDPOINT,
                                   CAP_READ | CAP_TRANSFER);
    cap_id_t bad = cap_create_for(src, &bad_obj, CAP_OBJ_CHANNEL, CAP_READ);
    TEST_ASSERT(good >= 0 && bad >= 0, "failure source caps created");

    cnode_t *dst_cnode = cap_space_of(dst);
    TEST_ASSERT_NOT_NULL(dst_cnode, "failure target CNode available");
    if (dst_cnode == NULL) {
        (void)cap_revoke_for(src, good);
        (void)cap_revoke_for(src, bad);
        (void)task_delete(src_id);
        (void)task_delete(dst_id);
        return;
    }
    uint8_t free_slot = 0;
    while (free_slot < KERN_TASK_CAP_SLOTS &&
           (dst_cnode->occupied & (UINT64_C(1) << free_slot)) != 0U) {
        free_slot++;
    }
    TEST_ASSERT(free_slot < KERN_TASK_CAP_SLOTS,
                "failure target has a free CNode slot");
    if (free_slot >= KERN_TASK_CAP_SLOTS) {
        (void)cap_revoke_for(src, good);
        (void)cap_revoke_for(src, bad);
        (void)task_delete(src_id);
        (void)task_delete(dst_id);
        return;
    }
    uint32_t generation_before = dst_cnode->slots[free_slot].generation;
    uint64_t occupied_before = dst_cnode->occupied;
    uint16_t pool_before = cap_free_count();

    cap_transaction_t txn;
    cap_txn_begin(&txn);
    (void)cap_txn_prepare_copy(&txn, src, good, dst, CAP_READ);
    (void)cap_txn_prepare_copy(&txn, src, bad, dst, CAP_READ);
    TEST_ASSERT_EQ((int)KERN_ERR_CAP, (int)cap_txn_commit(&txn),
                   "invalid late op aborts complete transaction");
    TEST_ASSERT_EQ((int)CAP_TXN_STATE_ABORTED, (int)txn.state,
                   "failed transaction records aborted state");
    TEST_ASSERT(txn.results[0] < 0 && txn.results[1] < 0,
                "failed transaction publishes no results");
    TEST_ASSERT_EQ((int)pool_before, (int)cap_free_count(),
                   "failed transaction consumes no global cap slot");
    TEST_ASSERT(dst_cnode->occupied == occupied_before,
                "failed transaction changes no CNode membership");
    TEST_ASSERT_EQ((int)generation_before,
                   (int)dst_cnode->slots[free_slot].generation,
                   "failed transaction consumes no local generation");
    TEST_ASSERT(cap_lookup_for(src, good, CAP_OBJ_ENDPOINT, CAP_READ) ==
                    &good_obj,
                "failed transaction preserves transferable source");
    TEST_ASSERT(cap_lookup_for(src, bad, CAP_OBJ_CHANNEL, CAP_READ) == &bad_obj,
                "failed transaction preserves failing source");
    TEST_ASSERT_EQ((int)KERN_OK, (int)cap_txn_rollback(&txn),
                   "aborted transaction descriptor rolls back");
    TEST_ASSERT_EQ((int)KERN_ERR_STATE,
                   (int)cap_txn_prepare_copy(&txn, src, good, dst, CAP_READ),
                   "rolled-back descriptor requires a new begin");

    /* Leave exactly one destination slot free, then request two copies.  The
     * full batch must fail during reservation without consuming that slot's
     * local generation or either reserved global pool slot. */
    cap_id_t filler[KERN_TASK_CAP_SLOTS - 1];
    int filler_obj = 2203;
    int filled = 0;
    for (int i = 0; i < KERN_TASK_CAP_SLOTS - 1; i++) {
        filler[i] = cap_create_for(dst, &filler_obj, CAP_OBJ_EVENT, CAP_READ);
        if (filler[i] < 0) {
            break;
        }
        filled++;
    }
    TEST_ASSERT_EQ(KERN_TASK_CAP_SLOTS - 1, filled,
                   "batch-capacity target leaves one free slot");
    free_slot = 0;
    while (free_slot < KERN_TASK_CAP_SLOTS &&
           (dst_cnode->occupied & (UINT64_C(1) << free_slot)) != 0U) {
        free_slot++;
    }
    TEST_ASSERT(free_slot < KERN_TASK_CAP_SLOTS,
                "batch-capacity target retains one free slot");
    if (free_slot >= KERN_TASK_CAP_SLOTS) {
        for (int i = 0; i < filled; i++) {
            (void)cap_revoke_for(dst, filler[i]);
        }
        (void)cap_revoke_for(src, good);
        (void)cap_revoke_for(src, bad);
        (void)task_delete(src_id);
        (void)task_delete(dst_id);
        return;
    }
    generation_before = dst_cnode->slots[free_slot].generation;
    occupied_before = dst_cnode->occupied;
    pool_before = cap_free_count();

    cap_txn_begin(&txn);
    (void)cap_txn_prepare_copy(&txn, src, good, dst, CAP_READ);
    (void)cap_txn_prepare_copy(&txn, src, good, dst, CAP_READ);
    TEST_ASSERT_EQ((int)KERN_ERR_RESOURCE, (int)cap_txn_commit(&txn),
                   "batch capacity is reserved before publication");
    TEST_ASSERT_EQ((int)pool_before, (int)cap_free_count(),
                   "capacity failure consumes no global slots");
    TEST_ASSERT(dst_cnode->occupied == occupied_before,
                "capacity failure publishes no destination cap");
    TEST_ASSERT_EQ((int)generation_before,
                   (int)dst_cnode->slots[free_slot].generation,
                   "capacity failure consumes no CNode generation");

    for (int i = 0; i < filled; i++) {
        (void)cap_revoke_for(dst, filler[i]);
    }
    (void)cap_revoke_for(src, good);
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
 * Test 26b: deferred hooks run after the caller's outer object lock
 *============================================================================*/

static irq_spinlock_t cap_test_outer_lock;
static irq_spinlock_t cap_test_registry_lock;
static volatile int cap_safe_point_cleanup_count;

static void cap_safe_point_cleanup(void *object, uint8_t obj_type) {
    (void)object;
    if (obj_type == CAP_OBJ_SYSTEM) {
        uint32_t crit = irq_spin_lock(&cap_test_registry_lock);
        cap_safe_point_cleanup_count++;
        irq_spin_unlock(&cap_test_registry_lock, crit);
    }
}

static void test_cap_cleanup_outer_lock_safe_point(void) {
    test_section("Test 26b: cleanup waits for outer lock safe point");

    int obj = 1202;
    cap_safe_point_cleanup_count = 0;
    irq_spin_init_rank(&cap_test_outer_lock, LOCKDEP_RANK_OBJECT);
    irq_spin_init_rank(&cap_test_registry_lock, LOCKDEP_RANK_REGISTRY);
    TEST_ASSERT_EQ((int)KERN_OK,
                   (int)cap_register_cleanup(CAP_OBJ_SYSTEM,
                                             cap_safe_point_cleanup),
                   "register safe-point cleanup callback");

    cap_id_t cap = cap_create(&obj, CAP_OBJ_SYSTEM, CAP_FULL, 1);
    TEST_ASSERT(cap != ((cap_id_t)-1), "create safe-point test cap");

    uint32_t crit = irq_spin_lock(&cap_test_outer_lock);
    cap_delete(cap);
    TEST_ASSERT_EQ(0, cap_safe_point_cleanup_count,
                   "cleanup deferred while outer object lock held");
    irq_spin_unlock(&cap_test_outer_lock, crit);

    TEST_ASSERT_EQ(1, cap_safe_point_cleanup_count,
                   "cleanup ran once after outer object unlock");
    (void)cap_register_cleanup(CAP_OBJ_SYSTEM, NULL);
}

/*============================================================================
 * Test 25b: M2 验收 A3 — revoke 大型派生树后 cleanup 只调一次
 *
 * 构造深度 + 广度派生树 (root → child1 → grandchild; root → child2 →
 * grandchild2...),revoke root,cleanup 必须恰好调一次 (refcount 归零)。
 *============================================================================*/
static int cap_cleanup_deep_count;
static void cap_cleanup_deep_fn(void *object, uint8_t obj_type) {
    (void)object; (void)obj_type;
    cap_cleanup_deep_count++;
}

static void test_cap_revoke_deep_tree_cleanup_once(void) {
    test_section("Test 25b: deep derive tree cleanup once (A3)");

#if CAP_ENABLE
    int root_obj = 0xC0FFEE;
    /* root 持 GRANT,可派生 */
    cap_id_t root = cap_create(&root_obj, CAP_OBJ_MEMBLOCK,
                               CAP_FULL, 1);
    TEST_ASSERT(root > 0, "A3: root cap created");
    (void)cap_register_cleanup(CAP_OBJ_MEMBLOCK, cap_cleanup_deep_fn);
    cap_cleanup_deep_count = 0;

    /* 派生: root → child[0..4], 每个 child 再派生 grandchild。
     * 共 1 root + 5 child + 5 grandchild = 11 caps 指向同一 object。
     * 加上 root 自身 = 12 caps。但 cleanup 看 cap_pool 里指向该 object
     * 的 in_use cap 数,所以 revoke 整棵树后 refcount 应归 0。 */
    cap_id_t children[5];
    cap_id_t grandchildren[5];
    for (int i = 0; i < 5; i++) {
        children[i] = cap_derive(root, CAP_FULL);
        TEST_ASSERT(children[i] > 0, "A3: child cap derived");
        grandchildren[i] = cap_derive(children[i], CAP_FULL);
        TEST_ASSERT(grandchildren[i] > 0, "A3: grandchild cap derived");
    }

    /* revoke root:整棵树 (children + grandchildren) 都应被撤销 */
    kern_err_t err = cap_revoke(root);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "A3: revoke root");

    /* cleanup 必须恰好调 1 次 (refcount 归零才 cleanup) */
    TEST_ASSERT_EQ(1, cap_cleanup_deep_count,
                   "A3: cleanup called exactly once after deep tree revoke");

    (void)cap_register_cleanup(CAP_OBJ_MEMBLOCK, NULL);
#else
    test_skip("CAP_ENABLE off");
#endif
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

#if CAP_RESTART_SUBSET
/*============================================================================
 * Test 30: cap_derive_for_restart strips CAP_GRANT and installs into new task
 *============================================================================*/

static void test_cap_derive_for_restart_strips_grant(void) {
    test_section("Test 30: derive_for_restart strips CAP_GRANT");

    /* supervisor: a user task holding a CAP_FULL TASK cap */
    task_id_t sup_id = task_create("caprst_sup", cap_test_task, NULL, 10, 512);
    TEST_ASSERT(sup_id >= 0, "create supervisor task");
    tcb_t *sup = task_get_tcb(sup_id);
    TEST_ASSERT(sup != NULL, "get supervisor TCB");
    if (sup == NULL) { (void)task_delete(sup_id); return; }
    sup->attrs = TASK_ATTR_USER;

    /* The supervisor's "self" TASK cap (mimics what sys_task_create grants). */
    cap_id_t parent = cap_create_for(sup,
                                     (void *)(uintptr_t)(sup_id + 1),
                                     CAP_OBJ_TASK, CAP_FULL);
    TEST_ASSERT(parent != ((cap_id_t)-1), "supervisor gets CAP_FULL TASK cap");

    /* new task to receive the reduced-rights child */
    task_id_t new_id = task_create("caprst_new", cap_test_task, NULL, 10, 512);
    TEST_ASSERT(new_id >= 0, "create new task");
    tcb_t *new_task = task_get_tcb(new_id);
    TEST_ASSERT(new_task != NULL, "get new task TCB");
    if (new_task == NULL) {
        cap_delete(parent);
        (void)task_delete(sup_id);
        (void)task_delete(new_id);
        return;
    }
    new_task->attrs = TASK_ATTR_USER;
    uint64_t new_caps_before = test_cspace_occupied(new_task);

    /* Request CAP_FULL — CAP_GRANT must be stripped regardless. */
    cap_id_t child = cap_derive_for_restart(sup, parent, new_task, CAP_FULL);
    TEST_ASSERT(child != ((cap_id_t)-1), "derive_for_restart succeeds");
    TEST_ASSERT(test_cspace_occupied(new_task) != new_caps_before,
                "child installed into NEW task cspace");

    /* The child's rights must be CAP_FULL & ~CAP_GRANT. */
    uint8_t child_rights = 0;
    kern_err_t err = cap_get_rights_for(new_task, child, &child_rights);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "child rights queryable");
    TEST_ASSERT_EQ((int)(CAP_FULL & ~CAP_GRANT), (int)child_rights,
                   "child rights = CAP_FULL minus CAP_GRANT");
    TEST_ASSERT((child_rights & CAP_GRANT) == 0, "CAP_GRANT is stripped");

    /* Supervisor must NOT have gained a cspace entry (install target = new). */
    /* (supervisor already held parent; nothing new should appear for child.) */
    void *sup_resolve = cap_lookup_for(sup, child, CAP_OBJ_TASK, CAP_READ);
    TEST_ASSERT(sup_resolve == NULL,
                "supervisor does NOT receive the child cap handle");

    cap_delete(parent);
    (void)task_delete(sup_id);
    (void)task_delete(new_id);
}

/*============================================================================
 * Test 31: derive_for_restart denied without CAP_GRANT on parent
 *============================================================================*/

static void test_cap_derive_for_restart_no_grant(void) {
    test_section("Test 31: derive_for_restart denied without CAP_GRANT");

    task_id_t sup_id = task_create("caprst_ng", cap_test_task, NULL, 10, 512);
    tcb_t *sup = task_get_tcb(sup_id);
    TEST_ASSERT(sup != NULL, "create supervisor for no-grant test");
    if (sup == NULL) { (void)task_delete(sup_id); return; }
    sup->attrs = TASK_ATTR_USER;

    /* Parent with CAP_GRANT explicitly cleared. */
    cap_id_t parent = cap_create_for(sup,
                                     (void *)(uintptr_t)(sup_id + 1),
                                     CAP_OBJ_TASK,
                                     CAP_FULL & ~CAP_GRANT);
    TEST_ASSERT(parent != ((cap_id_t)-1), "create no-grant parent");

    task_id_t new_id = task_create("caprst_ng_new", cap_test_task, NULL, 10, 512);
    tcb_t *new_task = task_get_tcb(new_id);
    TEST_ASSERT(new_task != NULL, "create new task for no-grant test");
    if (new_task == NULL) {
        cap_delete(parent);
        (void)task_delete(sup_id);
        (void)task_delete(new_id);
        return;
    }
    new_task->attrs = TASK_ATTR_USER;

    cap_id_t child = cap_derive_for_restart(sup, parent, new_task, CAP_READ);
    TEST_ASSERT(child == ((cap_id_t)-1),
                "derive_for_restart denied without CAP_GRANT");

    cap_delete(parent);
    (void)task_delete(sup_id);
    (void)task_delete(new_id);
}
#endif /* CAP_RESTART_SUBSET */

/*============================================================================
 * Test 32: first-class CNode cap directs copy/mint/move
 *============================================================================*/

static void test_cnode_cap_operations(void) {
    test_section("Test 32: CNode cap Copy/Mint/Move");

    uint16_t free_before = cap_free_count();
    task_id_t src_id = task_create("cnode_src", cap_test_task, NULL, 10, 512);
    task_id_t dst_id = task_create("cnode_dst", cap_test_task, NULL, 10, 512);
    TEST_ASSERT(src_id >= 0 && dst_id >= 0, "CNode source/target tasks created");
    if (src_id < 0 || dst_id < 0) {
        if (src_id >= 0) (void)task_delete(src_id);
        if (dst_id >= 0) (void)task_delete(dst_id);
        return;
    }

    tcb_t *src = task_get_tcb(src_id);
    tcb_t *dst = task_get_tcb(dst_id);
    src->attrs = TASK_ATTR_USER;
    dst->attrs = TASK_ATTR_USER;

    int object = 0x321;
    cap_id_t source = cap_create_for(src, &object, CAP_OBJ_ENDPOINT, CAP_FULL);
    cap_id_t dst_node = cap_cnode_cap_create(src, dst, CAP_WRITE);
    TEST_ASSERT(source >= 0 && dst_node >= 0,
                "source and destination CNode caps created");

    cap_id_t copied = cap_cnode_copy(src, source, dst_node, CAP_READ);
    TEST_ASSERT(copied >= 0, "CNode Copy succeeds");
    TEST_ASSERT(cap_lookup_for(src, source, CAP_OBJ_ENDPOINT, CAP_READ) == &object,
                "CNode Copy preserves source");
    TEST_ASSERT(cap_lookup_for(dst, copied, CAP_OBJ_ENDPOINT, CAP_READ) == &object,
                "CNode Copy installs into target");

    const uint32_t badge = UINT32_C(0xC0DECAFE);
    cap_id_t minted = cap_cnode_mint(src, source, dst_node, CAP_READ, badge);
    TEST_ASSERT(minted >= 0, "CNode Mint succeeds");
    TEST_ASSERT_EQ((int)badge, (int)cap_get_badge(minted),
                   "CNode Mint records badge");
    TEST_ASSERT(cap_lookup_for(dst, minted, CAP_OBJ_ENDPOINT, CAP_WRITE) == NULL,
                "CNode Mint cannot amplify rights");

    int moved_object = 0x654;
    cap_id_t moved_source = cap_create_for(src, &moved_object,
                                           CAP_OBJ_CHANNEL,
                                           CAP_READ | CAP_TRANSFER);
    cap_id_t moved = KERN_INVALID_ID;
    kern_err_t err = cap_cnode_move(src, moved_source, dst_node, &moved);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "CNode Move succeeds");
    TEST_ASSERT(moved != moved_source && cap_is_local_cptr(moved),
                "CNode Move returns target-local CPtr");
    TEST_ASSERT(cap_lookup_for(src, moved, CAP_OBJ_CHANNEL, CAP_READ) == NULL,
                "CNode Move removes source membership");
    TEST_ASSERT(cap_lookup_for(dst, moved, CAP_OBJ_CHANNEL, CAP_READ) == &moved_object,
                "CNode Move installs target membership");

    cap_id_t readonly_node = cap_cnode_cap_create(src, dst, CAP_READ);
    TEST_ASSERT(readonly_node >= 0, "read-only CNode cap created");
    TEST_ASSERT(cap_cnode_copy(src, source, readonly_node, CAP_READ) < 0,
                "CNode Copy rejects destination without CAP_WRITE");

    /* Destroying the target revokes CNode caps held in another task. */
    (void)task_delete(dst_id);
    TEST_ASSERT(cap_lookup_for(src, dst_node, CAP_OBJ_CNODE, CAP_WRITE) == NULL,
                "target death revokes external CNode cap");
    (void)task_delete(src_id);
    TEST_ASSERT_EQ((int)free_before, (int)cap_free_count(),
                   "CNode operations leave no global cap leak");
}

/*============================================================================
 * Test 33: CNode object and local-slot generations survive TCB reuse
 *============================================================================*/

static void test_cnode_generation_on_task_reuse(void) {
    test_section("Test 33: CNode generation survives task reuse");

    task_id_t first_id = task_create("cnode_gen_a", cap_test_task, NULL, 10, 512);
    TEST_ASSERT(first_id >= 0, "first CNode generation task created");
    if (first_id < 0) return;

    tcb_t *first = task_get_tcb(first_id);
    first->attrs = TASK_ATTR_USER;
    cnode_t *first_node = cap_space_of(first);
    TEST_ASSERT(first_node != NULL, "first task owns root CNode");
    if (first_node == NULL) {
        (void)task_delete(first_id);
        return;
    }

    uint32_t object_generation = first_node->hdr.generation;
    int object = 7;
    cap_id_t cap = cap_create_for(first, &object, CAP_OBJ_EVENT, CAP_READ);
    TEST_ASSERT(cap >= 0, "cap installed for local generation test");

    uint8_t local_slot = UINT8_MAX;
    for (uint8_t i = 0; i < KERN_TASK_CAP_SLOTS; i++) {
        if ((first_node->occupied & (UINT64_C(1) << i)) != 0 &&
            first_node->slots[i].cap == cap) {
            local_slot = i;
            break;
        }
    }
    TEST_ASSERT(local_slot != UINT8_MAX, "installed cap has a CNode leaf slot");
    uint32_t local_generation = local_slot != UINT8_MAX
                              ? first_node->slots[local_slot].generation : 0;
    cap_delete(cap);
    if (local_slot != UINT8_MAX) {
        TEST_ASSERT(first_node->slots[local_slot].generation > local_generation,
                    "deleting cap advances local slot generation");
    }

    (void)task_delete(first_id);
    task_id_t second_id = task_create("cnode_gen_b", cap_test_task, NULL, 10, 512);
    TEST_ASSERT_EQ((int)first_id, (int)second_id,
                   "task allocator reuses the retired TCB slot");
    tcb_t *second = task_get_tcb(second_id);
    cnode_t *second_node = cap_space_of(second);
    TEST_ASSERT(second_node == first_node, "task reuse keeps persistent CNode storage");
    TEST_ASSERT(second_node != NULL &&
                second_node->hdr.generation != object_generation,
                "task reuse advances CNode object generation");
    if (second_node != NULL && local_slot != UINT8_MAX) {
        TEST_ASSERT(second_node->slots[local_slot].generation > local_generation,
                    "task reuse does not reset local slot generation");
    }
    (void)task_delete(second_id);
}

/*============================================================================
 * Test 34: user handles are CNode-local CPtrs, not backing-pool handles
 *============================================================================*/

static void test_local_cptr_isolation_and_stale_rejection(void) {
    test_section("Test 34: local CPtr isolation and stale rejection");

    task_id_t a_id = task_create("cptr_a", cap_test_task, NULL, 10, 512);
    task_id_t b_id = task_create("cptr_b", cap_test_task, NULL, 10, 512);
    TEST_ASSERT(a_id >= 0 && b_id >= 0, "local CPtr tasks created");
    if (a_id < 0 || b_id < 0) return;

    tcb_t *a = task_get_tcb(a_id);
    tcb_t *b = task_get_tcb(b_id);
    a->attrs = TASK_ATTR_USER;
    b->attrs = TASK_ATTR_USER;
    int a_obj = 101;
    int b_obj = 202;
    cap_id_t a_cap = cap_create_for(a, &a_obj, CAP_OBJ_ENDPOINT, CAP_READ);
    cap_id_t b_cap = cap_create_for(b, &b_obj, CAP_OBJ_ENDPOINT, CAP_READ);
    TEST_ASSERT(cap_is_local_cptr(a_cap) && cap_is_local_cptr(b_cap),
                "user-visible handles carry the local CPtr tag");
    TEST_ASSERT(a_cap != b_cap,
                "different root CNodes produce distinct CPtr namespaces");
    TEST_ASSERT(cap_lookup_for(a, b_cap, CAP_OBJ_ENDPOINT, CAP_READ) == NULL,
                "task A cannot resolve task B CPtr");
    TEST_ASSERT(cap_lookup_for(b, a_cap, CAP_OBJ_ENDPOINT, CAP_READ) == NULL,
                "task B cannot resolve task A CPtr");

    uint8_t old_cnode = 0;
    uint8_t old_slot = 0;
    uint32_t old_generation = 0;
    TEST_ASSERT_EQ((int)KERN_OK,
                   (int)cap_test_local_handle_info(a_cap, &old_cnode,
                                                   &old_slot,
                                                   &old_generation),
                   "local CPtr fields decode");
    TEST_ASSERT_EQ((int)a_id, (int)old_cnode,
                   "local CPtr names its root CNode index");

    TEST_ASSERT_EQ((int)KERN_OK, (int)cap_delete_for(a, a_cap),
                   "local CPtr delete succeeds");
    cap_id_t replacement = cap_create_for(a, &a_obj, CAP_OBJ_ENDPOINT,
                                           CAP_READ);
    TEST_ASSERT(cap_is_local_cptr(replacement) && replacement != a_cap,
                "local slot reuse advances visible generation");
    TEST_ASSERT(cap_lookup_for(a, a_cap, CAP_OBJ_ENDPOINT, CAP_READ) == NULL,
                "stale local CPtr is rejected after slot reuse");
    TEST_ASSERT(cap_lookup_for(a, replacement, CAP_OBJ_ENDPOINT, CAP_READ) ==
                &a_obj, "replacement local CPtr resolves");

    (void)task_delete(a_id);
    task_id_t reused_id = task_create("cptr_reuse", cap_test_task, NULL, 10, 512);
    TEST_ASSERT_EQ((int)a_id, (int)reused_id,
                   "local CPtr test reuses root CNode index");
    tcb_t *reused = task_get_tcb(reused_id);
    reused->attrs = TASK_ATTR_USER;
    cap_id_t after_reuse = cap_create_for(reused, &a_obj,
                                           CAP_OBJ_ENDPOINT, CAP_READ);
    TEST_ASSERT(cap_lookup_for(reused, replacement, CAP_OBJ_ENDPOINT,
                               CAP_READ) == NULL,
                "task reuse cannot revive an old local CPtr");
    TEST_ASSERT(after_reuse != replacement,
                "task reuse retains local slot generation history");

    (void)task_delete(reused_id);
    (void)task_delete(b_id);
}

/*============================================================================
 * Test 35: local CPtr generation exhaustion permanently retires the slot
 *============================================================================*/

static void test_local_cptr_generation_exhaustion(void) {
    test_section("Test 35: local CPtr generation exhaustion");

    task_id_t tid = task_create("cptr_limit", cap_test_task, NULL, 10, 512);
    TEST_ASSERT(tid >= 0, "local generation task created");
    if (tid < 0) return;
    tcb_t *task = task_get_tcb(tid);
    task->attrs = TASK_ATTR_USER;

    int object = 303;
    cap_id_t original = cap_create_for(task, &object, CAP_OBJ_EVENT, CAP_READ);
    cap_id_t boundary = KERN_INVALID_ID;
    uint8_t retired_slot = UINT8_MAX;
    kern_err_t err = cap_test_force_local_generation(
        task, original, cap_test_local_generation_limit(),
        &boundary, &retired_slot);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "force local CPtr to final generation");
    TEST_ASSERT(boundary != original && cap_is_local_cptr(boundary),
                "boundary handle remains a local CPtr");
    TEST_ASSERT(cap_lookup_for(task, original, CAP_OBJ_EVENT, CAP_READ) == NULL,
                "pre-boundary local CPtr becomes stale");
    TEST_ASSERT(cap_lookup_for(task, boundary, CAP_OBJ_EVENT, CAP_READ) ==
                &object, "final local generation resolves before delete");

    TEST_ASSERT_EQ((int)KERN_OK, (int)cap_delete_for(task, boundary),
                   "delete final local generation");
    cap_id_t next = cap_create_for(task, &object, CAP_OBJ_EVENT, CAP_READ);
    uint8_t next_cnode = 0;
    uint8_t next_slot = UINT8_MAX;
    uint32_t next_generation = 0;
    (void)cap_test_local_handle_info(next, &next_cnode, &next_slot,
                                     &next_generation);
    TEST_ASSERT(next >= 0 && next_slot != retired_slot,
                "allocator skips permanently retired local slot");

    (void)cap_delete_for(task, next);
    TEST_ASSERT_EQ((int)KERN_OK,
                   (int)cap_test_reset_retired_local_slot(task, retired_slot),
                   "test hook restores retired local slot");
    (void)task_delete(tid);
}

/*============================================================================
 * M3-Step2: cap transaction 异常组合测试
 *
 * prepare 只记录 op (不解析 cap),commit 时 cap_txn_validate_locked 才
 * 校验 task generation + cap 存在性。因此 prepare 与 commit 之间发生的
 * revoke / 对象删除 / sender fault 必须在 commit 被检测,且零残留。
 *============================================================================*/

static void test_txn_revoke_between_prepare_commit(void) {
    test_section("Test 27a: cap revoke between prepare and commit");

    task_id_t src_id = task_create("txnrs", cap_test_task, NULL, 10, 512);
    task_id_t dst_id = task_create("txnrd", cap_test_task, NULL, 10, 512);
    TEST_ASSERT(src_id >= 0 && dst_id >= 0, "create txn revoke tasks");
    if (src_id < 0 || dst_id < 0) {
        return;
    }

    tcb_t *src = task_get_tcb(src_id);
    tcb_t *dst = task_get_tcb(dst_id);
    src->attrs = TASK_ATTR_USER;
    dst->attrs = TASK_ATTR_USER;

    int obj = 2301;
    cap_id_t cap = cap_create_for(src, &obj, CAP_OBJ_ENDPOINT,
                                  CAP_READ | CAP_TRANSFER);
    TEST_ASSERT(cap >= 0, "create transferable cap");

    cap_transaction_t txn;
    cap_txn_begin(&txn);
    TEST_ASSERT_EQ((int)KERN_OK,
                   (int)cap_txn_prepare_copy(&txn, src, cap, dst, CAP_READ),
                   "COPY prepared before revoke");

    (void)cap_revoke_for(src, cap);

    /* 基线在 revoke 之后、commit 之前:commit 失败必须零残留 */
    uint16_t pool_at_commit = cap_free_count();
    uint64_t occupied_before = test_cspace_occupied(dst);

    kern_err_t commit_err = cap_txn_commit(&txn);
    TEST_ASSERT_EQ((int)KERN_ERR_CAP, (int)commit_err,
                   "commit fails after source revoke");
    TEST_ASSERT_EQ((int)CAP_TXN_STATE_ABORTED, (int)txn.state,
                   "failed commit records aborted state");
    TEST_ASSERT_EQ((int)pool_at_commit, (int)cap_free_count(),
                   "aborted commit consumes no pool slot");
    TEST_ASSERT(test_cspace_occupied(dst) == occupied_before,
                "aborted commit publishes no dst cap");
    TEST_ASSERT(cap_lookup_for(dst, txn.results[0], CAP_OBJ_ENDPOINT,
                               CAP_READ) == NULL,
                "no dst cap after aborted commit");

    (void)task_delete(src_id);
    (void)task_delete(dst_id);
}

static void test_txn_object_delete_between_prepare_commit(void) {
    test_section("Test 27b: object delete between prepare and commit");

    task_id_t src_id = task_create("txnob", cap_test_task, NULL, 10, 512);
    task_id_t dst_id = task_create("txnod", cap_test_task, NULL, 10, 512);
    TEST_ASSERT(src_id >= 0 && dst_id >= 0, "create txn object tasks");
    if (src_id < 0 || dst_id < 0) {
        return;
    }

    tcb_t *src = task_get_tcb(src_id);
    tcb_t *dst = task_get_tcb(dst_id);
    src->attrs = TASK_ATTR_USER;
    dst->attrs = TASK_ATTR_USER;

    int obj = 2302;
    cap_id_t cap = cap_create_for(src, &obj, CAP_OBJ_ENDPOINT,
                                  CAP_READ | CAP_TRANSFER);
    TEST_ASSERT(cap >= 0, "create transferable cap");

    cap_transaction_t txn;
    cap_txn_begin(&txn);
    TEST_ASSERT_EQ((int)KERN_OK,
                   (int)cap_txn_prepare_copy(&txn, src, cap, dst, CAP_READ),
                   "COPY prepared before object delete");

    (void)cap_revoke_object(&obj, CAP_OBJ_ENDPOINT);

    /* 基线在对象删除之后、commit 之前:commit 失败必须零残留 */
    uint16_t pool_at_commit = cap_free_count();
    uint64_t occupied_before = test_cspace_occupied(dst);

    TEST_ASSERT_EQ((int)KERN_ERR_CAP, (int)cap_txn_commit(&txn),
                   "commit fails after object delete");
    TEST_ASSERT_EQ((int)CAP_TXN_STATE_ABORTED, (int)txn.state,
                   "failed commit records aborted state");
    TEST_ASSERT_EQ((int)pool_at_commit, (int)cap_free_count(),
                   "aborted commit consumes no pool slot");
    TEST_ASSERT(test_cspace_occupied(dst) == occupied_before,
                "aborted commit publishes no dst cap");

    (void)task_delete(src_id);
    (void)task_delete(dst_id);
}

static void test_txn_sender_fault_after_transfer(void) {
    test_section("Test 27c: sender fault after transfer keeps dst cap");

    task_id_t src_id = task_create("txnsf", cap_test_task, NULL, 10, 512);
    task_id_t dst_id = task_create("txnfd", cap_test_task, NULL, 10, 512);
    TEST_ASSERT(src_id >= 0 && dst_id >= 0, "create sender fault tasks");
    if (src_id < 0 || dst_id < 0) {
        return;
    }

    tcb_t *src = task_get_tcb(src_id);
    tcb_t *dst = task_get_tcb(dst_id);
    src->attrs = TASK_ATTR_USER;
    dst->attrs = TASK_ATTR_USER;

    int obj = 2303;
    cap_id_t cap = cap_create_for(src, &obj, CAP_OBJ_ENDPOINT,
                                  CAP_READ | CAP_TRANSFER);
    TEST_ASSERT(cap >= 0, "create transferable cap");

    ipc_cap_xfer_t xfers[1];
    xfers[0].src_cap = cap;
    xfers[0].rights = CAP_READ;
    xfers[0].flags = IPC_CAP_COPY;
    cap_id_t out[1] = { (cap_id_t)-1 };
    kern_err_t err = ipc_transfer_caps(src, dst, xfers, 1, out);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "COPY succeeds before fault");
    TEST_ASSERT(out[0] >= 0, "COPY publishes dst cap id");

    /* sender fault: 传输完成后删除 src task */
    (void)task_delete(src_id);

    void *ptr = cap_lookup_for(dst, out[0], CAP_OBJ_ENDPOINT, CAP_READ);
    TEST_ASSERT(ptr == &obj, "dst cap survives sender fault");

    (void)cap_revoke_for(dst, out[0]);
    (void)task_delete(dst_id);
}

static void test_txn_sender_fault_between_prepare_commit(void) {
    test_section("Test 27d: sender fault between prepare and commit");

    task_id_t src_id = task_create("txnpf", cap_test_task, NULL, 10, 512);
    task_id_t dst_id = task_create("txnpd", cap_test_task, NULL, 10, 512);
    TEST_ASSERT(src_id >= 0 && dst_id >= 0, "create txn prepare tasks");
    if (src_id < 0 || dst_id < 0) {
        return;
    }

    tcb_t *src = task_get_tcb(src_id);
    tcb_t *dst = task_get_tcb(dst_id);
    src->attrs = TASK_ATTR_USER;
    dst->attrs = TASK_ATTR_USER;

    int obj = 2304;
    cap_id_t cap = cap_create_for(src, &obj, CAP_OBJ_ENDPOINT,
                                  CAP_READ | CAP_TRANSFER);
    TEST_ASSERT(cap >= 0, "create transferable cap");

    cap_transaction_t txn;
    cap_txn_begin(&txn);
    TEST_ASSERT_EQ((int)KERN_OK,
                   (int)cap_txn_prepare_copy(&txn, src, cap, dst, CAP_READ),
                   "COPY prepared before sender fault");

    /* sender fault: prepare 与 commit 之间删除 src (task generation bump) */
    (void)task_delete(src_id);

    /* 基线在 sender 删除之后、commit 之前:commit 失败必须零残留 */
    uint16_t pool_at_commit = cap_free_count();
    uint64_t occupied_before = test_cspace_occupied(dst);

    TEST_ASSERT_EQ((int)KERN_ERR_CAP, (int)cap_txn_commit(&txn),
                   "commit fails after sender deletion");
    TEST_ASSERT_EQ((int)CAP_TXN_STATE_ABORTED, (int)txn.state,
                   "failed commit records aborted state");
    TEST_ASSERT_EQ((int)pool_at_commit, (int)cap_free_count(),
                   "aborted commit consumes no pool slot");
    TEST_ASSERT(test_cspace_occupied(dst) == occupied_before,
                "aborted commit publishes no dst cap");

    (void)task_delete(dst_id);
}

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
    test_cap_generation_exhaustion_retires_slot();
    test_cap_object_stale_on_reuse();
    test_object_generation_exhaustion_retires_slot();
    test_cap_mint_badge();
    test_cap_task_id_reuse_stale();
    test_cap_copy_atomic_on_full();
    test_cap_revoke_cascade();
    test_cap_delete_preserves_children();
    test_cap_cspace_required_for_user();
    test_cap_cspace_install_and_revoke();
    test_cap_cspace_extended_slots();
    test_cap_cspace_over_32_slots();
    test_cap_revoke_object();
    test_cap_copy_to_task();
    test_cap_move_to_task();
    test_ipc_cap_transfer_rollback();
    test_ipc_cap_move_rollback();
    test_ipc_cap_move_success();
    test_cap_explicit_transaction();
    test_cap_transaction_failure_is_read_only();
    test_cap_object_refcount();
    test_cap_cleanup_callback();
    test_cap_cleanup_outer_lock_safe_point();
    test_cap_revoke_deep_tree_cleanup_once();
    test_txn_revoke_between_prepare_commit();
    test_txn_object_delete_between_prepare_commit();
    test_txn_sender_fault_after_transfer();
    test_txn_sender_fault_between_prepare_commit();
    test_mmio_cap_lifecycle();
#if CAP_RESTART_SUBSET
    test_cap_derive_for_restart_strips_grant();
    test_cap_derive_for_restart_no_grant();
#endif
    test_cnode_cap_operations();
    test_cnode_generation_on_task_reuse();
    test_local_cptr_isolation_and_stale_rejection();
    test_local_cptr_generation_exhaustion();
}

TEST_MODULE_REGISTER(capability, test_capability_module);

#endif /* CAP_ENABLE && TEST_MODULE_CAP */
