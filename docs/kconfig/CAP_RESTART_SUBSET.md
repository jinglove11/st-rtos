# `CAP_RESTART_SUBSET`

**Capability subset on restart (drop CAP_GRANT)**

| Property | Value |
|---|---|
| Type | `bool` |
| Default | `n` |
| Menu path | `Phase 2 — Fault-Tolerant Infrastructure` |

## Dependencies

- `[SUPERVISOR](SUPERVISOR.md) && [CAP_ENABLE](CAP_ENABLE.md)`

## Help

```
重启的任务不再继承 CAP_GRANT,防止失控子任务派生 cap。
实现:Phase 2.4。
```

---

- Back to [INDEX](INDEX.md)
- Top-level [Kconfig](../../Kconfig)
- Project roadmap: [MICROKERNEL_OS_ROADMAP.md](../../MICROKERNEL_OS_ROADMAP.md)
