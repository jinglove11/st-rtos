# 中断管理模块设计文档

## 1. 设计目标

- **目的**：提供完整的中断管理体系：ISR 注册/注销、底半部 (Bottom Half)、线程化 IRQ
- **架构**：RAM 向量表 + ISR 描述符池 + 底半部信号量驱动 + 线程化 IRQ 任务调度
- **安全性**：BH 调度 ISR 安全，线程化 IRQ 回调在任务上下文执行，可调用阻塞 API
- **复杂度**：ISR 注册 O(n)，BH 调度 O(1)，线程化 IRQ 分发 O(n)

---

## 2. 架构设计

### 2.1 分层架构

```
┌─────────────────────────────────────────────────────────────┐
│                      用户 API 层                             │
│  irq_register()  irq_unregister()  irq_enable/disable()    │
│  irq_request_threaded()  irq_release_threaded()             │
│  bh_create()  bh_schedule()  bh_delete()                    │
│  kern_is_in_isr()  kern_irq_context()                      │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                      ISR 管理层                              │
│  - ISR 描述符池 (irq_desc_t[IRQ_MAX_USER])                   │
│  - 线程化 IRQ 池 (irq_thread_t[IRQ_THREADED_MAX])           │
│  - BH 池 (bh_t[IRQ_BH_MAX])                                 │
│  - RAM 向量表 (VTOR → SRAM)                                  │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                      硬件抽象层                              │
│  hal_irq_set_vector()  hal_irq_set_priority()               │
│  hal_irq_enable_irq()  hal_irq_disable_irq()               │
│  hal_irq_get_active()  NVIC / SCB 操作                      │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                    服务任务层                                │
│  - BH 服务任务 (优先级 2) — 信号量驱动, 处理 defer 回调      │
│  - 线程化 IRQ 任务 (用户指定优先级) — 执行用户 handler        │
└─────────────────────────────────────────────────────────────┘
```

### 2.2 RAM 向量表

```
Flash (.isr_vector, 512B)          SRAM (.ram_vector, 512B, 512对齐)
┌──────────────────┐    SystemInit    ┌──────────────────────┐
│ 初始向量表 (只读)  │ ──copy──▶       │ 可读写向量表          │
│ SP + Reset + ...  │                 │ 支持运行时替换 ISR    │
└──────────────────┘                 └──────────────────────┘
                                               ▲
                                      SCB->VTOR = &ram_vector
```

- 系统启动时将 128 个 32-bit 向量从 Flash 复制到 SRAM
- `SCB->VTOR` 指向 RAM 向量表，支持运行时动态修改 ISR 入口
- 512 字节对齐 (Cortex-M7 要求 VTOR 低 7 位为 0)

---

## 3. ISR 管理器 (irq.c)

### 3.1 ISR 描述符池

每个注册的 ISR 对应一个描述符：

| 字段 | 类型 | 说明 |
|------|------|------|
| `irq_num` | int16_t | 外设 IRQ 号 (0-97 for STM32F767) |
| `handler` | isr_func_t | ISR 处理函数指针 |
| `priority` | uint8_t | NVIC 优先级 (0-14) |
| `flags` | uint8_t | IRQ_FLAG_HANDLER / IRQ_FLAG_THREADED |
| `in_use` | uint8_t | 槽位使用标志 |

池大小由 `IRQ_MAX_USER` 配置 (默认 16)。

### 3.2 ISR 注册流程

```
irq_register(irq, handler, priority)
    │
    ├─ 1. 参数校验: irq[0,97], handler≠NULL, priority≤14
    ├─ 2. 检查重复注册 → 返回 KERN_ERR_BUSY
    ├─ 3. 分配 irq_desc_t 槽位 → 满则 KERN_ERR_RESOURCE
    ├─ 4. hal_irq_set_vector(irq, handler)   → 写入 RAM 向量表
    ├─ 5. hal_irq_set_priority(irq, priority) → 配置 NVIC
    ├─ 6. hal_irq_enable_irq(irq)             → 使能 NVIC
    └─ 7. 返回 KERN_OK
```

### 3.3 ISR 注销流程

```
irq_unregister(irq)
    ├─ 1. 查找描述符 (by irq_num)
    ├─ 2. hal_irq_disable_irq(irq)           → 禁用 NVIC
    ├─ 3. hal_irq_set_vector(irq, _default_handler) → 恢复默认
    └─ 4. 释放槽位
```

