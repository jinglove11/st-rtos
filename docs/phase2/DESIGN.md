# Phase 2: 能力系统 + 内存管理 + 异常处理 — 详细设计

> 目标: STM32F767ZI (Cortex-M7) | 日期: 2026-05-06
> 前置: Phase 1 (MPU + Syscall) 已完成, 145 tests, 0 failures

---

## 1. 设计目标

| 目标 | 现状 | 目标状态 |
|------|------|---------|
| 对象引用 | 原始 ID (sem_id_t, mutex_id_t...) | 能力令牌 (cap_id_t), 权限控制 |
| 权限模型 | 无, 任何任务可访问任何对象 | READ/WRITE/MANAGE 权限位图 |
| 能力传递 | 不支持 | 可在任务间转移/派生 |
| 内存管理 | mempool 仅内核用 | 用户任务可通过 syscall 分配/释放 |
| Fault 处理 | MemManage/BusFault/UsageFault → 死循环 | 捕获→诊断→终止用户任务/panic |

---

## 2. 子模块 A: 能力系统 (Capability)

### 2.1 能力模型

```
capability_t {
    uint16_t token;      // 不透明令牌 (随机生成, 非 ID), 用户任务持有
    uint8_t  rights;     // 权限位图
    uint8_t  owner;      // task_id 拥有者
    void    *object;     // 指向内核对象 (sem_t*, mutex_t*, ...)
    uint8_t  obj_type;   // 对象类型 (用于校验)
    uint8_t  in_use;     // 槽是否使用中
}
```

**权限位图 (5 bit)**:
```
bit 0: READ     — sem_wait, mqueue_recv, event_wait, mutex_lock
bit 1: WRITE    — sem_post, mqueue_send, event_set, mutex_unlock
bit 2: MANAGE   — delete, reset, stop
bit 3: TRANSFER — 可以转移给其他任务
bit 4: GRANT    — 可以创建子能力 (derive)
```

**对象类型**:
```c
CAP_OBJ_SEMAPHORE = 0,
CAP_OBJ_MUTEX     = 1,
CAP_OBJ_MQUEUE    = 2,
CAP_OBJ_EVENT     = 3,
CAP_OBJ_TIMER     = 4,
CAP_OBJ_IRQ       = 5,
CAP_OBJ_BH        = 6,
CAP_OBJ_MEMBLOCK  = 7,
```

### 2.2 能力池

```c
#define CAP_MAX_COUNT  32

typedef struct {
    cap_id_t    token;      // 16-bit 随机令牌
    uint8_t     rights;     // 权限位图
    uint8_t     owner;      // 拥有者 task_id
    void       *object;     // 内核对象指针
    uint8_t     obj_type;   // 对象类型
    uint8_t     in_use;
} cap_entry_t;

static cap_entry_t cap_pool[CAP_MAX_COUNT];
static uint16_t cap_token_seed;  // 随机种子, 初始化为硬件随机值
```

### 2.3 能力操作

```c
// 内部 API (内核调用)
cap_id_t cap_create(void *object, uint8_t obj_type, uint8_t rights, uint8_t owner);
void     cap_delete(cap_id_t cap);
void    *cap_resolve(cap_id_t cap, uint8_t obj_type, uint8_t required_rights);
void     cap_revoke_all(uint8_t owner);  // 任务结束时回收所有能力

// Syscall API (用户调用)
// SYSCALL_CAP_DERIVE   — cap_id = cap_derive(cap_id, subset_rights)
// SYSCALL_CAP_TRANSFER — cap_transfer(cap_id, target_task)
// SYSCALL_CAP_REVOKE   — cap_revoke(cap_id)
```

**token 生成**: 每次创建能力时, `seed = seed * 1103515245 + 12345`, 取低 16 位。确保 token 不可预测。

