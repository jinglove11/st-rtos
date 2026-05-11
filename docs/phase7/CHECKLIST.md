# Phase 7: Diagnostic Ecosystem — 任务清单

> 版本: 1.0 | 日期: 2026-05-09 | 起始: 521 tests, 0 failures

---

## Step 1: Extended Trace Events (trace 事件扩展)

### 1.1 新增 trace 事件类型

- [ ] 1.1.1 `trace.h`: 新增 `TRACE_IPC_SEND (4)`, `TRACE_IPC_RECV (5)`, `TRACE_BH_SCHEDULE (6)`, `TRACE_FAULT (7)`
- [ ] 1.1.2 `trace.h`: TRACE_BUFFER_SIZE 从 128 扩展到 256
- [ ] 1.1.3 `trace.h`: 更新 trace_entry_t 注释 (data 字段含义 per event)

### 1.2 Trace API 扩展

- [ ] 1.2.1 `trace.c`: 实现 `trace_clear()` — 重置 head/count 为 0
- [ ] 1.2.2 `trace.c`: 实现 `trace_filter(event, callback)` — 遍历 buffer 调用 callback
- [ ] 1.2.3 `trace.h`: 导出 `trace_clear()`, `trace_filter()` 声明

### 1.3 接入 ISR trace (irq.c)

- [ ] 1.3.1 `irq.c`: ISR 入口处添加 `trace_record(TRACE_ISR_ENTER, 0, irq_num)`
- [ ] 1.3.2 `irq.c`: ISR 出口处添加 `trace_record(TRACE_ISR_EXIT, 0, irq_num)`
- [ ] 1.3.3 验证: ISR trace 不破坏中断延迟 (trace_record < 10 instructions)

### 1.4 接入 BH trace (bh.c)

- [ ] 1.4.1 `bh.c`: `bh_schedule()` 中添加 `trace_record(TRACE_BH_SCHEDULE, 0, bh_id)`

### 1.5 接入 Fault trace (fault.c)

- [ ] 1.5.1 `fault.c`: `fault_handler_c()` 开头添加 `trace_record(TRACE_FAULT, task_id, fault_type)`
- [ ] 1.5.2 确保 trace_record 在 crash_dump 填充之前调用 (不影响 crash_dump 时序)

### 1.6 接入 IPC trace

- [ ] 1.6.1 `endpoint.c`: `ep_send()` 中添加 `trace_record(TRACE_IPC_SEND, task_id, ep_id)`
- [ ] 1.6.2 `endpoint.c`: `ep_recv()` 中添加 `trace_record(TRACE_IPC_RECV, task_id, ep_id)`
- [ ] 1.6.3 `channel.c`: `ch_send()` 中添加 `trace_record(TRACE_IPC_SEND, task_id, ch_id)`
- [ ] 1.6.4 `channel.c`: `ch_recv()` 中添加 `trace_record(TRACE_IPC_RECV, task_id, ch_id)`

---

## Step 2: Dedicated Stats Module (独立 stats 模块)

### 2.1 新建 stats 模块

- [ ] 2.1.1 `src/kernel/stats/stats.h`: 定义 `kern_stats_t`, 扩展 `task_stats_t` (ctx_switches, irq_count, irq_latency_max, syscall_count)
- [ ] 2.1.2 `src/kernel/stats/stats.c`: 实现 `stats_init()`, `stats_task_switch()`, `stats_get()`, `stats_get_uptime()`, `stats_get_ctx_switches()`
- [ ] 2.1.3 `Makefile`: 添加 `src/kernel/stats/stats.o`

### 2.2 从 scheduler.c 解耦

- [ ] 2.2.1 `scheduler.c`: 将 `#if KERN_TASK_STATS` 块替换为 `stats_task_switch(prev, next)` 调用
- [ ] 2.2.2 `scheduler.c`: 将 `sched_get_task_stats()` 实现移到 `stats.c`
- [ ] 2.2.3 `scheduler.h`: 保留 `sched_get_task_stats()` 声明 (兼容), 内部调用 stats 模块
- [ ] 2.2.4 验证: 编译通过, 现有 test_stats 全部通过

### 2.3 IRQ 统计

- [ ] 2.3.1 `irq.c`: ISR 入口记录 `irq_enter_tick`, ISR 出口计算 `irq_latency = now - irq_enter_tick`
- [ ] 2.3.2 `irq.c`: 更新 `stats.irq_count` 和 `stats.irq_latency_max`
- [ ] 2.3.3 `stats.c`: 实现 `stats_record_irq(latency)` 和 `stats_get_irq_latency_max()`

