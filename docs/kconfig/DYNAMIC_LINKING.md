# `DYNAMIC_LINKING`

**Dynamic linking (.so + dlopen)**

| Property | Value |
|---|---|
| Type | `bool` |
| Default | `n` |
| Menu path | `Phase 9 — Comprehensiveness` |

## Dependencies

- `[ELF_LOADER](ELF_LOADER.md)`

## Help

```
PLT/GOT + sys_dlopen/sys_dlsym。资源占用大,默认关。
实现:Phase 9.3。
```

---

- Back to [INDEX](INDEX.md)
- Top-level [Kconfig](../../Kconfig)
- Project roadmap: [MICROKERNEL_OS_ROADMAP.md](../planning/MICROKERNEL_OS_ROADMAP.md)
