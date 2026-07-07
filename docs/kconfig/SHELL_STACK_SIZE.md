# `SHELL_STACK_SIZE`

**Shell task stack size (bytes)**

| Property | Value |
|---|---|
| Type | `int` |
| Default | `2048` |
| Range | `1024 8192` |
| Menu path | `Shell Configuration` |

## Dependencies

- `[SHELL_ENABLE](SHELL_ENABLE.md)`

## Help

```
Shell 任务栈大小（字节）。
行缓冲 + 命令解析 + 目录遍历需要较大栈空间。
```

---

- Back to [INDEX](INDEX.md)
- Top-level [Kconfig](../../Kconfig)
- Project roadmap: [MICROKERNEL_OS_ROADMAP.md](../planning/MICROKERNEL_OS_ROADMAP.md)
