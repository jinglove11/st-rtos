# `VFS_ENABLE`

**Enable Virtual File System**

| Property | Value |
|---|---|
| Type | `bool` |
| Default | `n` |
| Menu path | `VFS Configuration` |

_No dependencies._

## Help

```
内核 VFS 实现已删除 (Phase F3,设备走 capability + endpoint RPC)。
保留开关仅为兼容旧配置: 置 y 会编译失败 (devfs API 已不存在)。
```

---

- Back to [INDEX](INDEX.md)
- Top-level [Kconfig](../../Kconfig)
- Project roadmap: [MICROKERNEL_OS_ROADMAP.md](../../MICROKERNEL_OS_ROADMAP.md)
