# Phase 1: MPU 内存保护 + 系统调用 — 详细设计

> 目标: STM32F767ZI (Cortex-M7) | 日期: 2026-04-30

---

## 1. 设计目标

将 my-rtos 从单体 RTOS 转变为真正的微内核：

| 目标 | 现状 | 目标状态 |
|------|------|---------|
| CPU 模式 | 所有代码运行在特权模式 | 内核特权，用户任务非特权 |
| 栈 | PSP/MSP 仅用于首次启动 | 用户任务 PSP，内核 MSP，全程分离 |
| 内存保护 | MPU 未编程 | 每任务独立 MPU region，栈溢出硬件检测 |
| 内核入口 | 函数直接调用 | 仅通过 SVC 系统调用进入 |
| 对象引用 | 直接指针 | 对象 ID（Phase 2 升级为能力令牌） |

---

## 2. 架构概览

```
                    ┌──────────────────────────┐
                    │       用户任务 (PSP)       │
                    │  非特权模式 (nPRIV=1)     │
                    │  ┌────────────────────┐  │
                    │  │  svc #1            │  │
                    │  │  R0 = syscall_num   │  │
                    │  │  R1-R3 = args      │  │
                    │  └────────┬───────────┘  │
                    └───────────┼──────────────┘
                                │ SVC 异常
                    ┌───────────▼──────────────┐
                    │   SVC_Handler (MSP)       │
                    │   特权模式                 │
                    │   ├─ SVC #0: 首次启动     │
                    │   └─ SVC #1: syscall 分发 │
                    │       └─ syscall_table[]  │
                    │           ├─ task_*       │
                    │           ├─ sem_*        │
                    │           ├─ mutex_*      │
                    │           └─ ...          │
                    └──────────────────────────┘
```

---

## 3. CPU 模式切换机制

### 3.1 CONTROL 寄存器

Cortex-M7 CONTROL 寄存器 (2 位)：

| Bit | 名称 | 0 | 1 |
|-----|------|---|---|
| 0 | nPRIV | 特权模式 | **非特权模式** (用户任务) |
| 1 | SPSEL | 使用 MSP | **使用 PSP** |

用户任务: `CONTROL = 0x3` (nPRIV=1, SPSEL=1) — 非特权 + PSP
内核: `CONTROL = 0x0` — 特权 + MSP

### 3.2 模式切换时机

```
异常进入时 (硬件自动):
  CONTROL.SPSEL → 0 (自动切换到 MSP)
  CONTROL.nPRIV → 0 (自动进入特权模式)

异常返回时 (硬件自动):
  从栈帧恢复 CONTROL → 回到用户模式 (nPRIV=1, PSP)
```

关键：SVC Handler 执行期间 CONTROL.SPSEL=0 (MSP)，退出时恢复 PSP。
这确保内核态的整个 syscall 执行都在 MSP 上，用户栈栈溢出不影响内核。

### 3.3 栈切换流程

```
用户任务运行中           SVC 进入              SVC_Handler 执行         SVC 退出
    PSP                   硬件自动:              手动:                   硬件自动:
    │                     保存 xPSR,PC,LR,     保存 R4-R11 到 PSP      恢复 R4-R11
    │                     R12,R3-R0 到 PSP     切换到 MSP               恢复 xPSR,PC,...
    │                     切换到 MSP           调用 C 处理函数           恢复 CONTROL
    ▼                     ▼                    ▼                        ▼
  [用户栈]              [用户栈]+[内核栈]      [内核栈 only]           [用户栈]
```

---

## 4. SVC Handler 重构

### 4.1 当前实现

`context.S:SVC_Handler` 当前只处理 SVC #0 (首次启动)，直接从 `_next_task` 恢复上下文并切换到 PSP：

```asm
SVC_Handler:
    ldr r0, =_next_task
    ldr r0, [r0]
    ldr r1, [r0]               ; first_task->sp
    ldmia r1!, {r4-r11}
    msr psp, r1
    movs r2, #2
    msr control, r2            ; SPSEL=1 (PSP), nPRIV=0 (仍特权!)
    ...
```

### 4.2 目标实现

