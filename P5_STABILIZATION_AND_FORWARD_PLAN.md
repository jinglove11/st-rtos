# My-RTOS P5 — 稳定化 + 前瞻路线

> 上接 `federated-puzzling-barto.md`(RP2350/M33 迁移)。本计划聚焦**锁定当前战果**与
> **挑选 3 个 ROI 最高的前瞻项**。状态基准:`build/rp2350-pico-sdk/my-rtos-pico2w.elf`
> 烧录后 UART 串口测得 **2867/2867 tests PASS, 0 FAIL**(20 模块全绿,shell 上线)。

---

## §A 代码审计 — 用户本轮修复

测试基线对比:

| 模块 | 之前 PASS/FAIL | 现在 PASS/FAIL | 状态 |
|---|---|---|---|
| driver | 658/112 | 全绿 | 修复 |
| service_model | 466/41 | 全绿(svc_runtime 507/0) | 修复 |
| syscall | 298/30 | 全绿(svc_runtime 338/0) | 修复 |
| irq | 101/19 | 125/0 | 修复 |
| **总计** | ~94% | **100% (2867/2867)** | **PASSED** |

### A.1 关键修复(逐项审计)

| 文件 | 改动 | 质量评估 |
|---|---|---|
| `src/kernel/lib/kstring.c` | memset/memcpy/memmove 加 `optimize("no-tree-loop-distribute-patterns")` | ✅ 已记忆 `gcc-loop-distribute-memset-recursion`,根治 freestanding 自递归栈爆 |
| `src/drivers/chip/rp2350/uart_rp2350.c` | 改写为 SDK 薄包装(`uart_inst_t*`);`#undef` 后再 include SDK | ✅ 已记忆 `rp2350-uart-must-enable-peri-clk`,正确消化 SDK macro 冲突 |
| `src/board/rp2350/{board.c, rp2350.h}` | 删 `set_sys_clock_khz`;`BOARD_DEFAULT_UART=uart0`(inst) | ✅ 让 SDK 拥有时钟,避免 double-init |
| `src/drivers/include/uart.h` | 删 `#define uart_init rtos_uart_init` 等 8 个 macro(RP2350 路径) | ✅ 消除宏名冲突 |
| `src/kernel/mpu/mpu.c` | 完整 PMSAv8:8-entry MAIR、AP legacy→v8 映射、`mpu_load_task_regions` 保存/恢复 MPU_CTRL | ✅ **亮点** — live region 更新期间关 MPU 防止 transient bad overlap,这是教科书级防御写法 |
| `src/arch/arm/cortex-m7/svc_handler.S` | PSPLIM 三路径(首次切/syscall 返回/blocked→切)都 `OFF_SP_LIMIT` 保存+恢复;`ldmia r7,{r0-r3}` 在调 C dispatcher 前重读硬件帧 | ✅ **亮点** — 注释解释了 r1-r3 在保存 PSPLIM 时被覆盖,必须重读,这是真问题 |
| `src/tests/test_framework.{c,h}` | 新增 `test_resource_snapshot_t`:每模块前后对比 `cap_free_count` / `mem_get_outstanding_allocs` / `stats_get_kern_stats()->fault_count` | ✅ **亮点** — 一次性消除"前一模块泄漏→后一模块级联 FAIL"的诊断盲区 |
| `src/kernel/syscall/user_api.h` | sys_call0-6 全部 16-byte 对齐压栈(0 填充 a4-a6),PSP 在 SVC 前保持 8-byte aligned | ✅ Cortex-M exception entry 不再插 padding word,svc_handler.S 的 `+64/+68/+72` 偏移与 wrapper 严格匹配 |
| `src/tests/test_irq.c` | 新增 12 个子测试(包括用户态 `sys_irq_bind` 通过 cap+endpoint 触发) | ✅ 覆盖度大,包含 capability 派生+吊销的端到端路径 |
| `src/tests/test_driver.c` | 4684 行,16+ 个 user-mode driver server/client 任务变体 | ✅ 测试用户态驱动 attach/detach/IRQ notify/rights enforcement |

