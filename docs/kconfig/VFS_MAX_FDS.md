# `VFS_MAX_FDS`

**Maximum file descriptors per task**

| Property | Value |
|---|---|
| Type | `int` |
| Default | `8` |
| Range | `4 32` |
| Menu path | `VFS Configuration` |

## Dependencies

- `[VFS_ENABLE](VFS_ENABLE.md)`

## Help

```
每个任务可同时打开的最大文件描述符数量。
每 fd 占 12 字节 (inode* + flags + offset + in_use)。
```

---

- Back to [INDEX](INDEX.md)
- Top-level [Kconfig](../../Kconfig)
- Project roadmap: [MICROKERNEL_OS_ROADMAP.md](../../MICROKERNEL_OS_ROADMAP.md)
