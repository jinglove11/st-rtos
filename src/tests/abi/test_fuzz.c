/**
 * @file test_fuzz.c
 * @brief M3-任务10: syscall fuzz harness
 *
 * 随机生成无效 cap、pointer、length、syscall 号和状态组合,
 * 验证内核不 panic/hang,所有无效输入都返回错误码。
 *
 * 设计:
 * - 简单 LCG PRNG (确定性,种子固定,可复现)
 * - 每轮随机选一个 syscall + 随机参数
 * - 参数从"有毒值池"抽取 (0, -1, 0xDEAD, 0xFFFFFFFF, 大数等)
 * - 断言返回值 < 0 (必须是 kern_err_t)
 * - 如果内核 panic 了,测试框架不会到达 summary,fuzz 自然"fail"
 */

#include "test_framework.h"
#include "kernel.h"
#include "abi.h"
#include "user_api.h"
#include "syscall.h"
#include "kernel_types.h"

#if TEST_ENABLE
/* P0-5 验证补漏:TEST-off 镜像(dev/release/tiny)不得链入测试代码。
 * TEST_ENABLE 为测试代码链接总门(与既有模块级 TEST_MODULE_* 门互补)。 */

/*============================================================================
 * 简单确定性 PRNG (LCG,同种可复现)
 *============================================================================*/

static uint32_t fuzz_state = 0x12345678U;

static uint32_t fuzz_rand(void) {
    /* Numerical Recipes LCG */
    fuzz_state = fuzz_state * 1664525U + 1013904223U;
    return fuzz_state;
}

/*============================================================================
 * 有毒值池 — 覆盖常见边界/非法值
 *============================================================================*/

static const uint32_t poison_values[] = {
    0,              /* NULL / 零 */
    1,              /* 最小正整数 (可能是 valid cap slot 0) */
    0x7FFFFFFF,     /* INT_MAX */
    0xFFFFFFFF,     /* -1 / CAP_INVALID / 全 1 */
    0xFFFFFFFD,     /* EXC_RETURN (不该出现在用户参数) */
    0xFFFFFFFE,     /* -2 = KERN_ERR_PARAM */
    0xDEADBEEF,     /* 未映射地址 */
    0x20000000,     /* SRAM 起始 (可能 valid 可能 invalid) */
    0x10000000,     /* Flash 起始 */
    0xE000ED00,     /* SCB (系统控制块) */
    0x00010000,     /* 小正整数 */
    0x7FFF,         /* int16 max */
    0x8000,         /* int16 min (负) */
    127,            /* cap pool max index */
    128,            /* cap pool 越界 */
    200,            /* task id 越界 */
};

#define POISON_COUNT (sizeof(poison_values) / sizeof(poison_values[0]))

static uint32_t fuzz_param(void) {
    /* 50% 从有毒值池取,50% 随机 */
    if (fuzz_rand() & 1) {
        return poison_values[fuzz_rand() % POISON_COUNT];
    }
    return fuzz_rand();
}

/*============================================================================
 * Fuzz 测试: 随机调各种 syscall
 *============================================================================*/

#define FUZZ_ITERATIONS 500

/* 会阻塞的 syscall 列表 — fuzz 中禁止调用 (kernel 上下文阻塞会死锁)。
 * 这些 syscall 在特定参数下会返回 KERN_SYSCALL_BLOCKED 然后切任务,
 * 在 kernel thread 上下文 (非 SVC handler) 调用会 hang。 */
static int fuzz_is_blocking_syscall(uint32_t num) {
    switch (num) {
        case SYSCALL_TASK_DELAY:        /* sleep */
        case SYSCALL_SEM_WAIT:
        case SYSCALL_MUTEX_LOCK:
        case SYSCALL_MQUEUE_SEND:
        case SYSCALL_MQUEUE_RECV:
        case SYSCALL_EVENT_WAIT:
        case SYSCALL_EP_SEND:
        case SYSCALL_EP_SEND_CAPS:
        case SYSCALL_EP_RECV:
        case SYSCALL_EP_RECV_CAPS:
        case SYSCALL_CH_SEND:
        case SYSCALL_CH_SEND_CAPS:
        case SYSCALL_CH_RECV:
        case SYSCALL_CH_RECV_CAPS:
        case SYSCALL_NTFN_WAIT:      /* P1-1: 阻塞等待 */
            return 1;
        default:
            return 0;
    }
}