**cap_resolve 流程**:
```
1. 遍历 cap_pool 查找 token 匹配的 entry
2. 检查 in_use == 1
3. 检查 obj_type 匹配
4. 检查 (entry.rights & required_rights) == required_rights
5. 如果调用者是内核任务 (privileged) → 跳过 owner 检查
6. 如果调用者是用户任务 → 检查 entry.owner == current_task_id
7. 返回 object 指针, 或 NULL (校验失败)
```

### 2.4 Syscall 集成

**方案**: 每个 syscall handler 内部调用 `cap_resolve()` 解析对象, 不改变 syscall 编号或用户 API 签名。

```
修改前 (Phase 1):
  sys_sem_wait(a1, a2, ...)
    → sem_id_t sem = (sem_id_t)a1;  // 直接使用原始 ID
    → sem_wait(sem, timeout);

修改后 (Phase 2):
  sys_sem_wait(a1, a2, ...)
    → cap_id_t cap = (cap_id_t)a1;
    → sem_t *sem = cap_resolve(cap, CAP_OBJ_SEMAPHORE, CAP_READ);
    → if (!sem) return KERN_ERR_CAP;
    → return sem_wait_impl(sem, timeout);
```

**用户 API 变化**: `user_api.h` 中 `sem_id_t` 变为 `cap_id_t`。用户任务现在持有能力令牌而非原始 ID。

**内核任务兼容**: 内核任务 (TASK_ATTR_PRIVILEGED) 创建的对象, 自动分配能力并跳过 owner 检查。

### 2.5 对象创建时的能力自动分配

每个 `*_create()` 函数在创建对象后自动分配能力:

```c
// sem_create 内部
sem_id_t sem = alloc_sem();
// ... 初始化 ...
cap_id_t cap = cap_create(sem_ptr, CAP_OBJ_SEMAPHORE,
                          CAP_READ | CAP_WRITE | CAP_MANAGE | CAP_TRANSFER,
                          current_task_id);
// 存储 cap 到 sem 的内部字段 (用于 cap_resolve 反向查找)
sem->cap = cap;
return cap;  // 返回能力令牌给调用者
```

### 2.6 TCB 能力集

TCB 已有 `cap_set[8]` 字段。用于跟踪任务持有的能力, 在任务终止时批量回收。

```c
// task.c: task_delete() 内部
cap_revoke_all(task_id);
// → 遍历 cap_pool, 释放所有 owner == task_id 的能力
// → 释放关联的内核对象 (sem_delete, mutex_delete, ...)
```

### 2.7 文件变更

| 操作 | 文件 | 说明 |
|------|------|------|
| **新增** | `src/kernel/cap/capability.c` | 能力池 + create/resolve/delete/revoke_all |
| **新增** | `src/kernel/cap/capability.h` | cap_id_t, cap_entry_t, API 声明 |
| **修改** | `src/kernel/ipc/semaphore.c` | sem_create 返回 cap_id, sem_wait/post 通过 cap_resolve |
| **修改** | `src/kernel/ipc/mutex.c` | mutex_create 返回 cap_id, mutex_lock/unlock 通过 cap_resolve |
| **修改** | `src/kernel/ipc/mqueue.c` | mqueue_create 返回 cap_id, send/recv 通过 cap_resolve |
| **修改** | `src/kernel/ipc/event.c` | event_create 返回 cap_id, wait/set 通过 cap_resolve |
| **修改** | `src/kernel/timer/timer.c` | timer_create 返回 cap_id |
| **修改** | `src/kernel/syscall/syscall.c` | 新增 CAP_DERIVE/CAP_TRANSFER/CAP_REVOKE 条目 |
| **修改** | `src/kernel/syscall/syscall.h` | 新增 syscall 编号 |
| **修改** | `src/kernel/include/kernel_types.h` | 启用 `KERN_ENABLE_CAPABILITY`, 扩展 capability_t |
| **修改** | `src/kernel/task/task.c` | task_delete 调用 cap_revoke_all |
| **修改** | `src/kernel/kernel.c` | kern_init 调用 cap_init |
| **修改** | `Kconfig` | 新增 CAP_ENABLE, CAP_MAX_COUNT |
| **新增** | `src/tests/test_capability.c` | 能力生命周期测试 |

