# Phase 2: 能力系统 + 内存管理 + 异常处理 — 功能完成表

> 状态说明: ⬜ 未开始 | 🔄 进行中 | ✅ 编译通过 | ❌ 测试失败
> 前置: Phase 1 已完成 (145 tests, 0 failures)
> 编译: **make all → 0 warnings** (2026-05-07)

---

## 一、能力系统核心 (`src/kernel/cap/`)

| # | 功能 | 状态 | 备注 |
|---|------|------|------|
| 1.1 | `capability.h` — cap_id_t, cap_entry_t, cap_obj_type_t, 权限位定义 | ✅ | |
| 1.2 | `capability.c` — 静态能力池 `cap_pool[CAP_MAX_COUNT]` | ✅ | |
| 1.3 | `cap_init()` — 池清零, token seed 初始化 | ✅ | LCG random seed |
| 1.4 | `cap_create(object, obj_type, rights, owner)` → cap_id_t (16-bit 随机 token) | ✅ | |
| 1.5 | `cap_delete(cap_id)` — 释放槽 | ✅ | |
| 1.6 | `cap_resolve(cap_id, obj_type, required_rights)` → object* (含权限+owner校验) | ✅ | |
| 1.7 | `cap_revoke_all(task_id)` — 任务结束时回收所有能力 | ✅ | |
| 1.8 | `cap_derive(cap_id, subset_rights)` — 降权派生 | ✅ | |
| 1.9 | `cap_transfer(cap_id, target_task)` — 转移所有权 | ✅ | |
| 1.10 | `cap_revoke(cap_id)` — 撤销单个能力 | ✅ | |
| 1.11 | 内核任务 (TASK_ATTR_PRIVILEGED) 跳过 owner 检查 | ✅ | |

## 二、能力系统集成 — IPC 对象改造

| # | 功能 | 状态 | 备注 |
|---|------|------|------|
| 2.1 | `sem_create` 返回 cap_id_t (syscall 层) | ✅ | handler 内部 cap_create |
| 2.2 | `sem_wait(cap_id, timeout)` 通过 cap_resolve 解析 | ✅ | |
| 2.3 | `sem_post(cap_id)` 通过 cap_resolve | ✅ | |
| 2.4 | `sem_delete(cap_id)` 通过 cap_resolve → cap_delete | ✅ | |
| 2.5 | `mutex_create` 返回 cap_id_t | ✅ | |
| 2.6 | `mutex_lock(cap_id, timeout)` 通过 cap_resolve | ✅ | |
| 2.7 | `mutex_unlock(cap_id)` 通过 cap_resolve | ✅ | |
| 2.8 | `mqueue_create` 返回 cap_id_t | ✅ | |
| 2.9 | `mqueue_send(cap_id, buf, size)` 通过 cap_resolve | ✅ | |
| 2.10 | `mqueue_recv(cap_id, buf, size)` 通过 cap_resolve | ✅ | |
| 2.11 | `event_create` 返回 cap_id_t | ✅ | |
| 2.12 | `event_wait(cap_id, mask, timeout)` 通过 cap_resolve | ✅ | |
| 2.13 | `event_set(cap_id, mask)` 通过 cap_resolve | ✅ | |
| 2.14 | `timer_create` 返回 cap_id_t | ✅ | |
| 2.15 | `timer_start/stop/reset/delete` 通过 cap_resolve | ✅ | |

## 三、能力系统集成 — Syscall 层

| # | 功能 | 状态 | 备注 |
|---|------|------|------|
| 3.1 | syscall handler 内部调用 cap_resolve (非直接使用 raw ID) | ✅ | 所有 IPC handler |
| 3.2 | cap_resolve 失败 → 返回 KERN_ERR_CAP | ✅ | |
| 3.3 | 新增 `SYSCALL_CAP_DERIVE` (29) — cap_derive syscall | ✅ | |
| 3.4 | 新增 `SYSCALL_CAP_TRANSFER` (30) — cap_transfer syscall | ✅ | |
| 3.5 | 新增 `SYSCALL_CAP_REVOKE` (31) — cap_revoke syscall | ✅ | |
| 3.6 | `user_api.h` — cap_id_t 类型, syscall 参数改为 cap_id | ✅ | cap_id_t 已是 int16_t |
| 3.7 | `KERN_ERR_CAP` 错误码 → kernel_types.h | ✅ | -7, 已存在 |

