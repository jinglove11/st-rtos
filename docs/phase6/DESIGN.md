# Phase 6: 异常容错 + 诊断增强

> 日期: 2026-05-08
> 基线: 489 tests, 0 failures
> 目标: fault 注入测试 + 看门狗 + CPU 统计 + shell 诊断 + 收尾

---

## 一、背景

Phase 1-5 已完成微内核核心功能:
- MPU 内存保护 + syscall (Phase 1)
- 能力系统 (Phase 2)
- VFS + devfs + ramfs (Phase 3)
- 设备驱动框架 (Phase 4)
- Endpoint/Channel IPC (Phase 5)

当前缺陷:
1. **Fault 测试薄弱** — test_fault.c 仅 10 条断言，只验证结构体大小和符号链接，无实际 fault 注入
2. **看门狗未实现** — `KERNEL_WATCHDOG` 配置项存在但 HAL 层无实现
3. **CPU 统计未更新** — TCB 有 `ctx_switch_count` / `cpu_usage` 字段，scheduler 未写入
4. **Shell 诊断不足** — 无 `top` 命令（CPU 使用率）、无 `trace` 命令
5. **defconfig 未同步** — `.config` 与 `configs/stm32f767_defconfig` 不一致
6. **Phase 5 遗留** — capability 权限测试 (5.2.19) 未完成

---

## 二、Step 1: Fault 注入测试

### 2.1 目标

验证 fault handler 在真实异常条件下正确工作:
- 用户任务 MemManage fault → 任务被终止，内核继续运行
- 用户任务 UsageFault (除零) → 同上
- 内核 fault → crash_dump 保存正确
- 栈溢出 → MemManage → 任务终止

### 2.2 测试策略

在 Cortex-M7 上，无法直接"注入" fault — 需要触发真实异常:

| 测试 | 触发方式 | 预期结果 |
|------|----------|----------|
| MemManage - 未授权地址写 | 写 0x00000000 (NULL 区域) | 用户任务被终止 |
| MemManage - MPU 违规 | 写 Flash 区域 (只读) | 用户任务被终止 |
| UsageFault - 除零 | `volatile int a = 1/0` | 用户任务被终止 |
| 栈溢出检测 | 递归函数耗尽栈 | stack_check 检测到 |
| crash_dump 字段验证 | fault 后检查 dump 内容 | 字段正确 |

### 2.3 实现方案

**用户态 fault 任务**: 创建一个用户任务，故意触发 fault。fault handler 应终止该任务而不影响内核。主测试任务通过 `task_join` 验证目标任务被终止。

```c
/* 触发 MemManage fault 的任务 */
static void fault_task_null_write(void *arg) {
    (void)arg;
    volatile uint32_t *p = (volatile uint32_t *)0x00000000;
    *p = 0xDEAD;  /* 写 NULL 区域 → MemManage */
    /* 不应到达这里 */
    task_exit((void *)0xBAD);
}

/* 测试: 用户 fault 不拖死内核 */
static void test_user_memmanage(void) {
    task_id_t tid = task_create_user("f_null", fault_task_null_write, NULL, 10, 512);
    task_start(tid);
    task_join(tid, NULL, 1000);
    /* 如果内核还活着到这里 → PASS */
    TEST_ASSERT(1, "kernel survived user MemManage");
}
```

### 2.4 文件变更

| 操作 | 文件 | 说明 |
|------|------|------|
| 修改 | `src/tests/test_fault.c` | 添加 6+ fault 注入测试 |

---

## 三、Step 2: 看门狗实现

### 3.1 目标

实现独立看门狗 (IWDG)，防止系统死锁。

### 3.2 STM32F767 IWDG

```
IWDG 寄存器 (0x40003000):
  KR  (0x00): Key register — 写 0xCCCC 启动, 0xAAAA 喂狗
  PR  (0x04): Prescaler — 0~7, 分频 4/8/16/32/64/128/256
  RLR (0x08): Reload — 12-bit (0~4095)
  SR  (0x0C): Status
  WINR(0x10): Window

超时计算:
  timeout_ms = (4 × 2^PR × RLR) / LSI_freq
  LSI ≈ 32 KHz

  PR=6, RLR=4095 → timeout ≈ (4 × 64 × 4095) / 32000 ≈ 32760ms
  PR=3, RLR=1250 → timeout ≈ (4 × 8 × 1250) / 32000 ≈ 1250ms
```

### 3.3 实现

```c
/* hal_watchdog_init(timeout_ms) — 计算 PR + RLR, 启动 IWDG */
/* hal_watchdog_feed() — 写 0xAAAA */
/* hal_watchdog_reset_cause() — 读 RCC_CSR 复位原因 */
```

在 `idle_task_func` 中调用 `hal_watchdog_feed()` (已有 `#if KERN_WATCHDOG_ENABLE` 分支)。

### 3.4 文件变更

| 操作 | 文件 | 说明 |
|------|------|------|
| 修改 | `src/arch/arm/cortex-m7/hal.c` | 实现 watchdog init/feed/reset_cause |
| 修改 | `src/hal/hal.h` | 声明 watchdog API |
| 新增 | `src/tests/test_watchdog.c` | 看门狗配置 + 喂狗测试 |

---

## 四、Step 3: CPU 使用率统计

### 4.1 目标

