# `ELF_LOADER`

**ELF loader (sys_proc_exec)**

| Property | Value |
|---|---|
| Type | `bool` |
| Default | `n` |
| Menu path | `Phase 5 — Application Runtime` |

## Dependencies

- `[USER_LIBC](USER_LIBC.md) && [FS_PERSISTENT](FS_PERSISTENT.md)`

## Help

```
从 /flash/apps/<name>.elf 加载 ELF,解析 PT_LOAD + relocations。
RP2350 NS 约束:同物理地址空间,MPU 隔代码/数据区。
不支持 fork、ASLR、动态链接(动态链接留 CONFIG_DYNAMIC_LINKING)。
实现:Phase 5.2。
```

---

- Back to [INDEX](INDEX.md)
- Top-level [Kconfig](../../Kconfig)
- Project roadmap: [MICROKERNEL_OS_ROADMAP.md](../../MICROKERNEL_OS_ROADMAP.md)