## 四、能力系统集成 — 任务管理

| # | 功能 | 状态 | 备注 |
|---|------|------|------|
| 4.1 | `task_delete` 时调用 `cap_revoke_all(task_id)` | ✅ | task.c |
| 4.2 | `task_exit` 时调用 `cap_revoke_all(self)` | ✅ | task.c |
| 4.3 | TCB `cap_set[8]` 跟踪任务持有的能力 | ⬜ | 结构已有, 低优先级 |
| 4.4 | `KERN_ENABLE_CAPABILITY` 编译开关启用 | ✅ | 别名 → CAP_ENABLE |
| 4.5 | `kern_init` 调用 `cap_init()` | ✅ | kernel.c |

## 五、内存管理 (MEM_ALLOC/FREE)

| # | 功能 | 状态 | 备注 |
|---|------|------|------|
| 5.1 | `sys_mem_alloc` — kmalloc(size) + cap_create(MEMBLOCK) → 返回 cap_id | ✅ | syscall.c |
| 5.2 | `sys_mem_free` — cap_resolve → kfree → cap_delete | ✅ | syscall.c |
| 5.3 | 用户 API: `mem_alloc(size)` / `mem_free(cap_id)` | ✅ | 通过 sys_callN |
| 5.4 | 无效 cap 或权限不足 → KERN_ERR_CAP | ✅ | |

## 六、Fault Handler (`src/kernel/fault/`)

| # | 功能 | 状态 | 备注 |
|---|------|------|------|
| 6.1 | `fault.h` — crash_dump_t 结构定义 | ✅ | ≤128B, _Static_assert |
| 6.2 | `fault.c` — fault_handler_c 分发函数 | ✅ | |
| 6.3 | `MemManage_Handler` — 读取 MMFSR+MMFAR, 用户任务→终止, 内核→panic | ✅ | startup.S + fault.c |
| 6.4 | `BusFault_Handler` — 读取 BFSR+BFAR, 同上 | ✅ | |
| 6.5 | `UsageFault_Handler` — 读取 UFSR, 同上 | ✅ | |
| 6.6 | `HardFault_Handler` — 完整上下文保存 → crash_dump → panic | ✅ | |
| 6.7 | 汇编 Fault 入口 — 保存 R4-R11, MSP, PSP 到 crash_dump | ✅ | FAULT_ENTRY 宏 |
| 6.8 | startup.S 向量表指向真实 fault handler (非 `_default_handler`) | ✅ | |
| 6.9 | `.crash_dump` section (128B, NOLOAD, SRAM) | ✅ | link/stm32f767.ld |
| 6.10 | `task_terminate` — 用户任务 fault → 标记 TERMINATED → 强制 PendSV | ✅ | fault.c |
| 6.11 | `kern_panic` 增强 — 接收 crash_dump, 打印诊断 → 死循环 | ✅ | |

## 七、Kconfig + 配置

| # | 功能 | 状态 | 备注 |
|---|------|------|------|
| 7.1 | `CAP_ENABLE` (bool, default y) | ✅ | |
| 7.2 | `CAP_MAX_COUNT` (int, default 32, range 8-128) | ✅ | |
| 7.3 | `FAULT_ENABLE` (bool, default y) | ✅ | |
| 7.4 | `FAULT_CRASH_DUMP` (bool, default y, depends FAULT_ENABLE) | ✅ | |
| 7.5 | `.config` 启用新增选项 | ✅ | |

## 八、Makefile 集成

| # | 功能 | 状态 | 备注 |
|---|------|------|------|
| 8.1 | 添加 `capability.c` 编译 + include 路径 | ✅ | |
| 8.2 | 添加 `fault.c` 编译 + include 路径 | ✅ | |
| 8.3 | 添加 `test_capability.c` (TEST_MODULE_CAP=y 时) | ✅ | |
| 8.4 | 添加 `test_fault.c` (TEST_MODULE_FAULT=y 时) | ✅ | |
| 8.5 | 编译零警告 (-Wall -Wextra -Werror) | ✅ | 已验证 |

## 九、测试 — 能力系统

