# `FAULT_CRASH_DUMP`

**Enable crash dump on fault**

| Property | Value |
|---|---|
| Type | `bool` |
| Default | `y` |
| Menu path | `Fault Handler Configuration` |

## Dependencies

- `[FAULT_ENABLE](FAULT_ENABLE.md)`

## Help

```
故障时保存完整寄存器上下文到 SRAM 中的
.crash_dump section，重启后保留用于调试。
```

---

- Back to [INDEX](INDEX.md)
- Top-level [Kconfig](../../Kconfig)
- Project roadmap: [MICROKERNEL_OS_ROADMAP.md](../planning/MICROKERNEL_OS_ROADMAP.md)
