# `PANIC_LOG`

**Panic dump to flash (crash_dump persistence)**

| Property | Value |
|---|---|
| Type | `bool` |
| Default | `n` |
| Menu path | `Phase 8 — Security & Reliability` |

## Dependencies

- `[BLOCK_DEVICE](BLOCK_DEVICE.md)`

## Help

```
panic 时把 backtrace 写到 flash 最后一个 sector。
上电时 supervisor 检查并上报。实现:Phase 8.2。
```

---

- Back to [INDEX](INDEX.md)
- Top-level [Kconfig](../../Kconfig)
- Project roadmap: [MICROKERNEL_OS_ROADMAP.md](../../MICROKERNEL_OS_ROADMAP.md)