### 3.4 中断上下文检测

```c
kern_is_in_isr()      // 读取 IPSR, 非零即 ISR 上下文
kern_irq_context()    // 返回 IRQ 号 (0-97) 或 -1 (任务模式)
                      // 通过 hal_irq_get_active() = (IPSR & 0x1FF) - 16
```

在任务上下文中 IPSR=0, `kern_is_in_isr()` 返回 0, `kern_irq_context()` 返回 -16。
判断逻辑: `hal_irq_get_active() >= 0` 表示在 ISR 中。

---

## 4. 底半部 — Bottom Half (bh.c)

### 4.1 设计动机

ISR 中不能执行耗时操作或调用阻塞 API。底半部将非紧急的延迟处理从 ISR 上下文移到任务上下文：

```
ISR (高优先级, 不可阻塞)              BH 服务任务 (优先级 2, 可阻塞)
┌──────────────────┐                 ┌───────────────────────────┐
│ 1. 紧急处理       │                 │ while (1) {               │
│ 2. bh_schedule()  │ ──sem_post──▶   │   sem_wait(bh_sem);      │
│ 3. 退出           │                 │   遍历 pool, 执行 handler │
└──────────────────┘                 │ }                         │
                                     └───────────────────────────┘
```

### 4.2 BH 控制块

| 字段 | 类型 | 说明 |
|------|------|------|
| `handler` | bh_handler_t | 延迟处理函数 |
| `arg` | void* | 用户参数 |
| `pending` | uint8_t | 待处理标志 (ISR 设置, 任务清除) |
| `in_use` | uint8_t | 槽位使用标志 |

### 4.3 信号量驱动模型

BH 服务任务使用信号量等待而非轮询：

- **init**: `bh_sem = sem_create(0, 0)` — 初始 0, 无上限
- **schedule**: `bh->pending = 1; sem_post(bh_sem)` — ISR 安全
- **loop**: `sem_wait(bh_sem, 100)` — 阻塞直到 BH 被调度, 100 tick 安全超时

优势：
- 无 BH 待处理时 CPU 零开销 (任务阻塞在信号量)
- ISR 触发即时响应 (sem_post 唤醒服务任务)
- 无轮询开销, 无调度干扰

### 4.4 BH 生命周期

```
bh_create(handler, arg) → bh_id
bh_schedule(bh_id)      → 设置 pending, 信号量唤醒服务任务
bh_delete(bh_id)        → 释放槽位
```

`bh_schedule()` 可安全地在 ISR 中调用 — 仅设置标志位并通过 `sem_post` 发送信号。
`bh_schedule()` 可被多次调用 — 服务任务一次遍历处理所有 pending 的 BH。

---

## 5. 线程化 IRQ (irq.c)

### 5.1 设计动机

某些驱动需要在 ISR 上下文中做较多处理，但直接写在 ISR 中会增加中断延迟。
线程化 IRQ 将 ISR 拆分为两部分：

```
硬件中断 → ISR 桩 (_threaded_isr_dispatch) → 任务 (_threaded_irq_task)
           │                                      │
           ├─ 禁用 IRQ (防重入)                    ├─ 执行用户 handler(arg)
           ├─ 清除 pending                        ├─ 可调用阻塞 API
           ├─ 设置 pending 标志                    └─ 重新使能 IRQ
           └─ sched_wakeup() 唤醒任务
```

### 5.2 线程化 IRQ 控制块

| 字段 | 类型 | 说明 |
|------|------|------|
| `task_id` | task_id_t | 关联任务 ID |
| `irq_num` | int16_t | IRQ 号 |
| `handler` | task_func_t | 用户任务上下文处理函数 |
| `arg` | void* | 用户参数 |
| `priority` | uint8_t | 任务优先级 |
| `in_use` | uint8_t | 使用标志 |
| `pending` | uint8_t | ISR 已触发的待处理标志 |

### 5.3 线程化 IRQ 流程

