# My-RTOS → 完整可裁剪微内核操作系统 总路线图

> **战略目标**:把项目推进到"全面、可裁剪、双核、生产可靠的 capability-based microkernel OS"。
>
> **设计哲学**:**全面优先,裁剪驱动**。每个完整 OS 应有的子系统都必须存在(Kconfig 控制),
> 即使默认关闭。原则——**用户可以不用,但不能没有**。
>
> **当前基线**(2026-06-30):RP2350/Pico 2 W(Cortex-M33 NS,Core 0),2867/2867 测试通过,
> 20 个测试模块全绿,shell 上线。已有:cap + MPU + syscall + IPC + vfs + uart_server +
> driver framework + supervisor 骨架 + trace/stats。

---

## §0 战略框架

### 平台聚焦

| 平台 | 角色 | 投入 |
|---|---|---|
| **RP2350 / Pico 2 W** | 主战场 | 90% |
| STM32F767 Nucleo | 维护模式(只保证"能编") | 5% |
| 其他(Cortex-A 等) | 不考虑 | 0% |

### 6 条战略原则

1. **每个 Phase 必须有真机可见的验收条件** —— 不是"代码写完"而是"shell 上能跑通"
2. **不重写已有基础** —— cap / IPC / scheduler / MPU / syscall 五块只扩展不重写
3. **能力优先,性能其次** —— 微内核的价值在 fault isolation,不在 throughput
4. **全面优先,裁剪驱动** —— 每个子系统必须存在,通过 Kconfig `CONFIG_*` 控制编译。不存在"可选跳过"
5. **每 Phase 结束必跑 regression** —— 2867/2867 是底线,不许回退
6. **配置矩阵可验证** —— 至少 3 个预设配置必须能在 CI 上编过

### 配置体系(Kconfig 驱动的裁剪)

#### 三个配置预设

| 预设 | Flash | RAM | 包含子系统 | 用途 |
|---|---|---|---|---|
| `tiny` | < 64KB | < 16KB | kernel + cap + IPC + 1 UART driver + shell | 最小化,资源受限场景 |
| `default` | ~256KB | ~64KB | + persistent FS + supervisor + 2 个 driver server | 主开发配置 |
| `full` | ~1MB+ | ~128KB+ | + SMP + NET + ELF loader + PM + RT + dyn-link | 所有子系统开启 |

#### CI 配置测试矩阵

每个 PR 自动跑 3 个预设编译,任何预设 break 都阻止合并。这强制每个子系统真正独立可裁剪。

#### 子系统 Kconfig 依赖图(部分)

```
CONFIG_SMP               (独立)
CONFIG_NET                depends on CONFIG_SOCKETS
CONFIG_SOCKETS            depends on CONFIG_FD
CONFIG_ELF_LOADER         depends on CONFIG_FS_PERSISTENT
CONFIG_FS_PERSISTENT      depends on CONFIG_BLOCK_DEVICE
CONFIG_BLOCK_DEVICE       depends on CONFIG_SPI_DRIVER
CONFIG_RT_SCHED           depends on CONFIG_SCHED_PRIORITY_CLASSES
CONFIG_PM                 depends on CONFIG_TICKLESS_IDLE
CONFIG_DYNAMIC_LINKING    depends on CONFIG_ELF_LOADER + CONFIG_FS_PERSISTENT
CONFIG_SECURE_BOOT        depends on CONFIG_SIGNING_KEY
```

每个 `CONFIG_*` 必须有对应文档:why / how to enable / 资源占用 / 测试覆盖。

### 当前能力 vs 完整 OS 差距(快速对照)