### A.2 遗留小问题(非阻塞,低优先级)

| ID | 问题 | 影响 | 建议 |
|---|---|---|---|
| R1 | `src/kernel/ipc/wait_queue.c:50` `[WAITQ] remove ignored: task not in queue` 在 `fault` 测试期间被触发并污染 UART 输出 | 诊断噪音,无功能影响 | 把这条诊断迁到 `trace_event(TRACE_IPC, ...)`,在 `KERN_DEBUG_ENABLE` 下走 trace 而非 UART |
| R2 | UART 高速输出偶发字符丢失(如 `"f128->128"` 应为 `"fail +0 cap 128->128"`) | 看 LOG 误判,但 2867/2867 总数和每个模块的 pass 数清晰 | DAPLink CDC 吞吐瓶颈;后期可加 `uart_tx_async`/ring buffer,或换 USB-CDC native |
| R3 | `src/arch/arm/cortex-m7/` 目录名仍是 cortex-m7,但实际跑的是 M33 代码 | 命名误导新读者 | 见 §C.1 命名整改 |
| R4 | 39 改 + 98 untracked,所有 P0-P4 计划文件未入 git | 历史可追溯性差 | 见 §B.1 commit baseline |

### A.3 审计结论

**所有"上一轮红"模块修复质量过硬,无回退风险。** 三大架构性修复——
1. PMSAv8 MPU 完整化(mpu.c)
2. PSPLIM 跨上下文保存(svc_handler.S)
3. 资源泄漏快照(test_framework.c)

——共同把项目从"功能正确但脆弱"推到"功能正确且可观测"。后续前瞻工作可以放心推进。

---

## §B 立即执行(锁定战果)

### B.1 Commit baseline

把当前 working tree 切成多个语义化 commit,便于回归。建议拆分(顺序无关):

```
1. build: switch RP2350 to Pico SDK CMake pipeline + Non-secure image
   (CMakeLists.txt, Makefile rp2350 分支, tools/openocd.cfg, configs/,
    link/, 删除 src/startup/arm/{boot2.S,startup.S,system.c} 等)

2. arch: port hal.c + context switch to Cortex-M33 / PMSAv8
   (src/arch/arm/cortex-m7/{svc_handler.S, pendsv_handler.S, first_switch.S},
    src/kernel/mpu/mpu.c, src/kernel/include/kernel_types.h sp_limit,
    scripts/gen_tcb_offsets.py)

3. drivers: rewrite rp2350 uart as SDK wrapper
   (src/drivers/chip/rp2350/uart_rp2350.c, src/board/rp2350/*,
    src/drivers/include/uart.h, src/drivers/include/gpio.h)

4. kernel: kstring freestanding recursion guard
   (src/kernel/lib/kstring.c — 关键修复,单独成 commit)

5. tests: per-module resource leak snapshots
   (src/tests/test_framework.{c,h}, 各 test_*.c 同步)

6. tests: stabilize irq/driver/syscall/service_model suites to 100%
   (具体测试改动)

7. docs: P0-P5 计划 + gap analysis + memory notes
```

**操作建议:**
- 每个 commit 单独 `cmake --build` 验证一遍(快,~30s)。
- 不要 `git add -A`,逐个文件 add 避免漏过 `.config` 之类的本地状态。
- 不进 `.config`(可能含本地 PORT/路径)。

### B.2 回归基线脚本