```
irq_request_threaded(irq, handler, arg, prio, stack)
    ├─ 1. 校验参数
    ├─ 2. 检查重复 → KERN_ERR_BUSY
    ├─ 3. 分配 irq_thread_t 槽位
    ├─ 4. task_create("irq_N", _threaded_irq_task, &slot, prio, stack)
    ├─ 5. hal_irq_set_vector(irq, _threaded_isr_dispatch)
    ├─ 6. hal_irq_set_priority + hal_irq_enable_irq
    └─ 7. irq_service_start() 中 task_start()

irq_release_threaded(irq)
    ├─ 1. hal_irq_disable_irq + 恢复 _default_handler
    ├─ 2. task_delete(关联任务)
    └─ 3. 释放槽位
```

### 5.4 竞争窗口保护

`_threaded_irq_task` 使用 1 tick 超时 `sched_block` 保护：
- ISR 在任务检查 `pending` 之后、调用 `sched_block` 之前触发
- 1 tick 超时确保任务在竞争情况下最多等待 1ms (1000Hz tick) 后重试

---

## 6. ISR 守卫

阻塞 API 在 ISR 上下文中被拒绝：

| API | 守卫行为 |
|-----|---------|
| `task_delay()` / `task_delay_ms()` | 返回 `KERN_ERR_ISR` |
| `sem_wait()` | 返回 `KERN_ERR_ISR` |
| `mutex_lock()` (timeout≠0) | 返回 `KERN_ERR_ISR` |
| `mqueue_send/recv()` (timeout≠0) | 返回 `KERN_ERR_ISR` |
| `event_wait()` | 返回 `KERN_ERR_ISR` |

非阻塞变体 (`sem_trywait`, `mutex_trylock`, `mqueue_trysend/tryrecv`, `event_get`) 保持 ISR 安全。

---

## 7. 配置参考

### Kconfig 选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `IRQ_ENABLE` | y | 使能中断管理 |
| `IRQ_MAX_USER` | 16 | ISR 描述符池大小 |
| `IRQ_DEFAULT_PRIORITY` | 8 | 默认 NVIC 优先级 |
| `IRQ_BH_ENABLE` | y | 使能底半部 |
| `IRQ_BH_MAX` | 8 | BH 池大小 |
| `IRQ_THREADED_ENABLE` | y | 使能线程化 IRQ |
| `IRQ_THREADED_MAX` | 4 | 线程化 IRQ 池大小 |
| `IRQ_THREADED_STACK_SIZE` | 512 | 线程化 IRQ 任务栈大小 |

### 关键常数

| 常量 | 值 | 说明 |
|------|-----|------|
| `IRQ_COUNT_MAX` | 98 | STM32F767 外设 IRQ 数量 |
| BH 服务优先级 | 2 | 低于定时器服务 (1), 高于普通任务 |
| BH 安全超时 | 100 ticks | sem_wait 安全超时 |

---

## 8. API 参考

### ISR 管理

```c
kern_err_t irq_register(int16_t irq, isr_func_t handler, uint8_t priority);
kern_err_t irq_unregister(int16_t irq);
kern_err_t irq_enable(int16_t irq);
kern_err_t irq_disable(int16_t irq);
int kern_is_in_isr(void);
int kern_irq_context(void);
```

### 底半部

```c
int16_t bh_create(bh_handler_t handler, void *arg);
kern_err_t bh_schedule(int16_t bh_id);   // ISR 安全
kern_err_t bh_delete(int16_t bh_id);
```

### 线程化 IRQ

```c
kern_err_t irq_request_threaded(int16_t irq, task_func_t handler,
                                void *arg, uint8_t priority, uint32_t stack_size);
kern_err_t irq_release_threaded(int16_t irq);
```

---

## 9. 文件清单

| 文件 | 说明 |
|------|------|
| `src/kernel/irq/irq.h` | ISR 管理器 API |
| `src/kernel/irq/irq.c` | ISR 注册/注销、线程化 IRQ、上下文检测 |
| `src/kernel/irq/bh.h` | 底半部 API |
| `src/kernel/irq/bh.c` | BH 池管理、信号量驱动服务任务 |
| `src/tests/test_irq.c` | 中断管理测试模块 (6 个测试) |
| `src/kernel/include/kernel_types.h` | irq_thread_t、bh_t 类型定义 |
| `src/arch/arm/cortex-m7/hal.c` | RAM 向量表操作 (hal_irq_set_vector) |
| `src/startup/arm/system.c` | SystemInit: 向量复制 + VTOR 重映射 |
| `link/stm32f767.ld` | .ram_vector 段 (512 对齐 SRAM) |
| `Kconfig` | 中断配置菜单 |