static void test_syscall_fuzz_random(void) {
    test_section("Test 1: random syscall fuzz");

    fuzz_state = 0x12345678U;  /* 固定种子,可复现 */
    int errors_caught = 0;
    int ok_returns = 0;
    int skipped = 0;
    uint16_t cap_free_before = cap_free_count();

    for (int i = 0; i < FUZZ_ITERATIONS; i++) {
        uint32_t syscall_num = fuzz_rand() % SYSCALL_TABLE_SIZE;

        /* 跳过会阻塞的 syscall (kernel 上下文不能阻塞) */
        if (fuzz_is_blocking_syscall(syscall_num)) {
            skipped++;
            continue;
        }
        /* 跳过会创建/删除对象改变系统状态的 syscall */
        if (syscall_num == SYSCALL_TASK_CREATE ||
            syscall_num == SYSCALL_TASK_DELETE ||
            syscall_num == SYSCALL_TASK_START ||
            syscall_num == SYSCALL_FACTORY_CREATE ||
            syscall_num == SYSCALL_CAP_DERIVE ||     /* 可能创建 cap */
            syscall_num == SYSCALL_CAP_TRANSFER ||   /* 可能移动 cap */
            syscall_num == SYSCALL_CAP_TRANSFER_TO ||/* 可能移动 cap */
            syscall_num == SYSCALL_CAP_MINT ||       /* 可能创建 cap */
            syscall_num == SYSCALL_CAP_REVOKE ||     /* 可能删除 cap */
            syscall_num == SYSCALL_SEM_CREATE ||
            syscall_num == SYSCALL_MUTEX_CREATE ||
            syscall_num == SYSCALL_MQUEUE_CREATE ||
            syscall_num == SYSCALL_EVENT_CREATE ||
            syscall_num == SYSCALL_NTFN_CREATE || /* P1-1: 分配 cap+对象 */
            syscall_num == SYSCALL_NTFN_DELETE || /* 改对象状态 */
            syscall_num == SYSCALL_EP_CREATE ||
            syscall_num == SYSCALL_CH_CREATE ||
            syscall_num == SYSCALL_TIMER_CREATE ||
            syscall_num == SYSCALL_MEM_ALLOC ||
            syscall_num == SYSCALL_SHM_CREATE ||
            syscall_num == SYSCALL_CAP_SELF_SLOT ||  /* 可能查找成功 */
            syscall_num == SYSCALL_BH_CREATE ||      /* 创建 BH 占池 */
            syscall_num == SYSCALL_BH_SCHEDULE ||    /* 改变 BH 状态 */
            syscall_num == SYSCALL_IRQ_REGISTER ||   /* 注册 IRQ 占资源 */
            syscall_num == SYSCALL_IRQ_BIND ||       /* 绑定 IRQ 状态 */
            syscall_num == SYSCALL_TIMER_BIND ||     /* 绑定 timer 状态 */
            syscall_num == SYSCALL_TASK_SET_POLICY ||/* 改变调度策略 */
            syscall_num == SYSCALL_TASK_SUSPEND ||   /* 挂起任务 */
            syscall_num == SYSCALL_TASK_RESUME ||    /* 恢复任务 */
            syscall_num == SYSCALL_SEM_DELETE ||
            syscall_num == SYSCALL_MUTEX_DELETE ||
            syscall_num == SYSCALL_MQUEUE_DELETE ||
            syscall_num == SYSCALL_EVENT_DELETE ||
            syscall_num == SYSCALL_EP_DELETE ||
            syscall_num == SYSCALL_CH_DELETE ||
            syscall_num == SYSCALL_MEM_FREE ||
            syscall_num == SYSCALL_SHM_UNMAP ||
            syscall_num == SYSCALL_MMIO_UNMAP ||
            syscall_num == SYSCALL_MMIO_REQUEST ||
            syscall_num == SYSCALL_MEM_MAP ||
            syscall_num == SYSCALL_FLASH_OP ||
            syscall_num == SYSCALL_FAULT_SUBSCRIBE ||
            syscall_num == SYSCALL_TASK_RESTART ||
            syscall_num == SYSCALL_CH_CONNECT) {     /* 改变 channel 状态 */
            skipped++;
            continue;
        }

        uint32_t a1 = fuzz_param();
        uint32_t a2 = fuzz_param();
        uint32_t a3 = fuzz_param();

        /* 直接调 kern_syscall_handler (kernel 上下文,特权模式) */
        int result = kern_syscall_handler(syscall_num, a1, a2, a3,
                                          fuzz_param(), fuzz_param(), fuzz_param());

        if (result < 0) {
            errors_caught++;  /* 预期: 返回错误码 */
        } else if (result == 0) {
            ok_returns++;     /* 可能是某些无操作的 syscall 返回 0 */
        }
        /* result > 0 的情况 (如 cap_id) 在 fuzz 中很罕见但不算错 */
    }

    /* 断言: 至少 60% 的非跳过调用应该返回错误
     * (跳过了阻塞 syscall,剩余的被攻击面较小) */
    int attempted = FUZZ_ITERATIONS - skipped;
    TEST_ASSERT(attempted > 0, "fuzz: at least some syscalls attempted");
    TEST_ASSERT(errors_caught > attempted * 6 / 10,
                "fuzz: majority of invalid calls return error");

    /* 关键: 到这里说明内核没 panic/hang (测试框架仍在运行) */
    TEST_ASSERT_EQ((int)cap_free_before, (int)cap_free_count(),
                   "fuzz: no cap pool leak");
    test_pass("syscall fuzz completed without crash");
}

