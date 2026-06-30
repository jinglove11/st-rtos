# `CAP_RCU`

**Capability table RCU reader**

| Property | Value |
|---|---|
| Type | `bool` |
| Default | `n` |
| Menu path | `Phase 6 — SMP (Cortex-M33 Core 1)` |

## Dependencies

- `[SMP](SMP.md) && [CAP_ENABLE](CAP_ENABLE.md)`

## Help

```
跨核 cap 访问的 read-copy-update 同步。
关闭则退化为全局 spinlock (性能差但简单)。
```

---

- Back to [INDEX](INDEX.md)
- Top-level [Kconfig](../../Kconfig)
- Project roadmap: [MICROKERNEL_OS_ROADMAP.md](../../MICROKERNEL_OS_ROADMAP.md)