| 维度 | 现状 | 完整 OS 目标 | 在哪个 Phase 完成 |
|---|---|---|---|
| Scheduler | ✅ 单核 priority + preempt | ✅ + RT class + SMP | Phase 6 / 9 |
| Capability | ✅ kernel mint | ✅ + user mint + RCU | Phase 2 / 6 |
| IPC | ✅ endpoint/channel/shm | ✅ + 跨核 IPI | Phase 6 |
| Memory isolation | ✅ MPU 8-region | MPU(无 MMU,接受) | — |
| Fault handling | ✅ decode + kill | + auto-restart loop | **Phase 2** |
| Drivers | UART only | UART+GPIO+I2C+SPI+Block | **Phase 3** |
| Filesystem | ramfs only | ramfs + persistent lfs | **Phase 4** |
| User runtime | syscall wrappers | + libc + ELF loader | **Phase 5** |
| **SMP** | Core 0 only | **Core 0 + Core 1** | **Phase 6** |
| Init / bootstrap | shell 启动测试 | init 进程 + boot script | Phase 2 |
| Network | 无 | WiFi + lwIP + socket(默认关) | **Phase 7** |
| Secure boot | 无 | signed image + panic persist | Phase 8 |
| Power management | 无 | sleep + deep-sleep + wakeup | Phase 9 |
| Real-time class | 无 | SCHED_FIFO / SCHED_RR 隔离 | Phase 9 |
| Dynamic linking | 无 | .so + dlopen(默认关) | Phase 9 |
| CI | 无 | GitHub Actions 3 配置矩阵 | **Phase 1** |

---

## §1 Phase 1 — 工程基础 + Kconfig 体系(1.5 周)

> **详细分步见 `P5_STABILIZATION_AND_FORWARD_PLAN.md`。** 这里只列纲要 + 新增 Kconfig 工作。

**目标:** 锁定基线 + 建立"全面可裁剪"的配置基础设施。

| 切片 | 内容 | 验收 |
|---|---|---|
| 1.1 | 拆 7 个语义化 commit | `git log` 清晰可读 |
| 1.2 | `scripts/regression.sh` 一键 configure-build-flash-capture-check | 5 分钟内出 PASS/FAIL |
| 1.3 | `tools/gdb_dump_state.gdb` 硬错 dump | 故意 fault 后能读 CFSR/PC/backtrace |
| 1.4 | `.github/workflows/build.yml` 双板 × 三配置编译闸门 | PR 推送 5 分钟内出 6 个绿勾 |
| 1.5 | **Kconfig 大整理** —— 每个 subsystem 加 `CONFIG_*` 开关,建立依赖图 | `make menuconfig` 可独立切换每个子系统 |
| 1.6 | **三个预设**:`configs/tiny_defconfig` / `default_defconfig` / `full_defconfig` | 三个配置都编过,flash 占用符合 §0 表 |
| 1.7 | **每个 `CONFIG_*` 加 docs/kconfig/ 文档**:why / 资源占用 / 测试覆盖 | 文档完整,无裸配置 |
| 1.8 | README + `docs/BOARD_SUPPORT.md` 双轨说明 | 新读者不误解 |

**退出条件:** `make regression` PASS + CI 跑 3 配置全绿 + 故意改坏任一子系统立即被 CI 拦。

---

## §2 Phase 2 — Fault-Tolerant 基础设施(2 周)

> **✅ COMPLETED (2026-07).** All four sub-sections (2.1 fault endpoint, 2.2
> supervisor monitor, 2.3 init process, 2.4 cap subset on restart) are landed
> and verified on Pico 2 W: 2918/2918 tests pass, a user-mode supervisor runs
> (blocked on the fault endpoint), and a crashy_app demo task is auto-restarted
> by the supervisor with exponential backoff. See `P5_PHASE2_COMPLETION_REPORT.md`.
> The notes below are the original design; the implementation differs in a few
> places (event-driven backoff via a timer bound to the fault ep instead of
> passive rate-window-wait; supervisor owns its recipe table on its own stack
> because USER tasks cannot access kernel SRAM).

> **微内核区别于 monolithic 的核心价值。** 没有这一步,前面 2867 个测试只是"代码正确性
> 测试",不是"系统可靠性测试"。

### 2.1 Fault endpoint(3 天)

- `src/kernel/fault/fault_endpoint.c`:bootstrap 阶段建保留 endpoint `kern_fault_ep`
- `fault.c` 命中 fault 时,把 `fault_info_t{task_id, task_name, cfsr, mmfar, pc, lr, sp}` 打包发送
- 新 syscall `SYS_FAULT_SUBSCRIBE`:让 supervisor 拿 fault_ep 的 read cap
- **faulted 任务处置**:`task_suspend` + 标记 `TASK_STATE_FAULTED`,等 supervisor 决策

