# `IRQ_DEFAULT_PRIORITY`

**Default ISR priority**

| Property | Value |
|---|---|
| Type | `int` |
| Default | `8` |
| Range | `0 14` |
| Menu path | `Interrupt Configuration` |

## Dependencies

- `[IRQ_ENABLE](IRQ_ENABLE.md)`

## Help

```
新注册 ISR 的默认 NVIC 优先级。
0=最高优先级, 14=最低 (PendSV/SysTick 使用 15)。
```

---

- Back to [INDEX](INDEX.md)
- Top-level [Kconfig](../../Kconfig)
- Project roadmap: [MICROKERNEL_OS_ROADMAP.md](../../MICROKERNEL_OS_ROADMAP.md)