```
SVC_Handler:
    ; 1. 提取 SVC 号
    ldr  r3, [sp, #24]         ; R3 = 异常栈帧中的 PC
    ldrb r3, [r3, #-2]         ; R3 = SVC 立即数 (低 8 位)

    ; 2. 路由
    cmp  r3, #0
    beq  .L_first_switch       ; SVC #0 → 首次启动 (现有逻辑)

    cmp  r3, #1
    beq  .L_general_syscall    ; SVC #1 → 通用系统调用

    ; 未知 SVC 号 → UsageFault
    b    .

.L_general_syscall:
    ; 保存用户上下文到 PSP
    mrs  r1, psp
    stmdb r1!, {r4-r11}
    msr  psp, r1

    ; R0-R3 已是用户参数 (syscall_num, arg1, arg2, arg3)
    bl   kern_syscall_handler

    ; 恢复用户上下文
    mrs  r1, psp
    ldmia r1!, {r4-r11}
    msr  psp, r1

    ; 异常返回 (R0 = syscall 返回值)
    ldr  lr, =0xFFFFFFFD       ; 返回线程模式, PSP
    bx   lr
```

### 4.3 首次启动也要设置 nPRIV

当前 SVC #0 只设了 `CONTROL=2` (SPSEL=1)，nPRIV 位仍是 0。
首次启动的任务如果是用户任务，需要 `CONTROL=3`：

```asm
.L_first_switch:
    ldr  r0, =_next_task
    ldr  r0, [r0]
    ldr  r1, [r0]              ; sp
    ldmia r1!, {r4-r11}
    msr  psp, r1

    ; 检查任务属性决定 CONTROL 值
    ldrb r2, [r0, #28]         ; TCB->attrs
    tst  r2, #1                ; bit 0: 是否非特权?
    ite  eq
    moveq r2, #2               ; 特权任务: SPSEL=1
    movne r2, #3               ; 用户任务: SPSEL=1 + nPRIV=1
    msr  control, r2
    isb
    ...
```

---

## 5. MPU 配置

### 5.1 Cortex-M7 MPU 特性

| 特性 | 值 |
|------|-----|
| Region 数量 | 8 (0-7) |
| 最小 region 大小 | 32 bytes |
| 最大 region 大小 | 4 GB |
| 对齐要求 | 大小对齐（起始地址必须是 region 大小的倍数） |
| 子区域 | 每个 region 分 8 个子区域，可独立禁用 |
| XN | 支持 Execute Never |

### 5.2 寄存器编程接口

```c
// RBAR (Region Base Address Register)
#define MPU_RBAR(addr, valid, region) \
    (((uint32_t)(addr) & 0xFFFFFFE0) | ((valid) ? (1 << 4) : 0) | ((region) & 0xF))

// RASR (Region Attribute and Size Register)
// size = log2(actual_size) - 1, 最小 4 (32B), 最大 31 (4GB)
#define MPU_RASR_ENABLE    (1 << 0)
#define MPU_RASR_SIZE(n)   (((n) & 0x1F) << 1)
#define MPU_RASR_SRD(n)    (((n) & 0xFF) << 8)   // SubRegion Disable
#define MPU_RASR_XN        (1 << 28)

// AP (Access Permission) 编码
#define MPU_AP_NOACCESS  (0x0 << 24)  // 000: 无访问
#define MPU_AP_PRW       (0x1 << 24)  // 001: 仅特权 RW
#define MPU_AP_PRW_URO   (0x2 << 24)  // 010: 特权 RW, 用户 RO
#define MPU_AP_FULL       (0x3 << 24)  // 011: 全访问
#define MPU_AP_PRO        (0x5 << 24)  // 101: 仅特权 RO
#define MPU_AP_PRW_URW    (0x3 << 24)  // 011: 特权 RW, 用户 RW

// 内存属性 (TEX, S, C, B)
#define MPU_ATTR_NORMAL   (0x0 << 16)  // Strongly-ordered
#define MPU_ATTR_WBWA     (0x7 << 16)  // 0011 111: Normal, WBWA, shareable
#define MPU_ATTR_DEVICE   (0x5 << 16)  // Device
```

MPU 寄存器操作步骤：
```c
// 选择 region 并写入基地址
MPU->RNR = region;
MPU->RBAR = base_addr | (1 << 4) | region;  // VALID=1

// 写入属性 + 大小 (同时生效)
MPU->RASR = rasr_value;

// 使能 MPU
SCB->SHCSR |= (1 << 0);  // MEMFAULTENA
MPU->CTRL = MPU_CTRL_ENABLE | MPU_CTRL_PRIVDEFENA;
//           bit 0: ENABLE         - 使能 MPU
//           bit 1: HFNMIENA       - NMI/HardFault 是否使用 MPU
//           bit 2: PRIVDEFENA     - 特权模式默认全访问 (背景区域)
```

重要的是 `PRIVDEFENA` 位：设为 1 意味着特权模式不受 MPU 限制（有背景区域默认全访问），非特权模式才受 region 限制。这样内核代码不需要配置自己的 region，简化了实现。