### 2.2 Supervisor 真正实现(4 天)

**重写 `src/user/supervisor/supervisor.c`**(当前 259 行只是计数器骨架):

```c
void supervisor_loop(void) {
    fault_cap = sys_fault_subscribe();
    while (1) {
        fault_info_t fi;
        sys_ep_recv(fault_cap, &fi, -1);
        supervisor_handle_fault(&fi);  // restart or kill,rate-limited
    }
}
```

**Rate limit 策略:**
- 每服务每 5 秒最多 1 次重启
- 累计 3 次后永久 kill
- 重启间隔指数退避(1s/2s/4s/8s)

### 2.3 Init 进程(3 天)

**新增 `src/user/init/init.c`** —— bootstrap 后第一个用户任务:
- 启动 supervisor
- 启动 driver servers(读 `/flash/init.cfg`,Phase 2 期间硬编码)
- 启动 shell
- 自己退出

**`root_bootstrap.c` 改造:** 不再直接 spawn shell,改成 spawn init。

### 2.4 Cap subset for restart(2 天)

- `src/kernel/cap/cap_subset.c`:`cap_derive_for_restart(parent, child)` —— 复制 parent cap table 但去掉所有 `CAP_GRANT`
- 重启的任务不能再派生 cap,防止失控子任务链

### 2.5 Kconfig 预留

```kconfig
CONFIG_SUPERVISOR=y        # 默认开
CONFIG_INIT_PROCESS=y      # 默认开
CONFIG_FAULT_ENDPOINT=y    # 默认开
```

### Phase 2 退出条件

**端到端冒烟:**
1. 用户任务 `crashy_app` 故意 NULL deref
2. supervisor 3 秒内重启它
3. 重启 3 次后永久 kill
4. shell 全程响应,内核不挂
5. UART:`[SUP] crashy_app faulted (pc=0x...), restarting (1/3)` × 3 → `killed`

**回归:** 2867/2867 不退步。

---

## §3 Phase 3 — 真实驱动扩展(3 周)

> **证明 driver framework 不只是为 UART 设计的。** 每个驱动按相同模式:chip driver →
> user server → cap + endpoint → 用户任务通过 cap 操作。每个 server 一个 `CONFIG_*`。

### 3.1 GPIO server(3 天) — `CONFIG_DRIVER_GPIO`

- `src/drivers/chip/rp2350/gpio_rp2350.c`(SDK 包装)
- `src/user/drivers/gpio_server.c`
- API:`sys_gpio_request(pin, mode)` / `sys_gpio_set` / `sys_gpio_get` / `sys_gpio_bind_irq`
- **验收:** 用户任务通过 cap 点亮 LED

### 3.2 I2C server(5 天) — `CONFIG_DRIVER_I2C`

- `src/drivers/chip/rp2350/i2c_rp2350.c`
- `src/user/drivers/i2c_server.c`
- API:`sys_i2c_open(bus, addr)` / `sys_i2c_write` / `sys_i2c_read`
- **验收:** 用户任务通过 cap 读 BMP280 温度

### 3.3 SPI + Block device(7 天) — `CONFIG_DRIVER_SPI` + `CONFIG_BLOCK_DEVICE`

**Phase 4 持久化 FS 的前置。**
- `src/drivers/chip/rp2350/spi_rp2350.c`
- `src/drivers/block/spi_flash.c`:W25Q128 等 SPI NOR flash
- API:`block_read/write/erase(cap, sector, buf, count)`
- **验收:** 擦写 100 sector 后读回数据一致

### 3.4 Timer server + RTC(3 天) — `CONFIG_DRIVER_RTC`

- `src/user/drivers/timer_server.c`:软定时器代理
- `src/drivers/chip/rp2350/rtc_rp2350.c`:wall clock
- API:`sys_timer_create_periodic(period_ms, ep_cap)` / `sys_time_now()`

### Phase 3 退出条件