---

## 3. 子模块 B: 内存管理 (MEM_ALLOC/FREE)

### 3.1 目标

将现有的 `kmalloc`/`kfree` (mem.c) 暴露为用户 syscall, 添加能力校验。

### 3.2 设计

```
mem_alloc(size) → void* or NULL
  → SVC #1, SYSCALL_MEM_ALLOC, R1=size
  → 内部: kmalloc(size)
  → 如果成功: 创建 MEMBLOCK 能力, cap_rights=READ|WRITE|MANAGE
  → 返回 ptr (通过能力令牌映射)

mem_free(ptr_or_cap) → KERN_OK
  → SVC #1, SYSCALL_MEM_FREE, R1=cap_id
  → 查找 MEMBLOCK 能力
  → cap_resolve → 获取 ptr
  → kfree(ptr)
  → cap_delete(cap_id)
  → 返回 KERN_OK
```

简化方案 (Phase 2): 直接暴露 kmalloc/kfree, MEMBLOCK 能力绑定指针。

```c
// syscall handler
static kern_err_t sys_mem_alloc(uint32_t a1, ...) {
    void *ptr = kmalloc(a1);
    if (!ptr) return KERN_ERR_NOMEM;
    cap_id_t cap = cap_create(ptr, CAP_OBJ_MEMBLOCK,
                              CAP_READ | CAP_WRITE | CAP_MANAGE,
                              current_task_id);
    return (kern_err_t)cap;  // 返回能力令牌
}

static kern_err_t sys_mem_free(uint32_t a1, ...) {
    void *ptr = cap_resolve((cap_id_t)a1, CAP_OBJ_MEMBLOCK, CAP_MANAGE);
    if (!ptr) return KERN_ERR_CAP;
    kfree(ptr);
    cap_delete((cap_id_t)a1);
    return KERN_OK;
}
```

### 3.3 文件变更

| 操作 | 文件 | 说明 |
|------|------|------|
| **修改** | `src/kernel/syscall/syscall.c` | MEM_ALLOC(27) 和 MEM_FREE(28) 实现 (从占位→完整) |
| **修改** | `src/tests/test_syscall.c` | 新增 MEM_ALLOC/FREE 测试 |

---

## 4. 子模块 C: Fault 异常处理

### 4.1 目标

- MemManage_Handler: MPU 违规 → 诊断 → 终止用户任务
- BusFault_Handler: 总线错误 → 诊断 → panic/终止
- UsageFault_Handler: 未定义指令/非法操作 → 诊断 → 终止用户任务
- HardFault_Handler: 升级故障 → 保存上下文 → panic
- 用户任务崩溃不影响内核和其他任务

### 4.2 Fault 寄存器

```
CFSR (0xE000ED28) — 可配置故障状态寄存器
  [7:0]   MMFSR  — MemManage Fault Status
    [0] IACCVIOL  — 指令访问违规
    [1] DACCVIOL  — 数据访问违规
    [3] MUNSTKERR — 异常返回时出栈错误
    [4] MSTKERR   — 异常进入时入栈错误
    [5] MLSPERR   — 懒栈保存错误
  [15:8]  BFSR   — Bus Fault Status
    [0] IBUSERR   — 指令总线错误
    [1] PRECISERR — 精确数据总线错误
    [2] IMPRECISERR — 不精确数据总线错误
    [3] UNSTKERR  — 异常返回出栈总线错误
    [4] STKERR    — 异常进入入栈总线错误
    [5] LSPERR    — 懒栈保存总线错误
  [31:16] UFSR   — Usage Fault Status
    [0] UNDEFINSTR — 未定义指令
    [1] INVSTATE   — 无效状态
    [2] INVPC      — 无效 PC
    [3] NOCP       — 协处理器错误
    [8] UNALIGNED  — 未对齐访问
    [9] DIVBYZERO  — 除零

HFSR (0xE000ED2C) — HardFault Status
  [30] FORCED  — 由其他 fault 升级
  [1]  VECTTBL — 向量表读取错误

MMFAR (0xE000ED34) — MemManage Fault Address
BFAR  (0xE000ED38) — Bus Fault Address
```

