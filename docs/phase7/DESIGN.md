# Phase 7: Diagnostic Ecosystem — 设计文档

> 版本: 1.0 | 日期: 2026-05-09 | 依赖: Phase 6 (Fault Tolerance)

---

## 一、背景与目标

### 1.1 现状

Phase 6 已经实现了诊断生态的基础设施，但分布零散：

| 组件 | Phase 6 状态 | 不足 |
|------|-------------|------|
| **Trace** | ring buffer + 4 事件类型 | ISR/BH/IPC/FAULT 事件未接入; buffer 仅 128 |
| **Shell** | 12 命令 (ps/top/trace/free...) | 缺 crash/mem 命令; trace 无过滤 |
| **CPU Stats** | inline 在 scheduler.c | 无独立模块; 缺 IRQ 延迟/上下文切换计数 |
| **Crash Dump** | crash_dump_t 存入 .crash_dump | 无解码工具 (shell 命令或 host 脚本) |

### 1.2 Phase 7 目标

1. **完整的 Trace 覆盖** — 所有关键内核路径都有 trace point
2. **独立的诊断模块** — stats 从 scheduler 解耦; trace 功能完整
3. **运行时诊断** — shell 可查 crash dump、内存布局、详细统计
4. **离线分析工具** — Python 脚本解析 ELF 中的 trace buffer 和 crash dump
5. **诊断文档** — 用户知道如何用这些工具定位问题

---

## 二、架构

```
┌──────────────────────────────────────────────────────┐
│                   诊断生态系统                         │
│                                                      │
│  ┌──────────┐  ┌──────────┐  ┌──────────────────┐   │
│  │  Trace   │  │  Stats   │  │  Crash Dump      │   │
│  │  System  │  │  Module  │  │  (.crash_dump)   │   │
│  │  (ring)  │  │  (独立)   │  │                   │   │
│  └────┬─────┘  └────┬─────┘  └────────┬──────────┘   │
│       │              │                 │              │
│       └──────────────┼─────────────────┘              │
│                      │                                │
│              ┌───────┴───────┐                        │
│              │    Shell      │   运行时访问            │
│              │  crash/mem/   │                        │
│              │  stats/trace  │                        │
│              └───────────────┘                        │
│                      │                                │
│              ┌───────┴───────┐                        │
│              │  Host Tools   │   离线分析              │
│              │  Python 脚本   │   (PC 端)              │
│              └───────────────┘                        │
└──────────────────────────────────────────────────────┘
```

### 2.1 Trace 事件扩展

```
当前 (4 事件):
  TRACE_TASK_SWITCH  — 已接入 scheduler
  TRACE_ISR_ENTER    — 定义但未接入 ★
  TRACE_ISR_EXIT     — 定义但未接入 ★
  TRACE_SYSCALL      — 已接入 syscall dispatcher

Phase 7 新增 (3 事件):
  TRACE_IPC_SEND     — endpoint/channel send
  TRACE_IPC_RECV     — endpoint/channel recv
  TRACE_BH_SCHEDULE  — bottom half scheduling
  TRACE_FAULT        — fault_handler_c entry ★★★ 最重要

Buffer 扩展: 128 → 256 条目 (TRACE_BUFFER_SIZE)
```

### 2.2 Stats 模块解耦

```
当前: KERN_TASK_STATS 逻辑嵌入在 scheduler.c 的多个 #if 块中
      → 每次上下文切换更新 tick counters
      → task_stats_t 定义在 kernel_types.h

Phase 7:
  src/kernel/stats/stats.c/h  — 独立模块
    - stats_task_switch(tcb_t *prev, tcb_t *next)  — 从 scheduler 调用
    - stats_get(task_id) → task_stats_t
    - stats_get_uptime() → uint32_t
    - stats_get_ctx_switches() → uint32_t
    - stats_get_irq_latency_max() → uint32_t
```

### 2.3 Shell 命令扩展

| 命令 | Phase 6 | Phase 7 |
|------|---------|---------|
| `crash` | 无 | **新增**: 解码 crash_dump_t, 显示 fault type/PC/寄存器 |
| `mem` | 无 | **新增**: 显示内存布局 (heap 使用、mempool 统计) |
| `stats` | 无 | **新增**: 详细统计 (uptime, ctx switches, IRQ 延迟) |
| `trace` | 基本列表 | **增强**: `trace [event]` 按类型过滤; `trace clear` |
| `ps` | 基本列表 | **增强**: 显示 state 字符串, stack 使用率 |

