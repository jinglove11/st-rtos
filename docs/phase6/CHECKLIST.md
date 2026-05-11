# Phase 6: 异常容错 + 诊断增强 — 完成表

> 日期: 2026-05-08
> 设计文档: `docs/phase6/DESIGN.md`
> 基线: 489 tests, 0 failures
> 目标: ≥510 tests, 0 failures
> 实际: 521 tests, 0 failures ✅

---

## Step 1: Fault 注入测试

| # | 任务 | 文件 | 状态 |
|---|------|------|------|
| 1.1 | test_fault.c — MemManage: 用户任务写 NULL 区域 → 任务终止 | `src/tests/test_fault.c` | ✅ |
| 1.2 | test_fault.c — UsageFault: 用户任务除零 → 任务终止 | `src/tests/test_fault.c` | ✅ |
| 1.3 | test_fault.c — crash_dump: fault 后字段验证 (pc, fault_type, psp) | `src/tests/test_fault.c` | ✅ |
| 1.4 | test_fault.c — 内核存活: 用户 fault 后内核可继续调度 | `src/tests/test_fault.c` | ✅ |
| 1.5 | fault.c — task_terminate 唤醒 joiners (bug fix) | `src/kernel/fault/fault.c` | ✅ |
| 1.6 | fault.h — crash_dump 从 static 改为 extern | `src/kernel/fault/fault.h` | ✅ |
| 1.7 | startup.S — FAULT_ENTRY 修复: push/pop lr 保存 EXC_RETURN | `src/startup/arm/startup.S` | ✅ |
| 1.8 | mpu.c — 使能 BusFault/UsageFault handler (防止升级为 HardFault) | `src/kernel/mpu/mpu.c` | ✅ |
| 1.9 | 编译验证 0 warnings | — | ✅ |
| 1.10 | 回归测试全部通过 (521 tests) | — | ✅ |

---

## Step 2: 看门狗实现

| # | 任务 | 文件 | 状态 |
|---|------|------|------|
| 2.1 | `hal.h` — 声明 `hal_watchdog_init/feed/reset_cause` | `src/hal/hal.h` | ✅ |
| 2.2 | `hal.c` — 实现 IWDG 寄存器操作 (KR/PR/RLR) | `src/arch/arm/cortex-m7/hal.c` | ✅ |
| 2.3 | `hal.c` — `hal_watchdog_init(timeout_ms)` — 计算分频+重载 | `src/arch/arm/cortex-m7/hal.c` | ✅ |
| 2.4 | `hal.c` — `hal_watchdog_feed()` — 写 0xAAAA | `src/arch/arm/cortex-m7/hal.c` | ✅ |
| 2.5 | `hal.c` — `hal_watchdog_reset_cause()` — 读 RCC_CSR | `src/arch/arm/cortex-m7/hal.c` | ✅ |
| 2.6 | `kernel.c` — kern_init 调用 watchdog_init (当 KERN_WATCHDOG_ENABLE) | `src/kernel/kernel.c` | ✅ |
| 2.7 | 新增 `test_watchdog.c` — IWDG 寄存器可访问 | `src/tests/test_watchdog.c` | ✅ |
| 2.8 | test_watchdog.c — feed 后 RVR 值正确 | `src/tests/test_watchdog.c` | ✅ |
| 2.9 | test_watchdog.c — init 后 SR 状态正确 | `src/tests/test_watchdog.c` | ✅ |
| 2.10 | Makefile 添加 test_watchdog.c | `Makefile` | ✅ |
| 2.11 | Kconfig 添加 TEST_MODULE_WATCHDOG | `Kconfig` | ✅ |
| 2.12 | 编译验证 0 warnings | — | ✅ |
| 2.13 | 回归测试全部通过 (521 tests) | — | ✅ |

---

## Step 3: CPU 使用率统计

