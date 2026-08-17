# `TEST_MODULE_ELF`

**ELF loader tests**

| Property | Value |
|---|---|
| Type | `bool` |
| Default | `y` |
| Menu path | `Test Configuration` |

## Dependencies

- `[TEST_ENABLE](TEST_ENABLE.md) && [ELF_LOADER](ELF_LOADER.md)`

## Help

```
核心补齐 #6:ELF 进程加载 + 执行 + join 验证。
嵌入一个 freestanding 测试 ELF,elf_load 解析 + 创建 user 任务执行。
```

---

- Back to [INDEX](INDEX.md)
- Top-level [Kconfig](../../Kconfig)
- Project roadmap: [MICROKERNEL_OS_ROADMAP.md](../../MICROKERNEL_OS_ROADMAP.md)