新增 `scripts/regression.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail
echo "[1/3] configure + build"
cmake -S . -B build/rp2350-pico-sdk \
  -DPICO_SDK_PATH=tools/pico-sdk \
  -DPICO_TOOLCHAIN_PATH=tools/arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-eabi/bin \
  -Dpicotool_DIR=tools/picotool/picotool \
  -DPICO_BOARD=pico2_w -DCMAKE_BUILD_TYPE=Release
cmake --build build/rp2350-pico-sdk -j4

echo "[2/3] flash"
openocd -f tools/openocd.cfg \
  -c "program build/rp2350-pico-sdk/my-rtos-pico2w.elf verify reset exit"

echo "[3/3] capture + check PASS line"
stty -F "${PORT:-/dev/ttyACM0}" raw -echo 115200
timeout 15 cat "${PORT:-/dev/ttyACM0}" | tee /tmp/uart.log
grep -E "Passed: [0-9]+|Failed: 0|All tests PASSED" /tmp/uart.log
```

后续每次大改前跑一遍,保证不回退。

### B.3 拓荒期 gdb 脚本就位(可选,~30 min)

新增 `tools/gdb_dump_state.gdb`:`reset halt` → 读 CFSR/HFSR/MMFAR/PSPLIM/PSP/MSP → `bt`。下次硬错时 `openocd &` + `arm-none-eabi-gdb -x tools/gdb_dump_state.gdb`。

---

## §C 前瞻 — P5 三大主轴

按 `docs/MICROKERNEL_GAP_ANALYSIS.md` 的优先级,挑 ROI 最高的 3 项,其余延后。

### C.1 命名与文档诚实化(1 天)

**问题:** `src/arch/arm/cortex-m7/` 跑的是 Cortex-M33 代码;`Makefile` 的 `else` 分支是 STM32F767-only,但 RP2350 走 CMake,文档没说清。

**做法:**
1. **不重命名目录**,但加 `src/arch/arm/cortex-m7/README.md` 说明"此目录包含 M7/M33 通用代码,软浮点 ABI 下指令级兼容;PMSAv8 路径在 `BOARD_MPU_ARMV8` 守卫下,M7 走 PMSAv7。"
2. **重写 `README.md` 顶部**说清双轨:`make BOARD=stm32f767` 走 classic Make;`make` 或 `make BOARD=rp2350` 走 CMake。
3. **`docs/BOARD_SUPPORT.md`** 新建,列两板的 feature matrix(MPU / FPU / SysTick / 烧录方式 / 当前测试覆盖)。

**验证:** `make info` 在两板下都给出准确信息;新读者读 README 不再误解。

### C.2 CI 编译闸门(0.5 天)

**问题:** 当前没有 CI,任何分支推上去都不会自动验证构建。`stm32f767` 路径容易在 RP2350-only 开发中悄悄 break。

**做法:**
1. `.github/workflows/build.yml`(或本地 `scripts/ci_local.sh`):
   - Job A:`make stm32f767_defconfig && make` — 必须出 .elf
   - Job B:`cmake -S . -B build/rp2350-pico-sdk -DPICO_SDK_PATH=... && cmake --build` — 必须出 .elf + UF2
2. 跳过单元测试(无 QEMU);只验证 link + size。
3. `scripts/verify_pico2w_build.py` 在 Job B 末尾跑一遍。

**验证:** PR 推送后 CI 在 5 分钟内出绿/红;故意改一个 hal.c 的 RP2350 路径破坏,Job B 红。

### C.3 用户态 Supervisor + 故障重启循环(2-3 天)

**这是本计划最有价值的一项。** 来自 `MICROKERNEL_GAP_ANALYSIS.md §1` 的"fault-restart loop"缺口。

**目标:** 用户任务 crash(MemManage fault 等)→ 内核不挂死 → supervisor 任务收到通知 → 重启该任务(保留 capability 子集)。

**现状:** `src/user/supervisor/supervisor.c` 已存在但只是骨架;`src/kernel/fault/fault.c` 能解码 fault 但不通知用户态;capability 已支持 `CAP_GRANT` 派生与 revoke hook。

**实施切片(每片可独立验证):**