### 5.3 Per-Task MPU Region 布局

```
Region 0: 任务代码 (.text)        RO+X  (用户可执行, 不可写)
Region 1: 任务数据 (.data + .bss) RW    (用户可读写)
Region 2: 任务栈 (PSP)           RW    (用户可读写)
                                    └─ 子区域 0 禁用 (底部 1/8 = 栈溢出守卫)
Region 3: 共享内存 (可选)         RW    (跨任务通信, 通过能力系统授权)
Region 4: 外设区域 (可选)          RW    (仅驱动任务, 通过能力系统授权)
Region 5: 预留
Region 6: 预留
Region 7: 预留
```

### 5.4 栈溢出 MPU 守卫

```
1024B 任务栈, 子区域粒度 = 1024/8 = 128B

高地址 ┌────────────┐ Base + 1024  (Region start)
       │  子区域 7   │ RW
       │  子区域 6   │ RW
       │  子区域 5   │ RW
       │  子区域 4   │ RW
       │  子区域 3   │ RW
       │  子区域 2   │ RW
       │  子区域 1   │ RW
低地址 │  子区域 0   │ SRD=1 (NO ACCESS)
       └────────────┘ Base
                     
如果 SP 低于 Base+128 → MemManage Fault (硬件立即检测)
```

RASR 配置：
```c
uint32_t rasr = MPU_RASR_ENABLE
              | MPU_RASR_SIZE(log2_size - 1)
              | MPU_RASR_SRD(0x01)   // 禁用子区域 0 (最底部 1/8)
              | MPU_AP_FULL           // 特权+用户都可以 RW
              | MPU_ATTR_NORMAL;
```

### 5.5 MPU 上下文切换流程

在 PendSV_Handler 的 `kern_pendsv_handler()` 返回后、恢复新任务上下文前插入 MPU 配置：

```asm
    ; 调用 C 函数选择下一个任务
    bl   kern_pendsv_handler

    ; 获取 _next_task
    ldr  r0, =_next_task
    ldr  r0, [r0]
    cmp  r0, #0
    beq  .L_pendsv_halt

    ; ★ 新增: 配置 MPU region 为下一个任务
    ; r0 = next_tcb
    ; 调用 mpu_load_task_regions(next_tcb)
    push {r0, lr}
    bl   mpu_load_task_regions
    pop  {r0, lr}

    ; 恢复栈指针和寄存器
    ldr  r1, [r0]               ; next->sp
    ldmia r1!, {r4-r11}
    msr  psp, r1
    ...
```

---

## 6. 系统调用表

### 6.1 Syscall 编号

```c
// 任务管理
#define SYSCALL_TASK_YIELD       0
#define SYSCALL_TASK_DELAY       1
#define SYSCALL_TASK_EXIT        2
#define SYSCALL_TASK_CREATE      3
#define SYSCALL_TASK_START       4
#define SYSCALL_TASK_SUSPEND     5
#define SYSCALL_TASK_RESUME      6
#define SYSCALL_TASK_DELETE      7
#define SYSCALL_TASK_SELF        8

// IPC — 信号量
#define SYSCALL_SEM_CREATE       9
#define SYSCALL_SEM_WAIT         10
#define SYSCALL_SEM_POST         11
#define SYSCALL_SEM_DELETE       12

// IPC — 互斥锁
#define SYSCALL_MUTEX_CREATE     13
#define SYSCALL_MUTEX_LOCK       14
#define SYSCALL_MUTEX_UNLOCK     15

// IPC — 消息队列
#define SYSCALL_MQUEUE_CREATE    16
#define SYSCALL_MQUEUE_SEND      17
#define SYSCALL_MQUEUE_RECV      18

// IPC — 事件
#define SYSCALL_EVENT_CREATE     19
#define SYSCALL_EVENT_WAIT       20
#define SYSCALL_EVENT_SET        21

// 定时器
#define SYSCALL_TIMER_CREATE     22
#define SYSCALL_TIMER_START      23

// 中断 (需要能力)
#define SYSCALL_IRQ_REGISTER     24
#define SYSCALL_BH_CREATE        25
#define SYSCALL_BH_SCHEDULE      26

// 内存
#define SYSCALL_MEM_ALLOC        27
#define SYSCALL_MEM_FREE         28
```

### 6.2 分发表

