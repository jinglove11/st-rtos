# `IRQ_BH_ENABLE`

**Enable bottom halves**

| Property | Value |
|---|---|
| Type | `bool` |
| Default | `y` |
| Menu path | `Interrupt Configuration` |

## Dependencies

- `[IRQ_ENABLE](IRQ_ENABLE.md)`

## Help

```
启用底半部 (bottom half) 延迟处理机制。
可从 ISR 中安全调度，在任务上下文中执行。
```

---

- Back to [INDEX](INDEX.md)
- Top-level [Kconfig](../../Kconfig)
- Project roadmap: [MICROKERNEL_OS_ROADMAP.md](../planning/MICROKERNEL_OS_ROADMAP.md)