### 2.4 Host 分析工具

```
tools/
├── myrtos_tools.py        # 共享: ELF 读取, section 定位
├── trace_parser.py        # 解析 trace buffer → 人类可读
└── crash_analyzer.py      # 解析 crash dump → 诊断报告
```

**工作原理:**
- my-rtos 链接脚本将 `.trace_buffer` 和 `.crash_dump` 放入已知 section
- Python 脚本使用 `pyelftools` 读取 ELF, 定位这些 section
- 解码二进制数据 → 格式化输出

---

## 三、文件变更

| 操作 | 文件 | 说明 |
|------|------|------|
| **修改** | `src/kernel/trace/trace.h` | 新增 TRACE_IPC_SEND/RECV, TRACE_BH, TRACE_FAULT; buffer 扩至 256 |
| **修改** | `src/kernel/trace/trace.c` | trace_clear(), trace_filter() |
| **新增** | `src/kernel/stats/stats.c` | Stats 模块实现 |
| **新增** | `src/kernel/stats/stats.h` | Stats API |
| **修改** | `src/kernel/core/scheduler.c` | 调用 stats_task_switch(), 移除 inline stats 逻辑 |
| **修改** | `src/kernel/irq/irq.c` | 嵌入 TRACE_ISR_ENTER/EXIT |
| **修改** | `src/kernel/irq/bh.c` | 嵌入 TRACE_BH_SCHEDULE |
| **修改** | `src/kernel/fault/fault.c` | 嵌入 TRACE_FAULT |
| **修改** | `src/kernel/ipc/endpoint.c` | 嵌入 TRACE_IPC_SEND/RECV |
| **修改** | `src/kernel/ipc/channel.c` | 嵌入 TRACE_IPC_SEND/RECV |
| **修改** | `src/app/shell.c` | 新增 crash/mem/stats 命令; 增强 trace/ps |
| **修改** | `src/kernel/include/kernel_types.h` | 扩展 task_stats_t |
| **新增** | `tools/myrtos_tools.py` | ELF 读取工具库 |
| **新增** | `tools/trace_parser.py` | Trace buffer 解析器 |
| **新增** | `tools/crash_analyzer.py` | Crash dump 分析器 |
| **新增** | `src/tests/test_trace.c` | Trace 功能测试 |
| **新增** | `src/tests/test_diag.c` | Shell 命令测试 (crash/mem/stats) |
| **修改** | `src/tests/test_stats.c` | 扩展 stats 验证 |
| **新增** | `docs/DIAGNOSTIC_GUIDE.md` | 诊断工具使用指南 |

---

## 四、数据结构设计

### 4.1 扩展 trace_entry_t

```c
// 新增事件类型
#define TRACE_TASK_SWITCH   0
#define TRACE_ISR_ENTER     1
#define TRACE_ISR_EXIT      2
#define TRACE_SYSCALL       3
#define TRACE_IPC_SEND      4   // ★ 新增
#define TRACE_IPC_RECV      5   // ★ 新增
#define TRACE_BH_SCHEDULE   6   // ★ 新增
#define TRACE_FAULT         7   // ★ 新增

// data 字段含义 (per event):
// TASK_SWITCH: 0
// ISR_ENTER:   irq_num
// ISR_EXIT:    irq_num
// SYSCALL:     syscall_num
// IPC_SEND:    endpoint/channel id
// IPC_RECV:    endpoint/channel id
// BH_SCHEDULE: bh_id
// FAULT:       fault_type (1=MemManage, 2=Bus, 3=Usage, 4=Hard)
```

### 4.2 task_stats_t 扩展

```c
typedef struct {
    uint32_t cpu_ticks;          // 已有
    uint32_t last_tick;          // 已有
    uint32_t ctx_switches;       // ★ 新增: 上下文切换次数
    uint32_t irq_count;          // ★ 新增: IRQ 触发次数
    uint32_t irq_latency_max;    // ★ 新增: 最大 IRQ 延迟 (ticks)
    uint32_t syscall_count;      // ★ 新增: syscall 调用次数
    uint32_t stack_peak;         // ★ 新增: 栈使用峰值
} task_stats_t;
```