```c
typedef kern_err_t (*syscall_fn_t)(uint32_t arg1, uint32_t arg2, uint32_t arg3);

static const syscall_fn_t syscall_table[] = {
    [SYSCALL_TASK_YIELD]    = (syscall_fn_t)sys_task_yield,
    [SYSCALL_TASK_DELAY]    = (syscall_fn_t)sys_task_delay,
    [SYSCALL_TASK_EXIT]     = (syscall_fn_t)sys_task_exit,
    [SYSCALL_TASK_CREATE]   = (syscall_fn_t)sys_task_create,
    [SYSCALL_TASK_START]    = (syscall_fn_t)sys_task_start,
    [SYSCALL_TASK_SUSPEND]  = (syscall_fn_t)sys_task_suspend,
    [SYSCALL_TASK_RESUME]   = (syscall_fn_t)sys_task_resume,
    [SYSCALL_TASK_DELETE]   = (syscall_fn_t)sys_task_delete,
    [SYSCALL_TASK_SELF]     = (syscall_fn_t)sys_task_self,
    // ... 继续
};

kern_err_t kern_syscall_handler(uint32_t syscall_num,
                                 uint32_t arg1, uint32_t arg2, uint32_t arg3)
{
    if (syscall_num >= ARRAY_SIZE(syscall_table))
        return KERN_ERR_PARAM;
    if (syscall_table[syscall_num] == NULL)
        return KERN_ERR_PARAM;

    return syscall_table[syscall_num](arg1, arg2, arg3);
}
```

### 6.3 用户态 wrapper

用户任务不直接执行 `svc` 指令，而是调用封装函数：

```c
// syscall.h — 用户态 API
static inline kern_err_t task_yield(void) {
    register uint32_t r0 __asm("r0") = SYSCALL_TASK_YIELD;
    __asm volatile("svc #1" :: "r"(r0) : "memory");
    register kern_err_t ret __asm("r0");
    return ret;
}

static inline kern_err_t task_delay(uint32_t ticks) {
    register uint32_t r0 __asm("r0") = SYSCALL_TASK_DELAY;
    register uint32_t r1 __asm("r1") = ticks;
    __asm volatile("svc #1" :: "r"(r0), "r"(r1) : "memory");
    register kern_err_t ret __asm("r0");
    return ret;
}
```

---

## 7. TCB 扩展

```c
typedef struct tcb {
    // === 已有字段 (保持不变) ===
    void       *sp;                   // offset 0  — 栈指针 (汇编访问)
    char        name[KERN_TASK_NAME_LEN];  // offset 4
    task_id_t   id;                   // offset 20
    uint8_t     priority;             // offset 24
    uint8_t     base_priority;        // offset 25
    task_state_t state;               // offset 26
    void       *stack_base;           // offset 28
    uint32_t    stack_size;           // offset 32
    uint32_t    time_slice;           // offset 36
    uint32_t    time_slice_reload;    // offset 40
    uint32_t    total_ticks;          // offset 44
    uint32_t    wake_tick;            // offset 48
    block_reason_t block_reason;      // offset 52
    void       *block_obj;            // offset 56
    kern_err_t  block_result;         // offset 60
    struct tcb *next;                 // offset 64
    struct tcb *prev;                 // offset 68
    struct tcb *wait_next;            // offset 72
    struct tcb *wait_prev;            // offset 76

    // === Phase 1 新增字段 ===
    uint8_t     attrs;                // offset 80 — TASK_ATTR_PRIVILEGED / TASK_ATTR_USER
    uint8_t     _pad[3];              // 对齐
    uint32_t    mpu_regions[8][2];   // offset 84 — [RBAR, RASR] × 8 = 64 bytes

    // === Phase 2 预留 ===
    cap_id_t    cap_set[8];          // offset 148

    // === 统计字段 (已有) ===
#if KERN_TASK_STATS
    uint32_t    ctx_switch_count;
    uint32_t    cpu_usage;
#endif
} tcb_t;

// 任务属性
#define TASK_ATTR_PRIVILEGED   0x00   // 内核任务 (特权模式)
#define TASK_ATTR_USER         0x01   // 用户任务 (非特权模式)
```

> `sp` 仍在 offset 0，`state` 仍在 offset 26 — PendSV 汇编依赖不变。

---

## 8. 地址空间

### 8.1 内存布局 (384KB SRAM)

```
SRAM (0x20000000 - 0x20060000, 384KB)

0x20060000 ┌────────────────────┐ _estack (MSP 初始值)
           │    内核栈 (MSP)     │ 4KB
0x2005F000 ├────────────────────┤
           │    RAM 向量表       │ 512B (128×4, 512对齐)
0x2005EE00 ├────────────────────┤
           │    内核 .bss       │ (TCB pool, sem pool, 等)
           │    内核 .data      │
0x200xxxxx ├────────────────────┤
           │    用户任务栈池     │ (每任务独立, MPU 保护)
           │    用户任务数据     │
           ├────────────────────┤
           │    共享内存区域     │ (Phase 5: 跨任务通信)
           ├────────────────────┤
           │    堆 / 动态内存    │ (mempool)
0x20000000 └────────────────────┘
```