**4 个 server 各有独立测试模块**;shell 增加 `drivers` 命令列出当前注册的 server。`tiny` 配置下所有 driver server 关闭,内核仍跑。

---

## §4 Phase 4 — 持久化文件系统(2 周) — `CONFIG_FS_PERSISTENT`

> **掉电不丢数据。** 让 OS 真正"有状态"。

### 4.1 Block device layer

> 已在 Phase 3.3 完成。

### 4.2 littlefs 移植(5 天)

- `third_party/littlefs/`(Apache 2.0)编为 static library
- `src/kernel/vfs/lfs_glue.c`:VFS inode ops → lfs 调用适配
- mountpoint:`/` = ramfs(系统/	tmp)/ `/flash` = littlefs(持久化)/ `/dev` = devfs

### 4.3 fs_server 扩展(3 天)

- 现有 `src/user/fs/fs_server.c` 加多 mount 支持
- `open("/flash/data.txt", ...)` 自动路由到 littlefs
- 新增 `sys_mount(path, fs_type, source)`(仅 root init 可调)

### 4.4 掉电一致性测试(2 天)

1. 写 `/flash/test.bin`(1KB)→ sync → 软复位 → 读回验证

### Phase 4 退出条件

`/flash/config.ini` 写入 → reset → supervisor init 读回内容并打 log。**用户能感知"OS 记住了上次的状态"。**

---

## §5 Phase 5 — 应用运行时(3 周) — `CONFIG_ELF_LOADER`

> **让"写应用"和"写内核"完全分离。**

### 5.1 用户态 libc(7 天) — `CONFIG_USER_LIBC`

