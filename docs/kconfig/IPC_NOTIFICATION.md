# `IPC_NOTIFICATION`

**Enable notification objects (P1-1, seL4-style)**

| Property | Value |
|---|---|
| Type | `bool` |
| Default | `n` |
| Menu path | `IPC Configuration` |

_No dependencies._

## Help

```
独立 notification 对象:单字聚合徽章 word,signal 只做 |=,
wait/poll 消费整字。与 event(按位匹配)互补,是 timer/irq/BH
通知化(P2-2/P2-3)与 M4 的前置。默认关闭,待消费者接入后转正。
```

---

- Back to [INDEX](INDEX.md)
- Top-level [Kconfig](../../Kconfig)
- Project roadmap: [MICROKERNEL_OS_ROADMAP.md](../../MICROKERNEL_OS_ROADMAP.md)