### 8.2 链接脚本修改

需要为内核保留专用栈区域（当前 `_estack` 是整个 SRAM 顶部）：

```ld
/* 新增: 内核数据段放在 SRAM 中高地址区域 */
.kernel_data (NOLOAD) :
{
    . = ALIGN(4);
    __kernel_data_start = .;
    . = . + 4K;              /* 内核栈 (MSP) */
    __kernel_stack_top = .;
    __kernel_data_end = .;
} > SRAM
```

---

## 9. 文件变更清单

| 操作 | 文件 | 说明 |
|------|------|------|
| **新增** | `src/kernel/mpu/mpu.c` | MPU region 编程、子区域守卫计算、上下文切换时加载 region |
| **新增** | `src/kernel/mpu/mpu.h` | MPU 寄存器宏、region 号定义 |
| **新增** | `src/kernel/syscall/syscall.c` | syscall 分发表、kern_syscall_handler 实现 |
| **新增** | `src/kernel/syscall/syscall.h` | syscall 编号宏、用户态 wrapper 函数 |
| **新增** | `src/kernel/syscall/user_api.h` | 用户任务可直接 include 的 syscall 封装 |
| **修改** | `src/arch/arm/cortex-m7/context.S` | SVC_Handler 重构（支持 #0 + #1）；PendSV 增加 MPU 配置调用 |
| **修改** | `src/arch/arm/cortex-m7/hal.c` | `hal_cpu_init` 使能 MPU；新增 MPU 操作函数 |
| **修改** | `src/kernel/kernel.c` | `kern_syscall_handler` 指向 syscall.c 的实现 |
| **修改** | `src/kernel/task/task.c` | TCB 初始化扩展（attrs, mpu_regions）；用户任务创建 API |
| **修改** | `src/kernel/include/kernel_types.h` | TCB 新增 attrs, mpu_regions, cap_set 字段 |
| **修改** | `src/kernel/include/kernel.h` | include syscall.h, mpu.h |
| **修改** | `link/stm32f767.ld` | 内核栈区域保留 |
| **修改** | `Kconfig` | MPU_ENABLE, SYSCALL_TABLE_SIZE |
| **新增** | `src/tests/test_mpu.c` | MPU 违规测试、栈溢出检测测试 |
| **新增** | `src/tests/test_syscall.c` | syscall 正确性测试 |
| **修改** | `Makefile` | 新源文件、include 路径 |

---

## 10. 关键设计决策

| 决策 | 选择 | 理由 |
|------|------|------|
| 临界区方式 | PRIMASK | BASEPRI 最大值 15 无法屏蔽 PendSV (优先级 15) |
| SVC 路由 | SVC #0 首次启动, #1 通用 | 兼容现有代码，指令中 8-bit 立即数足够区分 |
| MPU 背景区域 | PRIVDEFENA=1 | 内核特权模式自动全访问，无需为内核配置 region |
| 内核任务 | 保留特权模式 | Phase 1 先保护用户任务，内核任务逐步迁移 |
| region 数量 | 8 (全用) | 0=代码, 1=数据, 2=栈, 3-7 留给共享内存/外设 |
| 栈守卫方式 | 子区域禁用 | 不浪费 region，一个 region 内完成 |
| syscall 参数 | R0-R3 (4 个) | ARM AAPCS 规定，够用，复杂数据通过共享内存 |
| 返回值 | R0 = kern_err_t | 所有 syscall 统一返回错误码 |

---

## 11. 兼容性过渡策略

Phase 1 完成后存在两种任务：

```
内核任务 (TASK_ATTR_PRIVILEGED):
  - CONTROL = 0x2 (SPSEL=1, nPRIV=0)
  - 特权模式，可访问所有地址
  - 逐步减少，最终只保留 idle + 少量内核服务

用户任务 (TASK_ATTR_USER):
  - CONTROL = 0x3 (SPSEL=1, nPRIV=1)
  - 非特权模式，MPU 受限
  - 通过 SVC 调用一切内核功能
```

现有 121 个测试中的内核任务先以 `TASK_ATTR_PRIVILEGED` 运行，保证全部通过。
然后逐步将测试任务迁移到 `TASK_ATTR_USER`，验证 syscall 路径。