### 4.3 Fault 处理流程

```
MemManage_Handler:
  1. 读取 CFSR, MMFAR
  2. 判断是否来自用户任务 (查看异常栈帧中的 xPSR bit 24)
  3. 如果来自用户任务:
     a. 记录 fault 信息到 TCB
     b. 调用 task_terminate(current_task)
     c. 触发 PendSV 调度下一个任务
  4. 如果来自内核:
     a. kern_panic("kernel MPU fault")
     b. 记录全部寄存器到 crash_dump

BusFault_Handler:
  同 MemManage, 读取 BFSR + BFAR

UsageFault_Handler:
  同 MemManage, 读取 UFSR (无专用地址寄存器)

HardFault_Handler:
  1. 保存 R0-R12, SP, LR, PC, xPSR 到 crash_dump
  2. 读取 CFSR, HFSR, MMFAR, BFAR
  3. hal_debug_puts("HARD FAULT")
  4. kern_panic("hard fault")
```

### 4.4 Crash Dump 结构

```c
typedef struct {
    uint32_t r0, r1, r2, r3, r4, r5, r6, r7;
    uint32_t r8, r9, r10, r11, r12;
    uint32_t sp, lr, pc, xpsr;
    uint32_t cfsr, hfsr, mmfar, bfar;
    uint32_t msp, psp;
    uint32_t fault_type;   // 0=HardFault, 1=MemManage, 2=BusFault, 3=UsageFault
    uint32_t task_id;
    uint32_t reserved[4];
} crash_dump_t;
```

保留在 `.crash_dump` section (NOLOAD, 固定 SRAM 地址, 重启后保留用于调试)。

### 4.5 链接脚本

```
.crash_dump (NOLOAD) : ALIGN(4)
{
    __crash_dump_start = .;
    . = . + 128;             // crash_dump_t (~112B)
    __crash_dump_end = .;
} > SRAM
```

### 4.6 Fault Handler 汇编入口

每个 Fault Handler 需要保存通用寄存器到 crash_dump 结构。C 函数无法完成此操作 (编译器可能修改寄存器), 所以需要汇编入口:

```asm
MemManage_Handler:
    // R0-R3, R12, LR, PC, xPSR 已在异常栈帧中 (MSP)
    // 保存 R4-R11 和 MSP, PSP 到 crash_dump
    ldr     r0, =__crash_dump_start
    stmia   r0!, {r4-r11}
    str     sp, [r0], #4      // MSP during exception
    mrs     r1, psp
    str     r1, [r0], #4      // PSP
    // ... 继续保存
    bl      fault_handler_c   // C 函数处理
```

### 4.7 文件变更

| 操作 | 文件 | 说明 |
|------|------|------|
| **新增** | `src/kernel/fault/fault.c` | Fault handler C 实现 + crash_dump |
| **新增** | `src/kernel/fault/fault.h` | crash_dump_t, 函数声明 |
| **修改** | `src/startup/arm/startup.S` | 向量表: MemManage/BusFault/UsageFault → 真实 handler |
| **修改** | `src/startup/arm/startup.S` | 汇编 fault 入口 (寄存器保存) |
| **修改** | `link/stm32f767.ld` | 添加 `.crash_dump` section |
| **新增** | `src/tests/test_fault.c` | Fault 注入测试 (栈溢出、MPU 违规) |
| **修改** | `Kconfig` | 新增 FAULT_ENABLE |

---

## 5. Kconfig 新增选项