| # | 测试 | 状态 | 备注 |
|---|------|------|------|
| 9.1 | cap_create → 返回有效 token, token 非零 | ✅ | test_capability.c |
| 9.2 | cap_create → 满池时返回 CAP_INVALID (-1) | ✅ | |
| 9.3 | cap_resolve → 正确 token + 权限 → 返回 object | ✅ | |
| 9.4 | cap_resolve → 错误 token → 返回 NULL | ✅ | |
| 9.5 | cap_resolve → 类型不匹配 → 返回 NULL | ✅ | |
| 9.6 | cap_resolve → 权限不足 → 返回 NULL | ✅ | |
| 9.7 | cap_delete → 删除后 cap_resolve 返回 NULL | ✅ | |
| 9.8 | cap_derive → 派生能力有子集权限 | ✅ | |
| 9.9 | cap_derive → 派生能力权限超集 → 失败 | ✅ | |
| 9.10 | cap_transfer → 转移后新 owner 可访问, 旧 owner 不可访问 | ✅ | 硬件验证通过 |
| 9.11 | cap_revoke_all → 任务删除后所有能力回收 | ✅ | |
| 9.12 | 内核任务跳过 owner 检查 | ✅ | 硬件验证通过 |
| 9.13 | 用户任务 owner 不匹配 → cap_resolve 返回 NULL | ✅ | 硬件验证通过 |

## 十、测试 — 内存管理

| # | 测试 | 状态 | 备注 |
|---|------|------|------|
| 10.1 | mem_alloc(64) → 返回有效 cap_id | ✅ | 硬件验证通过 |
| 10.2 | mem_alloc → cap_resolve → 可读写 | ✅ | 硬件验证通过 |
| 10.3 | mem_free → 释放后 cap_resolve 失败 | ✅ | 硬件验证通过 |
| 10.4 | mem_alloc(0) → 返回 KERN_ERR_PARAM | ✅ | 硬件验证通过 |

## 十一、测试 — Fault Handler

| # | 测试 | 状态 | 备注 |
|---|------|------|------|
| 11.1 | Fault handler 编译链接正确 (向量表替换验证) | ✅ | |
| 11.2 | crash_dump_t 结构大小 ≤ 128B | ✅ | test_fault.c Test 1 |
| 11.3 | `.crash_dump` section 在 map 文件中确认 | ✅ | 0x20007c00, 128B |
| 11.4 | 栈溢出 → MemManage Fault → 任务终止 (不 panic) | ⬜ | 需要硬件验证 |
| 11.5 | 用户任务写代码段 → MemManage Fault → 任务终止 | ⬜ | 需要硬件验证 |
| 11.6 | 内核 MPU 违规 → kern_panic | ⬜ | |

## 十二、回归测试 — 硬件验证

| # | 测试集 | 状态 | 备注 |
|---|--------|------|------|
| 12.1 | Phase 1 全部 145 tests 仍通过 (内核任务兼容模式) | ✅ | 224 total, 0 failures |
| 12.2 | 用户任务通过 cap_id 访问 IPC 对象 (替代 raw_id) | ✅ | syscall 测试验证 |
| 12.3 | 编译零警告 | ✅ | -Wall -Wextra -Werror |
| 12.3 | 编译零警告 | ✅ | -Wall -Wextra -Werror |

---

## 完成统计 (2026-05-07)

| 类别 | 总数 | 已完成 | 完成率 |
|------|------|--------|--------|
| 能力系统核心 | 11 | 11 | 100% |
| IPC 能力集成 | 15 | 15 | 100% |
| Syscall 层集成 | 7 | 7 | 100% |
| 任务管理集成 | 5 | 4 | 80% |
| 内存管理 | 4 | 4 | 100% |
| Fault Handler | 11 | 11 | 100% |
| Kconfig + 配置 | 5 | 5 | 100% |
| Makefile | 5 | 5 | 100% |
| 测试 — 能力 | 13 | 13 | 100% |
| 测试 — 内存 | 4 | 4 | 100% |
| 测试 — Fault | 6 | 3 | 50% |
| 测试 — 回归 | 3 | 3 | 100% |
| **总计** | **89** | **85** | **96%** |

---

> 创建日期: 2026-05-06
> 最后更新: 2026-05-07 (硬件验证: 224 passed / 0 failed)
> 待完成: 4.3 (cap_set 跟踪), 11.4-11.6 (硬件 fault 触发测试, 3 项)