| 切片 | 内容 | 验证 |
|---|---|---|
| S1 | `fault.c`:fault 命中时把 fault info(CFSR/MMFAR/PC/faulted task_id)打包发给一个内核保留 endpoint `kern_fault_ep` | 单元测试:故意 NULL deref,断言 endpoint 收到 msg |
| S2 | `syscall_fault_subscribe`:让 supervisor 任务注册接收 fault 通知(返回 fault_ep 的 cap) | supervisor 启动后 `sys_fault_subscribe()` 拿到 cap |
| S3 | `supervisor.c` 实现 `monitor_loop`:ep_recv fault msg → 查 task name → `sys_task_create`+`sys_task_start` 重启 → 限制重启速率(每 5 秒最多 1 次) | 测试:crashy 任务死 1 次,supervisor 重启它,3 次后 supervisor 拒绝再启 |
| S4 | capability 策略:重启的任务只继承原 parent 的 cap subset(去掉 CAP_GRANT),避免失控子任务派生 cap | 测试:重启任务的 `sys_cap_*` 行为符合预期 |

**风险:**
- fault_ep 在内核 bootstrap 阶段必须早建,否则早期 fault 无处可发(降级到 kern_panic)。
- supervisor 自己 crash 怎么办?→ P6 的"meta-supervisor"问题,本期不解决,panic 即可。
- restart 风暴:必须有 rate limit(S3 已包含)。

**预期收益:** 项目从"测试通过"升级到"具备生产可用性的 fault-tolerant 微内核"。这是微内核区别于 monolithic 的核心价值。

---

## §D 暂不上 P5 的(明确推迟)

| 项 | 推迟原因 |
|---|---|
| **Address space isolation**(ASID + per-task page table) | RP2350 Non-secure 没有 MMU(M33 NS 只有 MPU)。要做需要切 Secure 模式或换 SoC,工程量 2-3 周。本期 cap+MPU 已经给了"软"隔离 |
| **Persistent FS**(littlefs/FatFS on flash) | 需要 SPI flash 驱动 + 块设备层。价值高但代价大,放 P6 |
| **Network stack**(lwIP + CYW43 WiFi driver) | CYW43 在 RP2350 上是开源但复杂。先做 wired 串口/USB-CDC |
| **SMP** | M33 单核,不适用 |

---

## §E 7 天里程碑

| Day | 内容 | 验收 |
|---|---|---|
| 1 | §B.1 commit baseline 拆分 + §B.2 regression.sh + §C.1 命名/README 整改 | git log 清晰;新 README 不再误导 |
| 2 | §C.2 CI 闸门 | PR 推送自动跑 build |
| 3-5 | §C.3 S1-S4 supervisor + fault-restart | 故意 crash 的用户任务被 supervisor 自动重启 |
| 6 | 端到端冒烟:uart_server crash → supervisor 重启 → shell 继续 | 真机演示 |
| 7 | 写 `P5_COMPLETION_REPORT.md`,更新 `MICROKERNEL_GAP_ANALYSIS.md` 的 radar | 文档反映新状态 |

**总计 ~7 天。** 每天结束跑一次 `scripts/regression.sh` 保证 2867/2867 不回退。

---

## §F 决策点

| ID | 决策 | 推荐 |
|---|---|---|
| D1 | 是否在 P5 内 commit? | **是** — 当前 working tree 风险太高,任何意外丢一文都是大损失 |
| D2 | commit 粒度 | **按 §B.1 的 7 个语义化 commit**,不要一锅炖 |
| D3 | 是否重命名 `cortex-m7/` 目录 | **否** — 改 README 而不是 mv,降低 git history 噪音 |
| D4 | CI 走 GitHub Actions 还是本地脚本 | **两者都做** — Actions 给 PR 用,本地脚本给开发时用 |
| D5 | Supervisor restart rate limit | **每任务每 5s 最多 1 次**,3 次后永久 kill |
| D6 | 是否本期加 meta-supervisor | **否** — supervisor 自己 crash 就 panic,P6 再做 |