### 2.4 Syscall 统计

- [ ] 2.4.1 `syscall.c`: 调用 `stats_record_syscall(task_id)`
- [ ] 2.4.2 `stats.c`: 实现 `stats_record_syscall()` — 更新 per-task syscall_count

### 2.5 Stack 水位统计

- [ ] 2.5.1 `stats.c`: 实现 `stats_update_stack_peak(tcb_t *tcb)` — 扫描 stack magic 计算使用峰值
- [ ] 2.5.2 `scheduler.c`: 上下文切换时调用 `stats_update_stack_peak()`

---

## Step 3: Shell Diagnostic Commands (Shell 诊断命令)

### 3.1 crash 命令

- [ ] 3.1.1 `shell.c`: 实现 `cmd_crash()` — 读取 crash_dump 全局变量并格式化输出
- [ ] 3.1.2 输出 fault_type 名称 (MemManage/Bus/Usage/Hard)
- [ ] 3.1.3 输出 PC, LR, R0-R3, R12, xPSR
- [ ] 3.1.4 输出 CFSR 分解 (MMFSR/BFSR/UFSR 字节)
- [ ] 3.1.5 输出 MMFAR, BFAR, CONTROL, MPU_CTRL
- [ ] 3.1.6 注册到 cmd_table: `{ "crash", "Last crash dump", cmd_crash }`

### 3.2 stats 命令

- [ ] 3.2.1 `shell.c`: 实现 `cmd_stats()` — 显示 kern_stats_t 全局统计
- [ ] 3.2.2 输出: uptime, ctx switches, IRQs, max IRQ latency, faults, syscalls
- [ ] 3.2.3 注册到 cmd_table: `{ "stats", "Kernel statistics", cmd_stats }`

### 3.3 mem 命令

- [ ] 3.3.1 `shell.c`: 实现 `cmd_mem()` — 显示 heap 使用率、mempool 统计
- [ ] 3.3.2 输出每任务栈使用率 (current / total, 高水位标记 * 表示 >80%)
- [ ] 3.3.3 注册到 cmd_table: `{ "mem", "Memory layout", cmd_mem }`

### 3.4 trace 命令增强

- [ ] 3.4.1 `shell.c`: `cmd_trace` 支持 `trace syscall` 过滤
- [ ] 3.4.2 `shell.c`: `cmd_trace` 支持 `trace fault` 过滤
- [ ] 3.4.3 `shell.c`: `cmd_trace` 支持 `trace isr` 过滤
- [ ] 3.4.4 `shell.c`: `cmd_trace` 支持 `trace clear` 清空
- [ ] 3.4.5 事件类型字符串表: `[SW] [ISR+] [ISR-] [SVC] [IPC+] [IPC-] [BH] [FLT]`

### 3.5 ps 命令增强

- [ ] 3.5.1 `shell.c`: `cmd_ps` 显示 state 字符串 (READY/RUNNING/BLOCKED/SUSPENDED/TERMINATED)
- [ ] 3.5.2 `shell.c`: `cmd_ps` 显示 stack 使用率列

---

## Step 4: Host-Side Analysis Tools (PC 端分析工具)

### 4.1 myrtos_tools.py — 共享库

- [ ] 4.1.1 `tools/myrtos_tools.py`: ELF 读取 (pyelftools), section 定位
- [ ] 4.1.2 `tools/myrtos_tools.py`: `find_section(elf, name)` → section data
- [ ] 4.1.3 `tools/myrtos_tools.py`: `read_uint32(data, offset)` 辅助
- [ ] 4.1.4 `tools/myrtos_tools.py`: 符号查找 (通过 .symtab)

### 4.2 trace_parser.py — Trace 解析器

- [ ] 4.2.1 `tools/trace_parser.py`: 读取 ELF → 定位 .trace_buffer section (或 .bss 中的 trace_buf)
- [ ] 4.2.2 `tools/trace_parser.py`: 解析 trace_entry_t[] → 时间排序
- [ ] 4.2.3 `tools/trace_parser.py`: 输出格式: `[tick] [event] task=X data=Y`
- [ ] 4.2.4 `tools/trace_parser.py`: 支持 `--filter EVENT` 命令行参数
- [ ] 4.2.5 `tools/trace_parser.py`: 支持 `--output csv` 导出

### 4.3 crash_analyzer.py — Crash 分析器

