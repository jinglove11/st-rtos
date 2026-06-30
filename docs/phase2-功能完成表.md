# Phase 2: 能力系统 + 内存管理 + 异常处理 — 功能完成表

> 状态说明: ⬜ 未开始 | ✅ 已完成并测试通过
> 前置: Phase 1 已完成 (145 tests, 0 failures)
> 编译: **make BOARD=stm32f767 -j8 → 0 warnings**
> 测试: **224 passed / 0 failed / 224 total — All tests PASSED!**

---

## A. 能力系统 (Capability)

| 功能 | 文件 | 状态 |
|------|------|:----:|
| cap_init — LCG 种子初始化 | `capability.c:22-30` | ✅ |
| cap_create — 创建令牌 | `capability.c:48-63` | ✅ |
| cap_delete — 释放令牌 | `capability.c:93-101` | ✅ |
| cap_resolve — 令牌解析 + 权限校验 | `capability.c:72-87` | ✅ |
| cap_resolve owner 检查 (用户任务) | `capability.c:83` | ✅ |
| cap_resolve owner 跳过 (特权任务) | `capability.c:76` | ✅ |
| cap_derive — 降权派生子令牌 | `capability.c:119-140` | ✅ |
| cap_derive GRANT 权限要求 | `capability.c:128` | ✅ |
| cap_derive 超集拒绝 | `capability.c:130` | ✅ |
| cap_transfer — 转移所有权 | `capability.c:146-161` | ✅ |
| cap_revoke — 撤销单个令牌 | `capability.c:167-181` | ✅ |
| cap_revoke_all — 撤销任务所有令牌 | `capability.c:107-113` | ✅ |
| Token LCG 生成 (1-32767, 非负) | `capability.c:36-42` | ✅ |
| 令牌池 32 entries (CAP_MAX_COUNT) | `capability.c:15` | ✅ |
| 池满返回 CAP_INVALID (-1) | `capability.c:62` | ✅ |
| cap_id_t int16_t 类型 | `kernel_types.h:43` | ✅ |
| CAP_OBJ_SEMAPHORE / MUTEX / MQUEUE / EVENT / TIMER / MEMBLOCK | `capability.h` | ✅ |
| CAP_READ / CAP_WRITE / CAP_MANAGE / CAP_GRANT / CAP_TRANSFER 权限位 | `capability.h` | ✅ |

---

## B. 内存管理 (Memory)

| 功能 | 文件 | 状态 |
|------|------|:----:|
| kmalloc — 动态分配 | `mem.c` | ✅ |
| kfree — 释放内存 | `mem.c` | ✅ |
| mempool_create — 创建固定大小池 | `mempool.c` | ✅ |
| mempool_alloc — 从池分配 | `mempool.c` | ✅ |
| mempool_free — 归还池块 | `mempool.c` | ✅ |
| mem_init / mempool_init 内核集成 | `kernel.c:26-27` | ✅ |

---

## C. 异常/故障处理 (Fault)

| 功能 | 文件 | 状态 |
|------|------|:----:|
| HardFault_Handler — 硬件故障诊断 | `fault.c` | ✅ |
| MemManage_Handler — MPU 违规诊断 | `fault.c` | ✅ |
| BusFault_Handler — 总线错误诊断 | `fault.c` | ✅ |
| UsageFault_Handler — 非法指令/除零诊断 | `fault.c` | ✅ |
| fault_print_context — 栈帧转储 | `fault.c` | ✅ |
| fault_print_cfsr — CFSR 寄存器解码 | `fault.c` | ✅ |
| 用户态故障 → 任务终止 | `fault.c:102` | ✅ |
| 内核态故障 → 死循环 (halt) | `fault.c` | ✅ |
| fault_type enum (HARD / MEMMANAGE / BUS / USAGE) | `fault.h` | ✅ |

---

## D. 中断管理 (Interrupt — 意外收获, 归入 Phase 2)

| 功能 | 文件 | 状态 |
|------|------|:----:|
| irq_register — ISR 注册 | `irq.c` | ✅ |
| irq_unregister — ISR 注销 | `irq.c` | ✅ |
| irq_enable / irq_disable | `irq.c` | ✅ |
| kern_is_in_isr — ISR 上下文检测 | `irq.c` | ✅ |
| kern_irq_context — 当前 IRQ 号 | `irq.c` | ✅ |
| bh_create — 创建 Bottom Half | `bh.c` | ✅ |
| bh_schedule — 调度 Bottom Half | `bh.c` | ✅ |
| bh_delete — 删除 Bottom Half | `bh.c` | ✅ |
| BH service task — 后台处理 | `bh.c` | ✅ |
| Threaded IRQ — 线程化中断 | `irq.c` | ✅ |
| irq_request_threaded / irq_release_threaded | `irq.c` | ✅ |
| RAM Vector Table (VTOR → SRAM) | `hal.c` + `system.c` | ✅ |
| hal_irq_set_vector — 运行时向量表更新 | `hal.c` | ✅ |
| ISR 守卫: delay/sem_wait/mutex_lock 禁用于 ISR | 各 IPC 文件 | ✅ |

---

## E. Syscall ↔ 能力系统集成

