# `VFS_MAX_INODES`

**Maximum inodes**

| Property | Value |
|---|---|
| Type | `int` |
| Default | `32` |
| Range | `8 128` |
| Menu path | `VFS Configuration` |

## Dependencies

- `[VFS_ENABLE](VFS_ENABLE.md)`

## Help

```
全局 inode 池大小。
每个文件、目录、设备节点占用一个 inode。
```

---

- Back to [INDEX](INDEX.md)
- Top-level [Kconfig](../../Kconfig)
- Project roadmap: [MICROKERNEL_OS_ROADMAP.md](../../MICROKERNEL_OS_ROADMAP.md)