**新增 `src/user/libc/`:**
- `printf.c` / `sprintf.c` / `malloc.c` / `free.c` / `string.c` / `errno.c`
- port [mini-libc](https://github.com/git-blind/mini-libc)(Public Domain)
- 编为 `userlibc.a`,应用 `-luserlc` 链接

### 5.2 ELF loader(7 天)

**新增 `src/kernel/proc/elf_loader.c`:**
- `sys_proc_exec(path, argv)` 从 `/flash/apps/<name>.elf` 加载
- 解析 ELF header、PT_LOAD、relocations(R_ARM_ABS32)
- 给新 proc 建独立 cap table(只继承 stdin/stdout + 显式 grant)

**RP2350 NS 约束(接受):**
- 无 MMU,只有 MPU(8 region,已用 5)
- 多 ELF 进程在**同一物理地址空间**但 MPU 隔代码/数据区
- 进程数上限受 MPU region 数限制(实际 ~3-4 个并发用户进程)
- 不支持 fork、不支持 ASLR、不支持动态链接(动态链接留 Phase 9)

### 5.3 Demo 应用(3 天)

**新增 `apps/hello.c`:**
```c
#include "userlibc.h"
int main(int argc, char **argv) {
    printf("hello from app! argc=%d\n", argc);
    return 0;
}
```

**shell 命令:** `exec hello arg1 arg2`
**构建流程:** `apps/Makefile` 编每个 .c → .elf;`apps/install.sh` 拷到 `/flash/apps/`

### Phase 5 退出条件

shell `exec hello foo bar` → 内核 ELF loader 加载 → 打印 "hello from app! argc=3" → 正常 exit 资源回收。

---

## §6 Phase 6 — SMP 双核(4 周) — `CONFIG_SMP`

> **从原 Phase 8 提升。** 用户明确要求"真双核"。RP2350 是 2 × Cortex-M33,目前只用 Core 0。
> 启用 Core 1 触发多个核心子系统的彻底重写,但**这是必做项,不是可选项**。

### 为什么放在 Phase 6(而不是更早或更晚)

- **不在 Phase 2-5 中间做**:cap / scheduler / IPC 还在演化,中间插入会反复重写
- **不推到 Phase 9 之后**:用户明确要求,且 SMP 影响所有上层子系统(ELF loader、net、PM)
- **Phase 5 完成后做**:ELF loader 定义了"用户进程"的最终形态,SMP 改造可以一次到位

### 6.1 SMP 原语(5 天)

**新增 `src/kernel/smp/`:**
- `spinlock.h`:`spinlock_t` + `spin_lock` / `spin_unlock` + `spin_lock_irqsave`
- 基于 M33 `LDREX` / `STREX` 原语
- `atomic.h`:`atomic_t` + `atomic_inc` / `atomic_cas`
- `percpu.h`:`CPU_LOCAL` 宏 + `smp_processor_id()`
  - 单核时(CONFIG_SMP=n):`CPU_LOCAL` 退化为全局变量
  - 多核时(CONFIG_SMP=y):每核独立 section,经 `TPR` / M33 `MSR` 寻址

### 6.2 Secondary core boot(4 天)

- Core 0 走 boot2 → reset_handler → main(现状)
- Core 0 通过 mailbox 启动 Core 1:写 `0x0000ffff` 到 `SIO + 0xd0`(Core 1 boot vector)
- Core 1 走 `secondary_reset.S`:配置自己的 MSP、向量表、PSPLIM,跳到 `secondary_init()`
- `secondary_init()`:初始化自己的 per-CPU 数据,加入 scheduler,进入 idle loop

### 6.3 SMP scheduler(7 天)

**`src/kernel/core/scheduler.c` 重写:**
- per-CPU run queue(每核独立 `cpu_rq_t`)
- per-CPU current task(`CPU_LOCAL tcb_t *_current_task`)
- work stealing:idle CPU 从其他 CPU 的 run queue 末尾偷任务
- affinity:task 默认可迁移;`task_set_affinity(tid, mask)` 锁核
- 调度锁:per-CPU `cpu_rq_lock`(spinlock),不是全局

### 6.4 跨核 IPC + cap(7 天)

**`src/kernel/ipc/endpoint.c` 改造:**
- `task_resume(target_tcb)`:检查 target->cpu_affinity,如果不在当前核 → 触发 mailbox IPI
- mailbox IRQ handler:目标核收到 IPI → 重扫自己的 wait queue

**`src/kernel/cap/capability.c` 改造:**
- cap table 加 RCU reader(per-CPU read-copy-update epoch)
- writer 走全局 spinlock,但 reader 无锁
- cap mint / revoke 是 writer,fault handler / syscall 检查是 reader

### 6.5 调试工具(3 天)

- shell `top` 命令:每核利用率 / 当前 task / run queue 深度
- gdb 多核同步:`openocd -c "dap create dap.* -chain-position 0/1"` 同时挂两核
- 故意触发其中一核 fault,验证另一核不挂

### Phase 6 退出条件

- Core 0 和 Core 1 同时跑不同 task 互不干扰
- 跨核 IPC 工作(C0 task → endpoint → 唤醒 C1 task)
- shell `top` 显示两核利用率
- 故意触发其中一核的 fault 不影响另一核
- **`CONFIG_SMP=n` 时仍能单核跑(裁剪保证)**

---

## §7 Phase 7 — 网络栈(4 周) — `CONFIG_NET`

> **从"可选"改成"必做但默认关"。** "可裁剪全面 OS"不能没有网络栈,即使用户场景暂时不需要。

### 7.1 CYW43 WiFi bus driver(7 天) — `CONFIG_DRIVER_CYW43`

- RP2350 的 CYW43 firmware(63KB)启动时加载
- 用 SDK cyw43-driver 包成 server
- firmware 来源:`/flash/cyw43_fw.bin`

### 7.2 lwIP port(7 天) — `CONFIG_LWIP`

- `third_party/lwip/`(BSD-style)
- syscall-based socket:`sys_socket` / `sys_connect` / `sys_send` / `sys_recv`
- net_server:接收用户 socket 操作

### 7.3 DHCP + DNS(3 天)

- 启动时自动 DHCP,提供 `sys_ifconfig()` 查询
- DNS resolver:`sys_gethostbyname(name) → ip4_addr_t`

### 7.4 TLS(可选,3 天) — `CONFIG_MBEDTLS`

- port mbedTLS(BSD-style)给 HTTPS 用
- 默认关,资源受限场景可不开

### 7.5 Demo:HTTP fetch(4 天)

**`apps/httpget.c`:**
```c
int main(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    connect(s, "example.com", 80);
    send(s, "GET / HTTP/1.0\r\n\r\n", 18, 0);
    char buf[256];
    int n = recv(s, buf, sizeof(buf), 0);
    buf[n] = 0;
    printf("%s", buf);
    return 0;
}
```

### Phase 7 退出条件

shell `exec httpget` → 自动连接 WiFi → DHCP → DNS → TCP → HTTP 请求 → UART 打印 example.com 前 100 字节。`CONFIG_NET=n` 时整个网络栈不编入镜像。

---

## §8 Phase 8 — 安全 & 可靠性(2 周)

> **让 OS 适合做产品。** 没有这一步,Phase 1-7 只是 demo。

### 8.1 Image signing & secure boot(3 天) — `CONFIG_SECURE_BOOT`

- RP2350 支持 signed boot image(SDK `pico_sign_image`)
- Ed25519 keypair(`tools/genkeys.sh`)
- 签名 boot2 + 应用镜像
- 启动时验签,失败则拒绝执行

### 8.2 Panic dump 持久化(3 天) — `CONFIG_PANIC_LOG`

- panic 时把 backtrace(64 字节 PC/LR/SP)写到 `/flash/panic.log` 最后一个 sector
- 上电时 supervisor 检查 panic.log,有则上报(UART 打印 + 计数)
- shell 命令 `paniclog` 查看

### 8.3 Profiler(2 天) — `CONFIG_PROFILER`

- 基于 `stats` 模块采样 PC(每 1ms 在 SysTick hook 里读)
- 导出到 `/flash/profile.dat`(flamegraph 兼容格式)
- shell 命令 `profile start/stop/dump`

### 8.4 OTA update(3 天) — `CONFIG_OTA`

- 新镜像分块写到 `/flash/ota/staged.bin`
- 校验签名后原子切换 boot slot
- 双 bank scheme(fallback 到旧 bank)

### Phase 8 退出条件

签名镜像上电验签通过;故意 panic 后 reset,能从 flash 读出 backtrace;profile 数据能生成 flamegraph;OTA 双 bank 切换可演示。

---

## §9 Phase 9 — 全面化补完(3 周)

> **"可裁剪全面"的"全面"必须做到位。** 把之前因"非主线"推迟的子系统补完,全部默认关。

### 9.1 Power management(5 天) — `CONFIG_PM`

- `sys_pm_request(state)`:WFI / SLEEP / DEEP-SLEEP
- 自动 idle:所有核进入 idle → tickless → WFI
- wakeup source:GPIO edge / RTC alarm / UART break
- 驱动 hook:`driver_suspend` / `driver_resume`
- **验收:** OS 在 idle 1 秒后进入 WFI,GPIO 中断唤醒

### 9.2 Real-time scheduling class(5 天) — `CONFIG_RT_SCHED`

- 引入 scheduling class:`SCHED_NORMAL` / `SCHED_FIFO` / `SCHED_RR`
- RT 任务优先级永远高于 normal,且不可被 preempt(除更高优先级 RT)
- `sys_setscheduler(tid, policy, priority)`
- **验收:** RT 任务的 jitter < 100μs @ 1Hz 周期

### 9.3 Dynamic linking(5 天) — `CONFIG_DYNAMIC_LINKING`

- `.so` 格式:基于 ELF,加 PLT/GOT
- `sys_dlopen(path)` / `sys_dlsym(handle, name)`
- 默认关(资源占用大,大多数场景用静态 ELF 即可)
- **验收:** `apps/plugin.c` 编为 `.so`,`exec loader` 加载后 dlopen+dlsym 调用插件函数

### 9.4 Formal verification hooks(2 天) — `CONFIG_FV`

- 不是真做形式化验证(那是研究项目)
- 提供 `ASSERT_INVARIANT` 宏 + invariant 检查 hook
- 关键不变量(scheduler ready queue 一致性 / cap table 无环 / 等)插入运行时检查
- `CONFIG_FV=y` 时启用,~5% 性能损失;`CONFIG_FV=n` 时零开销

### 9.5 GUI subsystem(可选,看硬件)— `CONFIG_FB`

- 仅当后续接 display 硬件(SSD1306 OLED over I2C / ILI9341 over SPI)时做
- frame buffer server + 基本绘图 primitive
- 默认关,不在 3 周时间预算内;留作"如果有人贡献"的开放项

### Phase 9 退出条件

所有 5 个子系统各有 `CONFIG_*` 控制;`full` 配置下全部开启可编可跑;`default` 配置下全关仍跑 2867/2867。

---

## §10 时间线总览

| Phase | 时长 | 累计 | 关键交付 | 用户感知 |
|---|---|---|---|---|
| **1** | 1.5w | 1.5w | CI 矩阵 + Kconfig 体系 + commit | 开发体验 |
| **2** | 2w | 3.5w | fault restart + init proc | crash 自动恢复 |
| **3** | 3w | 6.5w | GPIO/I2C/SPI/Block drivers | 真实硬件 IO |
| **4** | 2w | 8.5w | littlefs 持久化 | 掉电不丢数据 |
| **5** | 3w | 11.5w | ELF loader + user libc | 写独立 app |
| **6** | 4w | 15.5w | **SMP 双核** | **两核并行** |
| **7** | 4w | 19.5w | WiFi + lwIP + socket | 联网 |
| **8** | 2w | 21.5w | signed boot + panic persist + OTA | 产品级 |
| **9** | 3w | 24.5w | PM + RT + dyn-link + FV | 全功能 |

**总计 ~24.5 周(6 个月)**。无跳过项,所有子系统必须做。

---

## §11 风险登记

| 风险 | Phase | 概率 | 缓解 |
|---|---|---|---|
| Kconfig 依赖图维护复杂 | 1 | 中 | 用 Kconfig 图工具自动生成 docs/kconfig/dep_graph.png |
| Supervisor restart 风暴 | 2 | 中 | Rate limit + 3-strike rule + 指数退避 |
| SPI flash 写疲劳 | 3.3 | 低 | littlefs 自带 wear-leveling |
| littlefs 掉电丢数据 | 4 | 中 | littlefs v2.10+ power-cut safety + 100 次掉电测试 |
| ELF loader 地址空间冲突 | 5 | 高 | 单 ELF base,MPU 严格隔离,不做 ASLR |
| 用户 libc 性能太差 | 5 | 中 | port 成熟 mini-libc |
| **SMP 调度器重写踩坑** | **6** | **高** | Phase 6.1 原语先行 + 6.2 asymmetric boot 验证启动链路 |
| **跨核 cap RCU 实现复杂** | **6** | **高** | 退化到全局 spinlock 也接受,后期再优化 |
| CYW43 firmware 加载失败 | 7 | 高 | firmware 放外部 flash,签名校验,失败则禁用 wifi |
| OTA 双 bank 切换 brick | 8.4 | 中 | fallback bank 永远保留可启动镜像 |
| **6 个月内做不完** | **全** | **高** | Phase 9 内 5 个子项可按需分批;Phase 1-8 是 must |
| STM32F767 双轨维护成本 | 全 | 中 | Phase 2 后只保证"能编",CI 验证 |

---

## §12 关键决策点

| ID | 决策 | 推荐 | 替代方案 |
|---|---|---|---|
| D1 | Phase 顺序 | **1→2→3→4→5→6→7→8→9 全做** | — |
| D2 | 平台聚焦 | **RP2350 主战场** | 双板并行(慢 2-3 倍) |
| D3 | 用户 libc | **port mini-libc**(PD license) | 自写(2-3 倍工作量) |
| D4 | ELF 加载模式 | **静态链接单 ELF**(Phase 5)+ 动态链接(Phase 9) | — |
| D5 | 多任务并发数 | **3-4 个用户进程上限**(MPU 限制,单核时) | SMP 后受 task slot 上限 |
| **D6** | **SMP 时机** | **Phase 6 必做** | 永远延后(违背用户目标) |
| **D7** | **网络** | **Phase 7 必做但默认关** | 永远延后(违背"全面") |
| D8 | Supervisor restart 策略 | **3 次/永久 kill + 指数退避** | 无限重启(危险) |
| D9 | littlefs vs FatFS | **littlefs**(power-cut safe 内建) | FatFS |
| D10 | Secure boot 算法 | **Ed25519**(RP2350 ROM 支持) | RSA |
| **D11** | 配置预设数量 | **3 个**(tiny/default/full) | 5+(维护成本太高) |
| D12 | RT scheduling 策略 | **FIFO + RR**,不做 EDF(过早优化) | EDF |
| **D13** | GUI 子系统 | **看硬件**,无 display 不做 | 强行做虚拟 fb |
| D14 | 动态链接默认 | **默认关**(CONFIG_DYNAMIC_LINKING=n) | 默认开(资源浪费) |
| D15 | OTA 双 bank vs A/B partition | **A/B partition**(更简单,失败可回滚) | 双 bank in-place |

---

## §13 与 `MICROKERNEL_GAP_ANALYSIS.md` 的映射

| Gap 分析文档 § | 本路线图 Phase |
|---|---|
| §1 战略差距:address space isolation | **不解决**(RP2350 NS 无 MMU) |
| §1 战略差距:cap minting from user | Phase 5(ELF loader 用到) |
| §1 战略差距:VM mapping / mmap | **不解决**(同上) |
| §1 战略差距:fault-restart loop | **Phase 2** |
| §1 战略差距:**SMP** | **Phase 6** |
| §2 系统差距:drivers | **Phase 3** |
| §2 系统差距:FS | **Phase 4** |
| §2 系统差距:network | **Phase 7** |
| §2 系统差距:user libc | **Phase 5** |
| §2 系统差距:PM | **Phase 9.1** |
| §3 服务差距:supervisor | **Phase 2** |
| §3 服务差距:init proc | **Phase 2.3** |
| §3 服务差距:其他 server(fs/pm/loader) | 分散在 Phase 2/4/5 |
| §4 工程差距:CI | **Phase 1.4** |
| §4 工程差距:arch 命名 | **Phase 1.8** |
| §4 工程差距:secure boot | **Phase 8.1** |
| §5 RT scheduling | **Phase 9.2** |
| §5 dynamic linking | **Phase 9.3** |

---

## §14 立即行动建议

按这个路线图,下周一就可以开工。具体第一步:

1. **拍板决策点 D1-D15**(尤其 D6 SMP 已确认 + D7 网络已确认 + D11 配置预设数量)
2. **执行 Phase 1**(1.5 周内 commit baseline + Kconfig 体系 + CI 三配置矩阵上线)
3. **进入 Phase 2**(fault-tolerant 是最有价值的一项)
4. **每 Phase 结束写 COMPLETION_REPORT + 更新本 roadmap 的"已完成"标记**

> Phase 1 的细节已经在 `P5_STABILIZATION_AND_FORWARD_PLAN.md` 中展开。
> Phase 2-9 的细节在执行到那个 Phase 时再展开子文档(`P6_FAULT_TOLERANT_PLAN.md`、
> `P7_SMP_PLAN.md` 等)。

---

## §15 永远不做(只剩这些)

这些项**即使全面完成后也不做**:

| 项 | 原因 |
|---|---|
| **真 MMU / Address space isolation** | RP2350 NS 硬件不支持。要做需切 Secure mode(支持 Armv8-M MMU)或换 Cortex-A SoC。超出"RP2350 主战场"范围 |
| **Formal verification(seL4 风格)** | 研究项目,Isabelle/HOL + 数月工作量。超出工程目标 |
| **SMP > 2 核** | RP2350 只有 2 个 M33。换 SoC 才能扩展 |
| **POSIX 兼容层** | 不是目标。微内核应做 capability-based,不做 UNIX emulation |
| **Multiple architecture ports(x86/RISC-V/...) ** | 单 SoC 聚焦。要做先 fork 项目 |

---

**最后:** 这个路线图是 living document。每完成一个 Phase 回来更新"已完成"列,并按实际经验
调整后续 Phase 的估计和决策。