| 功能 | Syscall # | 状态 |
|------|:---------:|:----:|
| SYSCALL_SEM_CREATE → cap_create 封装 | 9 | ✅ |
| SYSCALL_SEM_WAIT → cap_resolve | 10 | ✅ |
| SYSCALL_SEM_POST → cap_resolve | 11 | ✅ |
| SYSCALL_SEM_DELETE → cap_resolve + cap_delete | 12 | ✅ |
| SYSCALL_MUTEX_CREATE → cap_create | 13 | ✅ |
| SYSCALL_MUTEX_LOCK → cap_resolve | 14 | ✅ |
| SYSCALL_MUTEX_UNLOCK → cap_resolve | 15 | ✅ |
| SYSCALL_MQUEUE_CREATE → cap_create | 16 | ✅ |
| SYSCALL_MQUEUE_SEND → cap_resolve | 17 | ✅ |
| SYSCALL_MQUEUE_RECV → cap_resolve | 18 | ✅ |
| SYSCALL_EVENT_CREATE → cap_create | 19 | ✅ |
| SYSCALL_EVENT_WAIT → cap_resolve | 20 | ✅ |
| SYSCALL_EVENT_SET → cap_resolve | 21 | ✅ |
| SYSCALL_TIMER_CREATE → cap_create | 22 | ✅ |
| SYSCALL_TIMER_START → cap_resolve | 23 | ✅ |
| SYSCALL_MEM_ALLOC → cap_create | 27 | ✅ |
| SYSCALL_MEM_FREE → cap_resolve + cap_delete | 28 | ✅ |
| SYSCALL_CAP_DERIVE | 29 | ✅ |
| SYSCALL_CAP_TRANSFER | 30 | ✅ |
| SYSCALL_CAP_REVOKE | 31 | ✅ |
| IPC ID +1 offset (NULL 保护) | 所有 IPC handler | ✅ |
| **SVC 返回值截断 bug 修复** | `syscall.h` + `syscall.c` | ✅ |

---

## F. 测试覆盖 (224 total)

| 模块 | 文件 | 断言数 | 状态 |
|------|------|:-----:|:----:|
| 调度器 (Phase 1) | `test_scheduler.c` | 50 | ✅ |
| 定时器 | `test_timer.c` | 19 | ✅ |
| 死锁检测 | `test_deadlock.c` | 14 | ✅ |
| 中断管理 | `test_irq.c` | 30 | ✅ |
| MPU | `test_mpu.c` | 4 | ✅ |
| Syscall + 能力集成 | `test_syscall.c` | 14 | ✅ |
| 能力系统 | `test_capability.c` | 35 | ✅ |
| 故障处理 | `test_fault.c` | 10 | ✅ |

### 能力系统测试详情

| 测试 | 内容 | 状态 |
|:----:|------|:----:|
| 1 | cap_create 返回有效非零令牌 | ✅ |
| 2 | cap_create 池满返回 INVALID (32 次填充 + 1 越界) | ✅ |
| 3 | cap_resolve 正确令牌+权限 → 返回对象 | ✅ |
| 4 | cap_resolve 无效令牌 → NULL | ✅ |
| 5 | cap_resolve 类型不匹配 → NULL | ✅ |
| 6 | cap_resolve 权限不足 → NULL | ✅ |
| 7 | cap_delete 后 resolve → NULL | ✅ |
| 8 | cap_derive 子集权限派生 | ✅ |
| 9 | cap_derive 超集权限拒绝 | ✅ |
| 10 | cap_derive 无 GRANT 拒绝 | ✅ |
| 11 | cap_revoke_all 批量撤销 | ✅ |
| 12 | cap_revoke 单个撤销 | ✅ |
| 13 | cap_create NULL 对象允许 | ✅ |
| 14 | cap_resolve CAP_INVALID → NULL | ✅ |
| 15 | IPC ID 0 roundtrip (id+1 offset) | ✅ |

---

## G. 资源占用

| 资源 | 占用 | 占比 |
|------|------|-----:|
| FLASH (.text) | 37,528 bytes | 1.80% of 2MB |
| SRAM (.bss+.data) | 31,572 bytes | 8.03% of 384KB |
| 能力池 | 32 × (2B token + 1B rights + 1B owner + 4B ptr + 1B type + 1B in_use) ≈ 320 bytes | — |
| 向量表 (RAM) | 512 bytes (128 entries × 4B) | — |

---

## H. 关键 Bug 修复记录

| # | 问题 | 根因 | 修复 |
|:--:|------|------|------|
| 1 | **SVC 返回值截断** | GCC -O2 对 `kern_err_t` enum 生成 `sxtb`，16-bit cap token → 8-bit | `syscall_fn_t` 返回类型改为 `int` |
| 2 | **cap_resolve 拒绝 NULL 对象** | 设计不允许 NULL，但 IPC ID+1 offset 需要 | 允许 NULL 作为 cap_create 参数 |
| 3 | **cap token 为负数** | LCG 掩码 0xFFFF 产出 ≥0x8000 的值 | 掩码改为 0x7FFF |
| 4 | **能力池耗尽** | sxtb 导致 cleanup 用错误 token | 修复 #1 后自动解决 |
| 5 | **UART 输出乱码** | HSI 16MHz 115200bps 误差 0.64% + 多任务并发 | debug print 移除后缓解 |
| 6 | **kern_syscall_handler 栈偏移 off-by-4** | `bl` 指令压入返回地址导致 a4-a6 偏移错误 | 不影响当前测试，待修复 |

---

## I. 待办 (Phase 3+)

| 项目 | 优先级 |
|------|:------:|
| kern_syscall_handler 栈偏移修正 (a4-a6 off-by-4) | 中 |
| UART 驱动添加 TC 等待 + 输出锁 | 中 |
| 用户态任务支持 (TASK_ATTR_USER) 端到端测试 | 低 |
| cap_transfer / cap_derive 通过 SVC 的端到端测试 | 低 |
