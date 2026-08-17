# `TEST_MODULE_BLOCK`

**Block device (flash) tests**

| Property | Value |
|---|---|
| Type | `bool` |
| Default | `y` |
| Menu path | `Test Configuration` |

## Dependencies

- `[TEST_ENABLE](TEST_ENABLE.md) && [BLOCK_DEVICE](BLOCK_DEVICE.md)`

## Help

```
Phase 3 §3.3 板载 QSPI NOR flash block device 测试。
擦写 100 sector 读回校验 + 边界检查。
⚠️ 会擦写 flash FS 区(固件区不动),测试期间系统卡顿几秒。
```

---

- Back to [INDEX](INDEX.md)
- Top-level [Kconfig](../../Kconfig)
- Project roadmap: [MICROKERNEL_OS_ROADMAP.md](../../MICROKERNEL_OS_ROADMAP.md)