### 4.3 Kernel 全局统计

```c
typedef struct {
    uint32_t uptime_ticks;       // 系统运行总 ticks
    uint32_t total_ctx_switches; // 总上下文切换次数
    uint32_t total_irqs;         // 总 IRQ 次数
    uint32_t irq_latency_max;    // 全局最大 IRQ 延迟
    uint32_t fault_count;        // fault 次数
} kern_stats_t;
```

---

## 五、Shell 命令设计

### 5.1 `crash` 命令

```
my-rtos> crash
=== Last Crash Dump ===
Fault Type:  MemManage (1)
Task ID:     2
PC:          0x08004A12
LR:          0x08004B00
R0-R3:       0xBBBBBBBB 0x00000001 0x20001000 0x00000000
R12:         0x00000000
xPSR:        0x61000000
CFSR:        0x00000082 (MMFSR=DACCVIOL)
HFSR:        0x00000000
MMFAR:       0xBBBBBBBB
BFAR:        0xE000ED04
CONTROL:     0x00000003 (nPRIV=1, SPSEL=1)
MPU_CTRL:    0x00000005 (ENABLE=1, PRIVDEFENA=1)
```

### 5.2 `mem` 命令

```
my-rtos> mem
=== Memory ===
Heap:    12345 / 65536 bytes (18.8%)
Mempool: 8/32 objects used
Stack:   task[0]=120/1024  task[1]=88/1024  task[2]=256/1024*
         (* = high watermark)
```

### 5.3 `stats` 命令

```
my-rtos> stats
=== Kernel Statistics ===
Uptime:          1234567 ticks
Context Switches: 89012
IRQs:            45678
Max IRQ Latency: 23 ticks
Faults:          0
Syscalls:        12345
```

### 5.4 `trace` 增强

```
my-rtos> trace          # 显示所有 256 条
my-rtos> trace syscall  # 仅显示 syscall 事件
my-rtos> trace fault    # 仅显示 fault 事件
my-rtos> trace clear    # 清空 buffer
```

---

## 六、Trace 接入点

| 位置 | 事件 | 时机 |
|------|------|------|
| `irq.c:ISR_Dispatcher` | ISR_ENTER | ISR 入口 (在用户回调前) |
| `irq.c:ISR_Dispatcher` | ISR_EXIT | ISR 出口 (在用户回调后) |
| `bh.c:bh_schedule` | BH_SCHEDULE | BH 加入队列 |
| `fault.c:fault_handler_c` | FAULT | fault 入口 (第一时间) |
| `endpoint.c:ep_send` | IPC_SEND | 端点发送 |
| `endpoint.c:ep_recv` | IPC_RECV | 端点接收 |
| `channel.c:ch_send` | IPC_SEND | 通道发送 |
| `channel.c:ch_recv` | IPC_RECV | 通道接收 |
| `scheduler.c` (已有) | TASK_SWITCH | 上下文切换 |
| `syscall.c` (已有) | SYSCALL | syscall 入口 |

---

## 七、测试计划

| 测试模块 | 测试数 (估) | 关键验证点 |
|----------|------------|-----------|
| `test_trace` | 8-10 | buffer wrap, 所有事件类型, trace_get_entry, trace_clear |
| `test_stats` (扩展) | 5-8 | CPU ticks 单调递增, ctx_switches ≥ 0, irq_count ≥ 0 |
| `test_diag` | 6-8 | crash 命令输出格式, mem 命令数据正确, stats 命令 |
| 回归 | 521→~550 | 所有现有测试必须通过 |

---

## 八、风险

| 风险 | 缓解 |
|------|------|
| Trace buffer 增大 (256×8B=2KB) 消耗 SRAM | 编译时可配置 TRACE_BUFFER_SIZE |
| Stats 模块解耦引入 bug | 分步做: 先加模块, 再移逻辑, 每步验证 |
| Python 工具依赖 pyelftools | 提供 pip install 说明; 备选: 纯二进制解析 |
| IRQ 中 trace_record 开销 | ISR trace 可通过 Kconfig 禁用; 默认开启 |
