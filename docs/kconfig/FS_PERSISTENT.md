# `FS_PERSISTENT`

**Persistent filesystem (littlefs)**

| Property | Value |
|---|---|
| Type | `bool` |
| Default | `n` |
| Menu path | `Phase 4 — Persistent Filesystem` |

## Dependencies

- `[VFS_ENABLE](VFS_ENABLE.md) && [BLOCK_DEVICE](BLOCK_DEVICE.md)`

## Help

```
引入 littlefs (Apache 2.0),挂载到 /flash。
Power-cut safe + wear-leveling 内建。
实现:Phase 4.2。
```

---

- Back to [INDEX](INDEX.md)
- Top-level [Kconfig](../../Kconfig)
- Project roadmap: [MICROKERNEL_OS_ROADMAP.md](../../MICROKERNEL_OS_ROADMAP.md)