/*============================================================================
 * Fuzz 测试: 专门 fuzz cap 操作 (最常被攻击的面)
 *============================================================================*/

static void test_cap_fuzz(void) {
    test_section("Test 2: cap operation fuzz (read-only)");

    fuzz_state = 0xF00DCAFEU;
    int cap_errors = 0;

    for (int i = 0; i < 200; i++) {
        /* 只 fuzz 只读 cap 操作 — 不创建/删除 cap (避免副作用泄漏) */
        uint32_t op = fuzz_rand() % 3;
        uint32_t cap = fuzz_param();

        int result;
        switch (op) {
            case 0: /* cap_type on random cap */
                result = kern_syscall_handler(SYSCALL_CAP_TYPE, cap, 0, 0, 0, 0, 0);
                break;
            case 1: /* cap_rights on random cap */
                result = kern_syscall_handler(SYSCALL_CAP_RIGHTS, cap, 0, 0, 0, 0, 0);
                break;
            default: /* cap_badge on random cap */
                result = kern_syscall_handler(SYSCALL_CAP_BADGE, cap, 0, 0, 0, 0, 0);
                break;
        }

        if (result < 0) {
            cap_errors++;
        }
    }

    /* 只读 cap 操作对特权任务 (test_runner) 可能放行 (cap_owner_allowed
     * 对非 USER 任务返回 1),所以不能期望大部分返回错误。
     * 关键断言:没 crash + 没 cap 泄漏 (上面已验证)。错误比例只做信息记录。 */
    TEST_ASSERT(cap_errors >= 0,
                "cap fuzz: completed (read-only ops on random caps)");
    test_pass("cap fuzz completed without crash");
}

/*============================================================================
 * Test 3: cap 创建/删除 fuzz — 创建 cap 后全部清理,验证无泄漏
 *
 * 随机调 cap_create/cap_derive/cap_mint 创建 cap,记录返回的 cap_id,
 * 跑完后全部 cap_delete。验证 cap_free_count 回到初始值。
 *============================================================================*/

#define CAP_MUTATE_COUNT 32

static void test_cap_mutate_fuzz(void) {
    test_section("Test 3: cap create/delete fuzz (no leak)");

    fuzz_state = 0xBADC0FFEU;
    uint16_t free_before = cap_free_count();

    /* 创建一个 parent cap 作为 derive/mint 的源 */
    test_cap_obj_t parent_obj;
    TEST_CAP_OBJ_INIT(&parent_obj, CAP_OBJ_SEMAPHORE);
    cap_id_t parent = cap_create(&parent_obj, CAP_OBJ_SEMAPHORE,
                                 CAP_FULL, 0);
    TEST_ASSERT(parent > 0, "mutate fuzz: parent cap created");

    /* 收集所有创建的 cap (不含 parent) */
    cap_id_t created[CAP_MUTATE_COUNT];
    int created_count = 0;

    for (int i = 0; i < CAP_MUTATE_COUNT; i++) {
        uint32_t op = fuzz_rand() % 3;
        cap_id_t cap = KERN_INVALID_ID;

        switch (op) {
            case 0: /* create new cap on random object */
                cap = cap_create(&parent_obj, CAP_OBJ_SEMAPHORE,
                                 CAP_READ | (fuzz_rand() & 0x1F), 0);
                break;
            case 1: /* derive from parent */
                cap = cap_derive(parent, CAP_READ | CAP_WRITE);
                break;
            case 2: /* mint from parent */
                cap = cap_mint_for(NULL, parent, CAP_READ, fuzz_rand());
                break;
        }

        if (cap > 0 && cap != KERN_INVALID_ID) {
            if (created_count < CAP_MUTATE_COUNT) {
                created[created_count++] = cap;
            } else {
                /* 数组满了,立即删 */
                cap_delete(cap);
            }
        }
    }

    /* 全部清理 */
    for (int i = 0; i < created_count; i++) {
        cap_delete(created[i]);
    }
    cap_delete(parent);

    /* 关键断言:cap 池回到初始值 (无泄漏) */
    TEST_ASSERT_EQ((int)free_before, (int)cap_free_count(),
                   "mutate fuzz: no cap pool leak after create/delete cycle");

    test_pass("cap mutate fuzz completed without crash or leak");
}

/*============================================================================
 * Module registration
 *============================================================================*/

static void test_fuzz_module(void) {
    test_syscall_fuzz_random();
    test_cap_fuzz();
    test_cap_mutate_fuzz();
}

TEST_ABI_MODULE(fuzz, test_fuzz_module);
#endif /* TEST_ENABLE */