每个任务记录 CPU 使用率百分比，shell `top` 命令可显示。

### 4.2 实现方案

在 PendSV 中切换时递增 `ctx_switch_count`。使用 tick 采样法:
- 每 tick 中断记录当前运行任务
- 每 1000 ticks (1秒) 统计各任务的 tick 计数占比

```c
/* scheduler.c — 每 tick 调用 */
void sched_update_cpu_usage(void) {
    tcb_t *cur = sched_get_current();
    if (cur && cur->id >= 0) {
        cur->total_ticks++;
    }
    tick_counter++;
    if (tick_counter >= 1000) {
        for (int i = 0; i < KERNEL_MAX_TASKS; i++) {
            if (task_used_bitmap & (1U << i)) {
                task_pool[i].cpu_usage = task_pool[i].total_ticks * 10000 / 1000;
                task_pool[i].total_ticks = 0;
            }
        }
        tick_counter = 0;
    }
}
```

`cpu_usage` 单位: 万分比 (10000 = 100%)。

### 4.3 文件变更

| 操作 | 文件 | 说明 |
|------|------|------|
| 修改 | `src/kernel/core/scheduler.c` | tick 中断更新 CPU 统计 |
| 修改 | `src/app/shell.c` | `top` 命令显示 CPU 使用率 |
| 新增 | `src/tests/test_stats.c` | CPU 统计准确性测试 |

---

## 五、Step 4: Shell 诊断增强

### 5.1 新增命令

| 命令 | 功能 | 实现 |
|------|------|------|
| `top` | 实时任务 + CPU 使用率 | 类似 `ps`，增加 CPU% 列 |
| `trace` | 显示最近 N 条内核事件 | 读 trace buffer |
| `reset` | 软件复位 | 调用 `hal_system_reset()` |
| `version` | 内核版本信息 | 打印 KERN_NAME + 版本号 |

### 5.2 Trace Buffer (简化版)

```c
#define TRACE_BUF_SIZE 128

typedef struct {
    uint32_t    tick;
    uint8_t     event;    /* TRACE_TASK_SWITCH, TRACE_ISR, TRACE_SYSCALL */
    uint8_t     task_id;
    uint16_t    data;
} trace_entry_t;

static trace_entry_t trace_buf[TRACE_BUF_SIZE];
static uint16_t trace_head;

void trace_record(uint8_t event, uint8_t task_id, uint16_t data);
```

trace 点埋入:
- `sched_switch` → `TRACE_TASK_SWITCH`
- `isr_enter/exit` → `TRACE_ISR`
- `kern_syscall_handler` → `TRACE_SYSCALL`

### 5.3 文件变更

| 操作 | 文件 | 说明 |
|------|------|------|
| 新增 | `src/kernel/trace/trace.c/h` | trace buffer + record API |
| 修改 | `src/kernel/core/scheduler.c` | 埋 trace 点 |
| 修改 | `src/kernel/syscall/syscall.c` | 埋 syscall trace 点 |
| 修改 | `src/app/shell.c` | top/trace/reset/version 命令 |
| 修改 | `Makefile` | 添加 trace.c |

---

## 六、Step 5: 收尾

### 6.1 Phase 5 遗留

| 任务 | 说明 |
|------|------|
| capability 权限测试 | 测试无权限操作返回 KERN_ERR_CAP |
| defconfig 同步 | 更新 `configs/stm32f767_defconfig` |

### 6.2 测试验证

- `make BOARD=stm32f767 -j8` → 0 warnings
- 全部现有测试通过 (489 tests)
- 新增测试通过
- 总测试数 ≥ 510

---

## 七、文件变更汇总

| 操作 | 文件 | 说明 |
|------|------|------|
| 修改 | `src/tests/test_fault.c` | fault 注入测试 (6+) |
| 修改 | `src/arch/arm/cortex-m7/hal.c` | watchdog 实现 |
| 修改 | `src/hal/hal.h` | watchdog API 声明 |
| 新增 | `src/tests/test_watchdog.c` | 看门狗测试 |
| 修改 | `src/kernel/core/scheduler.c` | CPU 统计 + trace 点 |
| 新增 | `src/kernel/trace/trace.c` | trace buffer |
| 新增 | `src/kernel/trace/trace.h` | trace API |
| 修改 | `src/kernel/syscall/syscall.c` | syscall trace 点 |
| 修改 | `src/app/shell.c` | top/trace/reset/version |
| 新增 | `src/tests/test_stats.c` | CPU 统计测试 |
| 修改 | `src/tests/test_capability.c` | 权限拒绝测试 |
| 修改 | `Makefile` | 添加 trace.c, test_stats.c, test_watchdog.c |
| 修改 | `Kconfig` | TRACE_ENABLE 配置 |
| 修改 | `src/kernel/include/kernel_config.h` | 更新默认值 |
| 修改 | `configs/stm32f767_defconfig` | 同步配置 |
| **合计** | **15** | |

---

## 八、实施步骤

```
Step 1: Fault 注入测试 (test_fault.c)
Step 2: 看门狗 (hal.c + test_watchdog.c)
Step 3: CPU 统计 (scheduler.c + shell.c top)
Step 4: Trace 系统 (trace.c + shell.c trace)
Step 5: 收尾 (capability 测试 + defconfig + 回归)
```