| # | 任务 | 文件 | 状态 |
|---|------|------|------|
| 3.1 | `scheduler.c` — 更新 `sched_update_stats()` 实现 CPU 使用率计算 | `src/kernel/core/scheduler.c` | ✅ |
| 3.2 | `scheduler.c` — tick 中断调用 `sched_update_stats()` | `src/kernel/core/scheduler.c` | ✅ |
| 3.3 | `scheduler.h` — 声明 `sched_update_stats()` | `src/kernel/core/scheduler.h` | ✅ |
| 3.4 | `shell.c` — `top` 命令: 任务列表 + CPU% 列 | `src/app/shell.c` | ✅ |
| 3.5 | 新增 `test_stats.c` — CPU 统计基本功能 | `src/tests/test_stats.c` | ✅ |
| 3.6 | test_stats.c — 多任务竞争后 cpu_usage 总和 ≈ 100% | `src/tests/test_stats.c` | ✅ |
| 3.7 | test_stats.c — idle 任务 cpu_usage 合理 | `src/tests/test_stats.c` | ✅ |
| 3.8 | Makefile 添加 test_stats.c | `Makefile` | ✅ |
| 3.9 | Kconfig 添加 TEST_MODULE_STATS | `Kconfig` | ✅ |
| 3.10 | 编译验证 0 warnings | — | ✅ |
| 3.11 | 回归测试全部通过 (521 tests) | — | ✅ |

---

## Step 4: Trace 系统 + Shell 增强

| # | 任务 | 文件 | 状态 |
|---|------|------|------|
| 4.1 | 新增 `trace.h` — trace_entry_t + trace_event_t + API 声明 | `src/kernel/trace/trace.h` | ✅ |
| 4.2 | 新增 `trace.c` — 环形 buffer + `trace_record()` | `src/kernel/trace/trace.c` | ✅ |
| 4.3 | `scheduler.c` — task switch 埋 trace 点 | `src/kernel/core/scheduler.c` | ✅ |
| 4.4 | `syscall.c` — syscall 入口埋 trace 点 | `src/kernel/syscall/syscall.c` | ✅ |
| 4.5 | `shell.c` — `trace` 命令: 打印最近 N 条事件 | `src/app/shell.c` | ✅ |
| 4.6 | `shell.c` — `reset` 命令: 软件复位 | `src/app/shell.c` | ✅ |
| 4.7 | `shell.c` — `version` 命令: 内核版本 | `src/app/shell.c` | ✅ |
| 4.8 | Makefile 添加 trace.c | `Makefile` | ✅ |
| 4.9 | Kconfig 添加 TRACE_ENABLE, TRACE_BUFFER_SIZE | `Kconfig` | ✅ |
| 4.10 | `kernel_config.h` — 更新默认值 | `src/kernel/include/kernel_config.h` | ✅ |
| 4.11 | 编译验证 0 warnings | — | ✅ |
| 4.12 | 回归测试全部通过 (521 tests) | — | ✅ |

---

## Step 5: 收尾

| # | 任务 | 文件 | 状态 |
|---|------|------|------|
| 5.1 | test_capability.c — 无权限操作返回 KERN_ERR_CAP | `src/tests/test_capability.c` | ✅ |
| 5.2 | defconfig 同步 — 更新 `configs/stm32f767_defconfig` | `configs/stm32f767_defconfig` | ✅ |
| 5.3 | `make BOARD=stm32f767 -j8` → 0 warnings | — | ✅ |
| 5.4 | 全部测试通过 | — | ✅ |
| 5.5 | 总测试数 ≥ 510 (521 tests) | — | ✅ |

---

## 文件变更汇总

| 操作 | 文件数 | 文件列表 |
|------|--------|----------|
| 新增 | 4 | `trace.c`, `trace.h`, `test_watchdog.c`, `test_stats.c` |
| 修改 | 15 | `test_fault.c`, `fault.c`, `fault.h`, `hal.c`, `hal.h`, `kernel.c`, `scheduler.c`, `scheduler.h`, `syscall.c`, `shell.c`, `test_capability.c`, `kernel_config.h`, `Makefile`, `startup.S`, `mpu.c` |
| 配置 | 3 | `Makefile`, `Kconfig`, `configs/stm32f767_defconfig` |
| **合计** | **21** | |

---

## 进度统计

| Step | 总任务数 | 已完成 | 进度 |
|------|----------|--------|------|
| Step 1: Fault 注入测试 | 10 | 10 | 100% |
| Step 2: 看门狗 | 13 | 13 | 100% |
| Step 3: CPU 统计 | 11 | 11 | 100% |
| Step 4: Trace + Shell | 12 | 12 | 100% |
| Step 5: 收尾 | 5 | 5 | 100% |
| **总计** | **51** | **51** | **100%** |
