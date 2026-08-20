# sync 四族用户 syscall 取舍评估(P0-3 / 审计项 C8)

> 结论先行:**短期保留内核 syscall,中期降级为用户态实现(路线 P2-4)。
> 触发降级的三个前置条件见 §4。**

## 1. 现状盘点

| 层 | 资产 | 规模 |
|---|---|---|
| 内核对象 | sem/mutex/mqueue/event 四族内核对象 + 15 个用户 syscall | ~1900 行,占活跃 ABI 19% |
| 内核特性 | mutex 优先级继承(PI) + 死锁检测;mqueue 定长消息 | mutex.c 547 行 |
| 用户态 | `sync_server`(基于 endpoint 的 lock/unlock/trylock 服务) | 164 行,阻塞 lock 未完成(等 reply cap 延迟回复) |
| 测试 | ABI 层 sleepable/timeout/delete-wakeup 全套用例 | abi/test_syscall_user.c 内 14 组 |

## 2. 三个选项

**A. 永久保留在内核(RTOS 路线)**
- 依据:PI 必须在调度器内生效才能兑现实时语义;跨核 PI 在用户态几乎不可实现; syscall 直达无 IPC 往返,延迟确定性好。
- 代价:内核攻击面 +1900 行;19% 的 ABI 与"最小内核"定位冲突;每族对象都要 cap 化/生命周期/双核锁,硬化成本持续。

**B. 立即全部移到用户态(seL4 路线)**
- 依据:微内核纯度;fault 隔离(sync 逻辑崩溃不拖内核)。
- 代价:**PI 丢失**——用户态 server 无法参与调度决策,优先级继承需要在 IPC 层重新发明(endpoint 唤醒时继承 client 优先级,seL4 正是这么做在内核调度器里的,所以"移出去"并不能把 PI 也带走);死锁检测需要全局等待图,跨进程化;阻塞 lock 依赖的延迟 reply 机制 sync_server 尚未打通;现有 14 组 ABI 用例全部重写。

**C. 分层混合(推荐)**
- 内核只留"机制":调度 + endpoint(唤醒/阻塞/timeout 已具备,continuation 状态机已原子化)。
- sync 语义降为用户库:非阻塞 trylock/unlock 立即可迁(sync_server 已验证);阻塞锁、PI、死锁检测留在内核作为 **legacy 兼容层**,Kconfig 双轨开关控制。
- 迁移按族推进:mqueue/event 先走(纯语义,无 PI),mutex 最后(PI 等价物就绪才走)。

## 3. 决策

**保留至 P2-4 才降级**,理由按权重排序:

1. PI + 死锁检测是真实价值,不是历史包袱——没有 IPC 层 PI 等价物之前移走即功能退化;
2. sync_server 阻塞锁缺延迟 reply 机制,目标形态尚未验证;
3. 当前没有任何性能/安全事件要求立刻动它;15 个 syscall 已 cap 化、已过 1M 压力,风险受控。

## 4. 降级触发条件(三者齐备即启动 P2-4)

1. `sync_server` 补齐基于 reply cap 延迟回复的阻塞 lock,并在 ABI 层有等价用例覆盖;
2. 内核 IPC 唤醒路径具备优先级继承(endpoint 唤醒 server 时临时提升其优先级),PI 语义可由 IPC 层兑现;
3. mqueue/event 两族先完成用户态替换试点(无 PI 依赖,风险最低),试点期间两族 syscall 标记 `deprecated`。

满足后:sem/mutex syscall 保留一个发布周期作兼容层(Kconfig `CONFIG_LEGACY_SYNC_SYSCALL`,默认关),随后从活跃 ABI 移除、编号 reserved。
