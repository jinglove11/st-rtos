# 中断管理模块调试记录

## 概述

在实现中断管理模块 (ISR 注册 + 底半部 + 线程化 IRQ) 过程中，遇到了 4 个测试失败和一个调度器干扰问题。本文档记录每个问题的现象、根因和解决方案。

---

## 问题 1: ISR 池测试 — NULL Handler 被拒绝

### 现象

```
[Module] irq
Test 1: ISR Pool Management
[PASS] Pool full: register fails
[FAIL] Unregister IRQ 0        ← 失败
```

### 根本原因

`test_isr_pool()` 中填充 ISR 池时使用了 `NULL` handler：

```c
kern_err_t err = irq_register((int16_t)i, NULL, 8);  // BUG
```

`irq_register()` 在参数校验中拒绝 `NULL` handler：

```c
if (irq < 0 || irq >= IRQ_COUNT_MAX || handler == NULL) {
    return KERN_ERR_PARAM;
}
```

因此池中实际没有注册任何 ISR，后续注销操作找不到已注册的 IRQ，返回 `KERN_ERR_NOEXIST`。

### 解决方案

在文件顶部添加 `test_handler_stub` 的前向声明，两个测试使用有效的 handler：

```c
// 前向声明
static void test_handler_stub(void);

// 修复后的测试
kern_err_t err = irq_register((int16_t)i, test_handler_stub, 8);
kern_err_t err = irq_register(0, test_handler_stub, 8);  // 满池测试
```

### 经验教训

- 测试桩必须与实际 API 约束一致
- 函数定义在后的情况需要前向声明

---

## 问题 2: BH Handler 未执行

### 现象

```
Test 3: Bottom Half Lifecycle
[PASS] BH create returns valid ID
[PASS] BH schedule OK
[FAIL] BH handler executed       ← counter 仍为 0
```

### 根本原因

`bh_init()` 中创建了 BH 服务任务但未调用 `task_start()`：

```c
// 原始代码
task_id_t bh_tid = task_create("bh_svc", bh_service_loop, NULL, 1, 512);
// 缺少 task_start(bh_tid);
```

任务创建后状态为 `TASK_STATE_CREATED`，必须调用 `task_start()` 才会加入就绪队列。
BH 服务任务从未被调度，所有 `bh_schedule()` 只是设置了 pending 标志，无人处理。

### 解决方案

将初始化拆分为两个函数：

```c
void bh_init(void) {
    // 仅初始化 BH 池 (在 kern_init 阶段调用)
    memset(bh_pool, 0, sizeof(bh_pool));
    bh_sem = sem_create(0, 0);
}

void bh_service_start(void) {
    // 创建并启动 BH 服务任务 (在 kern_start 阶段调用)
    task_id_t bh_tid = task_create("bh_svc", bh_service_loop, NULL, 2, 512);
    if (bh_tid >= 0) {
        task_start(bh_tid);  // 关键！
    }
}
```

`bh_service_start()` 在 `kernel.c:kern_start()` 中调用，与 `timer_service_start()` 同一阶段。

### 经验教训

- `task_create` 和 `task_start` 是两个独立步骤
- 服务任务的启动应在 `kern_start()` 阶段进行

---

## 问题 3: task_delay 在任务上下文中返回 KERN_ERR_TIMEOUT

### 现象

```
Test 6: ISR Guards
[FAIL] task_delay OK in task context
[FAIL] task_delay_ms OK in task context
```

### 根本原因

`task_delay()` 内部调用 `sched_block(BLOCK_REASON_SLEEP, NULL, ticks)`。
当延时到期时，`sched_tick_handler()` 调用 `sched_wakeup(tcb, KERN_ERR_TIMEOUT)`，
将 `block_result` 设为 `KERN_ERR_TIMEOUT`。

原始代码直接将 `block_result` 返回给调用者：

```c
kern_err_t task_delay(uint32_t ticks) {
    ...
    return sched_block(BLOCK_REASON_SLEEP, NULL, ticks);
    // 返回 KERN_ERR_TIMEOUT — 但延时完成是正常行为！
}
```

正常的延时完成不应被视为错误。`KERN_ERR_TIMEOUT` 是内部唤醒标记，语义是 "因超时而唤醒" 而非 "操作失败"。

### 解决方案

在 `task_delay()` 中将 `KERN_ERR_TIMEOUT` 转换为 `KERN_OK`：