```
menu "Capability Configuration"
    config CAP_ENABLE
        bool "Enable capability system"
        default y
    config CAP_MAX_COUNT
        int "Maximum capability entries"
        depends on CAP_ENABLE
        default 32
        range 8 128
endmenu

menu "Fault Handler Configuration"
    config FAULT_ENABLE
        bool "Enable fault handlers"
        default y
    config FAULT_CRASH_DUMP
        bool "Enable crash dump on fault"
        depends on FAULT_ENABLE
        default y
endmenu
```

---

## 6. 文件变更总览

| 操作 | 文件 | 模块 |
|------|------|------|
| **新增** | `src/kernel/cap/capability.c` | A |
| **新增** | `src/kernel/cap/capability.h` | A |
| **新增** | `src/kernel/fault/fault.c` | C |
| **新增** | `src/kernel/fault/fault.h` | C |
| **修改** | `src/kernel/ipc/semaphore.c` | A |
| **修改** | `src/kernel/ipc/mutex.c` | A |
| **修改** | `src/kernel/ipc/mqueue.c` | A |
| **修改** | `src/kernel/ipc/event.c` | A |
| **修改** | `src/kernel/timer/timer.c` | A |
| **修改** | `src/kernel/syscall/syscall.c` | A + B |
| **修改** | `src/kernel/syscall/syscall.h` | A + B |
| **修改** | `src/kernel/syscall/user_api.h` | A |
| **修改** | `src/kernel/include/kernel_types.h` | A |
| **修改** | `src/kernel/include/kernel.h` | A + C |
| **修改** | `src/kernel/task/task.c` | A |
| **修改** | `src/kernel/kernel.c` | A + C |
| **修改** | `src/startup/arm/startup.S` | C |
| **修改** | `link/stm32f767.ld` | C |
| **修改** | `Kconfig` | A + C |
| **修改** | `.config` | A + C |
| **修改** | `Makefile` | A + B + C |
| **新增** | `src/tests/test_capability.c` | A |
| **新增** | `src/tests/test_fault.c` | C |
| **修改** | `src/tests/test_syscall.c` | B |

---

## 7. 关键设计决策

| 决策 | 选择 | 理由 |
|------|------|------|
| 能力令牌 vs 原始 ID | 令牌 (16-bit random) | 不可预测, 防止用户任务伪造 |
| cap_resolve 归属 | 在每个 handler 中调用 | 保持显式, 易调试, 性能好 |
| 内核任务能力 | 自动授予 + 跳过 owner 检查 | 兼容 Phase 1 所有测试 |
| 对象删除时能力 | cap_delete 级联 | 防止悬垂引用 |
| 用户 API | syscall 参数从 raw_id 改为 cap_id | 完全隔离, 用户只能通过能力访问 |
| MEM_ALLOC 返回值 | 能力令牌 (cap_id_t) | 统一抽象, 内存也是受控资源 |
| Fault handler 语言 | C + 汇编入口 | 汇编保存寄存器, C 处理逻辑 |
| Crash dump 位置 | `.crash_dump` section (SRAM) | 重启后保留, 用于事后分析 |
| MemManage 策略 | 用户任务→终止, 内核→panic | 隔离故障, 保护内核 |

---

## 8. 验证策略

### 8.1 编译
- `make clean && make -Wall -Wextra -Werror` → 0 warnings

### 8.2 单元测试 (无硬件)
- 能力池管理: create/delete/resolve/revoke_all
- 权限校验: 正确的/错误的/不足的权限
- 内存分配: 大小检查, free 后不可用

### 8.3 硬件测试 (STM32F767)
- 145 回归测试全部通过 (内核任务兼容模式)
- 用户任务通过能力令牌访问 IPC 对象
- 用户任务使用无效能力 → KERN_ERR_CAP
- 用户任务使用不足权限 → KERN_ERR_CAP
- 能力 transfer 后新任务可访问
- mem_alloc/free 正确性
- 栈溢出 → MemManage Fault → 任务终止 (不 panic)
- 所有已有测试仍通过
