# `DEV_PROFILE`

**Development image (shell without test framework)**

| Property | Value |
|---|---|
| Type | `bool` |
| Default | `n` |
| Menu path | `Test Configuration` |

## Dependencies

- `![TEST_ENABLE](TEST_ENABLE.md) && [SHELL_ENABLE](SHELL_ENABLE.md)`

## Help

```
dev profile:不编译测试框架,release 启动路径 + 特权 shell。
介于 test(全套测试)与 release(纯 init)之间的开发镜像。
```

---

- Back to [INDEX](INDEX.md)
- Top-level [Kconfig](../../Kconfig)
- Project roadmap: [MICROKERNEL_OS_ROADMAP.md](../../MICROKERNEL_OS_ROADMAP.md)
