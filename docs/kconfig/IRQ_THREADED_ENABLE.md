# `IRQ_THREADED_ENABLE`

**Enable threaded IRQs**

| Property | Value |
|---|---|
| Type | `bool` |
| Default | `y` |
| Menu path | `Interrupt Configuration` |

## Dependencies

- `[IRQ_ENABLE](IRQ_ENABLE.md)`

## Help

```
启用线程化中断处理。
ISR 被降级为任务上下文执行，适合复杂处理。
```

---

- Back to [INDEX](INDEX.md)
- Top-level [Kconfig](../../Kconfig)
- Project roadmap: [MICROKERNEL_OS_ROADMAP.md](../../MICROKERNEL_OS_ROADMAP.md)
