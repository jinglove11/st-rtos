# Phase 2 完成报告：能力系统 + 内存管理 + 故障处理

## 概述

Phase 2 为 my-rtos 添加了三个核心子系统：
- **能力系统 (Capability)** — 基于令牌的访问控制
- **内存管理** — kmalloc / kfree / mempool
- **故障处理** — HardFault / MemManage / BusFault / UsageFault 诊断

所有 224 个测试通过，0 警告编译。

---

## 新增文件

| 文件 | 用途 |
|------|------|
| `src/kernel/cap/capability.h` | 能力系统 API |
| `src/kernel/cap/capability.c` | 令牌池 + 权限校验 |
| `src/kernel/mem/mem.h` | 内存管理 API |
| `src/kernel/mem/mem.c` | kmalloc / kfree |
| `src/kernel/mem/mempool.h` | 内存池 API |
| `src/kernel/mem/mempool.c` | 固定大小内存池 |
| `src/kernel/fault/fault.h` | 故障处理 API |
| `src/kernel/fault/fault.c` | HardFault / MemManage / BusFault / UsageFault |
| `src/tests/test_capability.c` | 能力系统测试 (15 项) |
| `src/tests/test_fault.c` | 故障处理测试 |

## 修改文件

| 文件 | 变更 |
|------|------|
| `src/kernel/include/kernel_types.h` | 添加 `cap_id_t`, `cap_entry_t`, `BLOCK_REASON_IRQ` 等类型 |
| `src/kernel/syscall/syscall.h` | `syscall_fn_t` 返回类型 `kern_err_t` → `int` |
| `src/kernel/syscall/syscall.c` | IPC syscall handler 集成能力解析；所有 handler 返回 `int` |
| `src/kernel/syscall/user_api.h` | 用户态 syscall 封装 |
| `src/kernel/kernel.c` | 初始化能力/内存/故障模块 |
| `src/kernel/include/kernel.h` | 包含新头文件 |
| `src/tests/test_syscall.c` | syscall + 能力集成测试 |
| `Kconfig` / `.config` | Phase 2 配置项 |
| `Makefile` | 新源文件编译 |

---

## 遇到的重大问题及解决方案

### 问题 1：SVC 返回值被截断（最关键 bug）

**症状**：
- `sys_call2(SYSCALL_SEM_CREATE, 1, 0)` 返回 `0x00000006`
- 内核实际创建的 cap token 是 `0x00005A06`
- 通过 SVC POST 路径传入正确的 token 时工作正常，但 CREATE 返回路径异常

**调试过程**：
1. 在内核 `sys_sem_create` 和 `sys_sem_post` 中添加 debug 打印，确认内核侧 token 正确（0x5A06）
2. 反汇编 `test_syscall_module` 用户代码，发现 `sxtb r0, r0` 指令
3. 反汇编 `sys_sem_create` 内核代码，确认返回路径同样有 `sxtb r0, r5`
4. 定位到编译器优化：所有 syscall handler 返回类型为 `kern_err_t`（小型 enum），GCC -O2 将返回值截断为单字节

**根因**：
```c
// 问题代码
typedef kern_err_t (*syscall_fn_t)(...);
kern_err_t kern_syscall_handler(...);

static kern_err_t sys_sem_create(...) {
    cap_id_t cap = cap_create(...);
    return (kern_err_t)cap;   // ← 编译器生成 sxtb：16-bit cap → 8-bit
}
```

`kern_err_t` 是 enum，值范围约 -12 ~ 0，编译器认为返回值只需 8 位有效。但 cap token 是 16-bit（1-32767），强制转换为 `kern_err_t` 后高 8 位被丢弃。

**解决方案**：
```c
// 修复后
typedef int (*syscall_fn_t)(...);
int kern_syscall_handler(...);

static int sys_sem_create(...) {
    cap_id_t cap = cap_create(...);
    return (int)cap;   // ← 完整 32-bit 传递，无截断
}
```

**涉及更改**：
- `syscall.h`: `syscall_fn_t` 返回类型改为 `int`
- `syscall.h`: `kern_syscall_handler` 返回类型改为 `int`
- `syscall.c`: 所有 `sys_*` handler 返回 `int`
- `syscall.c`: 移除 return 语句中的 `(kern_err_t)` 强制转换
- `test_syscall.c`: 测试代码用 `int` 接收 cap token（避免用户侧同样被 sxtb）

---

### 问题 2：cap_resolve 拒绝 NULL 对象

**症状**：`cap_create(NULL, ...)` 返回 -1（CAP_INVALID），导致 IPC ID 0 的原始对象无法创建能力。

**根因**：能力系统最初设计不允许 NULL 对象。但 IPC ID +1 偏移方案中，当 raw_id=0 时，存储的对象指针为 `(void*)(0+1) = (void*)1`，而非 NULL。

**解决方案**：允许 `cap_create(NULL, ...)` 创建有效 token（用于 IPC ID 0 场景）。

---

### 问题 3：cap_id_t 负值 token

**症状**：cap token 在用户侧显示为负数（如 `0xFFFFFFC7`），导致 `cap >= 0` 断言失败。

**根因**：LCG token 生成器的种子掩码为 `0xFFFF`（16-bit 全范围），产出的 token 可能 >= 0x8000，作为 `int16_t` 解释时为负数。

**解决方案**：
```c
uint16_t token = cap_token_seed & 0x7FFF;  // 限制到 1..32767
if (token == 0) token = 1;                  // 0 保留为无效
```

---

### 问题 4：能力池耗尽导致 cap_create 失败

**症状**：capability 测试模块的 "fill test" 大量失败。

**根因**：syscall 测试中，Test 7 (R2 Integrity) 使用 `kern_err_t` 接收 SEM_CREATE 的返回值（cap token），被 sxtb 截断后，cleanup 路径用错误的 token 调用 `sys_call1(SEM_DELETE, ...)`，导致 cap_delete 无效、能力泄漏。32 个槽位被耗尽。

**解决方案**：修复问题 1 后自动修复——Test 7 改用 `int` 接收返回值，cleanup 正确释放能力。

---

### 问题 5：UART 串口输出乱码

**症状**：测试输出大量字符丢失/乱码，`[FAIL]` 标记难以识别。

**根因**：
- 板载 STM32F767 采用 HSI 16MHz，UART BRR 计算后实际波特率约 115,942（误差 0.64%）
- 测试框架无 UART 输出锁，多任务并发输出时存在竞争
- kernel debug print 在 SVC handler 内调用，影响调度时序

**缓解措施**：
- Kernel debug print 在验证后移除
- UART 传输使用轮询模式，确保 TXE 等待
- 多次采样取交集确认测试结果

---

## 当前状态

```
编译：0 warnings
测试：224 passed / 0 failed / 224 total
所有模块：scheduler / timer / deadlock / irq / mpu / syscall / capability / fault
```

## 性能指标

| 指标 | 值 |
|------|-----|
| FLASH 占用 | 37,528 bytes (1.79% of 2MB) |
| SRAM 占用 | 31,440 bytes (8.00% of 384KB) |
| 能力池大小 | 32 entries (CAP_MAX_COUNT) |
| Token 范围 | 1-32767 (15-bit, LCG 生成) |