```c
kern_err_t task_delay(uint32_t ticks) {
    if (hal_irq_get_active() >= 0) {
        return KERN_ERR_ISR;
    }
    if (ticks == 0) {
        return KERN_OK;
    }
    kern_err_t err = sched_block(BLOCK_REASON_SLEEP, NULL, ticks);
    return (err == KERN_ERR_TIMEOUT) ? KERN_OK : err;
}
```

### 经验教训

- `KERN_ERR_TIMEOUT` 是超时唤醒的内部信号，调用者关心的是"延时是否完成"
- API 应返回语义正确的结果，而非内部实现细节

---

## 问题 4: BH 服务任务轮询干扰调度器测试

### 现象

```
Test 5: Suspend & Resume
[FAIL] Resumed task continued    ← count2 不大于 count1
```

Scheduler Test 5 在 BH 服务任务启用时持续失败，禁用后通过。

### 根本原因

BH 服务任务使用了 `task_delay(10)` 轮询模式：

```c
// 原始代码 — 轮询模式
while (1) {
    // 处理所有 pending BH...
    task_delay(10);  // 每 10 ticks 唤醒一次
}
```

BH 服务任务优先级为 2，高于测试任务的优先级 10。每 10 ticks 的周期性唤醒会导致：

1. BH 服务抢占正在运行的测试任务
2. PendSV 触发上下文切换
3. 可能打乱同优先级任务的 round-robin 顺序
4. 测试任务从 `task_resume()` 恢复后可能被排在就绪队列末尾，而测试运行器也处于优先级 10，形成竞争

测试任务的 `task_yield()` 后如果恰逢 BH 服务抢占，可能被错误地置于就绪队列的不利位置。

### 解决方案

改为**信号量驱动模型**，BH 服务任务在无工作时完全阻塞：

```c
// 修复后 — 信号量模式
static sem_id_t bh_sem;

void bh_init(void) {
    bh_sem = sem_create(0, 0);  // 计数信号量, 初始 0
}

// BH 服务循环
while (1) {
    sem_wait(bh_sem, 100);  // 阻塞等待工作, 100 tick 安全超时
    // 处理所有 pending BH...
}

// ISR 调度
kern_err_t bh_schedule(int16_t bh_id) {
    bh->pending = 1;
    sem_post(bh_sem);      // 唤醒服务任务
    return KERN_OK;
}
```

优势：
- **零 CPU 开销**：无 BH 待处理时任务完全阻塞，不抢占任何任务
- **即时响应**：`sem_post` 立即唤醒服务任务处理 BH
- **ISR 安全**：`sem_post` 使用临界区保护，安全在 ISR 中调用

### 经验教训

- 服务任务**永远不要轮询**，应使用阻塞原语等待实际工作
- 高优先级任务即使是短暂的周期性唤醒也会干扰低优先级任务的调度
- 信号量/事件标志是服务任务的标准唤醒机制
- 调试时应先隔离服务任务，确定根因是调度逻辑还是任务干扰

---

## 问题 5: 定时器服务任务 100 tick 超时分析

### 现象

定时器服务任务在堆为空时使用 100 tick 超时：

```c
if (timer_heap.size > 0) {
    timeout = next_expire - now;  // 精确等待
} else {
    timeout = 100;  // 堆空时 100 tick 轮询
}
```

### 分析

100 tick (100ms) 的超时间隔足够长，对调度器测试影响远小于 BH 的 10 tick 轮询。
在本次测试中，100 tick 轮询未导致 Scheduler Test 5 失败。
但理论上，在极端情况下 (恰好与测试任务 yield 同步) 仍可能产生微小干扰。

### 潜在优化

可以通过事件标志组消除定时器服务的空堆轮询：

```c
// 使用事件标志组, 仅在 timer_create/timer_start 时触发
// 但当前设计已足够, 100 tick 安全超时可作为"心跳"保留
```

---

## 调试方法论总结

1. **隔离法**：禁用可疑模块确定根因范围 (本例禁用服务任务验证了调度器自身无 bug)
2. **逐层验证**：从底层 (API 参数校验) 到顶层 (服务任务行为) 逐层排查
3. **语义分析**：区分内部标记 (`KERN_ERR_TIMEOUT`) 和用户语义 (`延时完成`)
4. **禁止轮询**：嵌入式 RTOS 中服务任务必须用阻塞原语，轮询是不正确的设计模式