- [ ] 4.3.1 `tools/crash_analyzer.py`: 读取 ELF → 定位 .crash_dump section
- [ ] 4.3.2 `tools/crash_analyzer.py`: 解码 crash_dump_t → 诊断报告
- [ ] 4.3.3 `tools/crash_analyzer.py`: 符号解析 (PC → 函数名 via addr2line 或 .symtab)
- [ ] 4.3.4 `tools/crash_analyzer.py`: CFSR 位解码 (具体原因: DACCVIOL, IACCVIOL, UNDEFINSTR...)

---

## Step 5: Test Suite (测试套件)

### 5.1 test_trace.c

- [ ] 5.1.1 测试: trace_record 写入 → trace_get_entry 读取一致性
- [ ] 5.1.2 测试: buffer 满了以后 wrap-around 正确
- [ ] 5.1.3 测试: trace_clear 清零
- [ ] 5.1.4 测试: trace_get_count 在写入后递增
- [ ] 5.1.5 测试: 所有 8 种事件类型可记录
- [ ] 5.1.6 `Makefile`: 添加 `test_trace.o`
- [ ] 5.1.7 `test_trace.c`: TEST_MODULE_REGISTER

### 5.2 test_stats.c 扩展

- [ ] 5.2.1 测试: CPU ticks 单调递增 (跨多次 yield)
- [ ] 5.2.2 测试: ctx_switches 在 yield 后增加
- [ ] 5.2.3 测试: uptime > 0 在调度启动后
- [ ] 5.2.4 测试: stack_peak 在任务使用栈后 > 0

### 5.3 test_diag.c — Shell 诊断命令测试

- [ ] 5.3.1 测试: crash 命令在无 crash 时显示 "No crash"
- [ ] 5.3.2 测试: stats 命令返回有效数据
- [ ] 5.3.3 测试: mem 命令显示 heap 信息
- [ ] 5.3.4 测试: trace 命令过滤功能
- [ ] 5.3.5 测试: ps 命令显示 state 字符串
- [ ] 5.3.6 `Makefile`: 添加 `test_diag.o`
- [ ] 5.3.7 `test_diag.c`: TEST_MODULE_REGISTER

### 5.4 全量回归

- [ ] 5.4.1 编译: `make BOARD=stm32f767` — 0 warnings
- [ ] 5.4.2 全部现有 521 tests 通过
- [ ] 5.4.3 全部新增 tests 通过 (预计 +20~25)
- [ ] 5.4.4 硬件验证: 烧录 STM32F767 运行全量测试

---

## Step 6: Documentation (文档)

### 6.1 诊断指南

- [ ] 6.1.1 `docs/DIAGNOSTIC_GUIDE.md`: Trace 使用方法 (shell trace 命令 + 离线解析)
- [ ] 6.1.2 `docs/DIAGNOSTIC_GUIDE.md`: Crash dump 分析方法 (shell crash 命令 + crash_analyzer.py)
- [ ] 6.1.3 `docs/DIAGNOSTIC_GUIDE.md`: Stats 解读 (CPU usage, IRQ latency 含义)
- [ ] 6.1.4 `docs/DIAGNOSTIC_GUIDE.md`: 常见故障诊断流程 (栈溢出, MPU 违规, 死锁)

### 6.2 Phase 7 完成报告

- [ ] 6.2.1 `docs/phase7/完成报告.md`: 记录实现细节、遇到的 bug、修复记录
- [ ] 6.2.2 `docs/phase7/功能完成表.md`: 每项功能确认完成状态

---

## 统计

| Step | 任务数 | 说明 |
|------|--------|------|
| Step 1 (Extended Trace) | 14 | 新事件类型 + API + 接入点 |
| Step 2 (Stats Module) | 13 | 独立模块 + 解耦 + 新统计 |
| Step 3 (Shell Commands) | 15 | crash/mem/stats 命令 + trace/ps 增强 |
| Step 4 (Host Tools) | 13 | 3 个 Python 脚本 |
| Step 5 (Test Suite) | 17 | 新测试 + 扩展 + 回归 |
| Step 6 (Documentation) | 6 | 诊断指南 + 完成报告 |
| **Total** | **78** | |

---

## 验证标准

- [ ] `make BOARD=stm32f767` 0 warnings
- [ ] 全量测试通过 (~545 tests, 0 failures)
- [ ] `tools/trace_parser.py build/stm32f767/my-rtos-stm32f767.elf` 正常运行
- [ ] `tools/crash_analyzer.py build/stm32f767/my-rtos-stm32f767.elf` 正常运行
- [ ] Shell `crash`/`mem`/`stats` 命令硬件验证通过
