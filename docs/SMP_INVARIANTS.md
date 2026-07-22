# SMP 不变量、迁移协议与锁顺序

## 1. 调度不变量

### I1：TCB 唯一性

任意时刻，每个 live task 只能位于以下一处：

- 一个 CPU 的 `_current_task[cpu]`（`RUNNING`）；
- 一个 CPU 的 runqueue（`READY`）；
- 一个对象的 wait queue（`BLOCKED`）；
- 不在任何队列（`CREATED/SUSPENDED/TERMINATED`）。

`RUNNING` TCB 不得再次入 ready queue；禁止同一 TCB 同时出现在两个
runqueue、两个 wait queue 或两个 CPU current 中。

### I2：per-CPU runqueue 一致性

每个 CPU 独立维护 ready list、ready bitmap、count 和 `need_resched`：

- bitmap 有 bit 时，对应优先级链表必须非空；
- bitmap 无 bit 时，对应链表必须为空；
- 只有 owner CPU 可以直接修改本核 runqueue。

跨核 add/remove/reinsert/quiesce 必须通过 remote-op queue + IPI，不得直接修改
另一核的 ready list。

### I3：迁移状态机

READY task 的 work stealing 路径为：

```text
STABLE(owner CPU queue)
  -> MIGRATING(从 donor 队列摘除)
  -> READY_REMOTE(已投递 target remote-op queue)
  -> STABLE(只由 target CPU 入队)
```

`cpu_owner` 和 `affinity_mask` 必须在每次入队/选中时校验。正在运行的任务不允许
把 affinity 改成不包含当前 owner CPU。

### I4：时间与唤醒

- Core 0 是唯一 timekeeper；只有 Core 0 增加全局 tick。
- Core 1 SysTick 只处理本地 time slice 和 per-CPU 统计。
- 两核 SysTick 都不获取 task/IPC/cap 等普通跨核锁。Core 0 SysTick 无锁唤醒
  固定在 Core 0 的 `timeout_svc`，由该线程扫描 timeout、reclaim 并聚合统计。
- wakeup 只能完成一次；`sched_wakeup()` 仅接受 `BLOCKED` task。
- 跨核 wakeup 通过 SIO FIFO IPI 投递，不依赖下一次 tick。

### I5：PendSV 禁止全局锁

PendSV 只操作本核 runqueue/per-CPU trace/stats。它不获取 task/cap/IPC/memory
等可能被被抢占线程持有的跨核锁。remote quiesce 的 completion 必须在
PendSV 最后一次引用旧 TCB 之后发布。

## 2. 全局锁顺序

debug 构建启用 lockdep，所有跨核 `irq_spinlock_t` 按严格递增 rank 获取：

```text
REGISTRY(10) -> TASK(20) -> OBJECT(30) -> RESOURCE(40) -> REMOTE(50)
```

| Rank | 典型锁 | 保护内容 |
| --- | --- | --- |
| REGISTRY | scheduler boot、IRQ table、device、mempool | 注册表与启动期状态 |
| TASK | task lock | TCB pool、used bitmap、join/exit/reclaim |
| OBJECT | sem/mutex/mqueue/event/endpoint/channel/timer/BH/fault | 对象 slot、wait queue、ring/heap |
| RESOURCE | cap pool、memory | capability 派生树、heap/SHM/MMIO |
| REMOTE | scheduler remote queue | 跨核调度操作 |

规则：

- 禁止反向获取；
- 禁止同 rank 的两把锁嵌套；
- 解锁必须 LIFO；
- 禁止持有对象锁调用用户 callback、timer callback 或 capability cleanup hook；
- capability hook 先记录到受 cap lock 保护的全局 FIFO；仅在最外层
  `irq_spinlock_t` 完全释放后由 thread/SVC 安全点执行。hard IRQ、PendSV、
  SysTick 只发布 pending，Core 0 `timeout_svc` 提供最终 drain 兜底；
- capability cleanup/revoke hook 不得阻塞或 yield；
- 线程争用 `irq_spinlock_t` 时，在 try-lock 失败的等待间隙恢复原 PRIMASK，
  允许调度 IPI 打断等待并完成 remote quiesce；成功持锁后才保持本核中断关闭。
- hard IRQ 和 PendSV 不允许等待普通跨核锁。

## 3. 数据归属

| 数据 | 归属/同步 |
| --- | --- |
| `_current_task[cpu]`、`_next_task[cpu]` | owner CPU；跨核诊断只读 |
| runqueue/bitmap/need_resched | owner CPU；remote-op + IPI 修改 |
| idle TCB/stack | per-CPU |
| trace ring | per-CPU，本核 IRQ 临界区 |
| kernel/task stats | per-CPU 计数，Core 0 聚合 |
| global tick | Core 0 单写 |
| timeout/reclaim scan | Core 0 `timeout_svc` thread |
| task/cap/IPC/device/IRQ/memory pool | 对应 rank 的真跨核锁 |

## 4. 发布与验收配置

- `configs/release_defconfig`：`SMP=n`，M1 真机门禁完成前不改变。
- `configs/rp2350_smp_defconfig`：`SMP=y`、双核测试开启、sem/endpoint
  跨核 ping-pong 各 100 万轮。
- 普通开发 `.config` 可将 `SMP_STRESS_ITERATIONS` 设为 10000 做快速回归。

## 5. 尚未由 M1 解决的边界

- `task_get_tcb()` 仍返回无 lifetime pin 的原始指针；对象 generation/RCU/CSpace
  lifetime 属于 M2。M1 要求所有发布、删除和回收写路径持 task lock。
- `CAP_RCU=y` 目前仍退化为 cap global spinlock 正确性路径；真正的 lockless
  reader 与 reclamation 属于 M2。
- 30 分钟/8 小时/24 小时 soak 需要 RP2350 实机、串口日志和复位监控，
  不能用“编译通过”替代。烧录验收 profile 后运行：

```sh
make test-smp-soak PORT=/dev/ttyACM0 DURATION=1800
make test-smp-soak PORT=/dev/ttyACM0 DURATION=28800
make test-smp-soak PORT=/dev/ttyACM0 DURATION=86400
```
