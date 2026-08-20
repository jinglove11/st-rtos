# `MPU_MAP_MAX`

**Per-task software MPU mapping table size (P1-3)**

| Property | Value |
|---|---|
| Type | `int` |
| Default | `16` |
| Range | `4 64` |
| Menu path | `Microkernel Configuration` |

## Dependencies

- `[MPU_ENABLE](MPU_ENABLE.md)`

## Help

```
每任务软映射表容量。硬件运行时槽(3..MPU_REGION_COUNT-1)只做该表
的 LRU 驻留缓存:表满才拒新映射,槽满不拒(MemManage 按需换入)。
突破原先"每任务 5 个运行时映射"的硬件槽上限。
```

---

- Back to [INDEX](INDEX.md)
- Top-level [Kconfig](../../Kconfig)
- Project roadmap: [MICROKERNEL_OS_ROADMAP.md](../../MICROKERNEL_OS_ROADMAP.md)
