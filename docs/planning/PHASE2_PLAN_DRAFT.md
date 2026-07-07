# Phase 2 余下部分 — 实施计划

**范围**:supervisor monitor_loop(§2.2)+ init 进程(§2.3)+ cap subset on restart(§2.4)+ review Important#1 修复
**前置**:S1(`fault_endpoint.c/.h` + `sys_fault_subscribe`)已在 working tree
**目标**:用户任务 crash → 内核不挂 → supervisor 收 fault_event_t → 按策略重启 → 3 次后永久 kill,shell 全程响应(roadmap §2 退出条件)

---

## 已确认决策

| 项 | 决策 |
|---|---|
| supervisor 运行模式 | **user-mode**(走真 SVC 路径) |
| S4 cap subset | **做** —— 新增 `cap_subset.c` + `sys_task_restart` syscall |
| init 进程 | **新增 `src/user/init/init.c`** |
| 重启配方来源 | **supervisor 内置注册表**(Phase 2 硬编码) |
| reduced cap 注入 | **新增 `SYSCALL_TASK_RESTART=72`**,kernel 侧调 `cap_derive_for_restart` 强制装进新任务 cspace |
| fault 身份识别 | **fault_event_t 内嵌 `task_name`**(fault.c notify 前 current->name 还在) |

---

## 关键事实(已查证,塑造计划形态)

1. **live boot 链路**:`main → test_runner_start → test_runner_task(kernel-mode)→ test_run_all_modules() → shell_start()`。`root_bootstrap` 路径只在测试里被调,从未进 boot 链 → §2.3 是**新建 init 链路**。
2. **TCB 不存 entry/arg/parent**(entry 只活在初始异常帧)。terminate 后 `task_reclaim_expired` memset TCB → 重启配方必须 supervisor 自持。
3. **重启=terminate→从头重建**。`cap_revoke_all` 清光任务所有 cap,无"原 cap 快照"可过滤。
4. **`cap_derive` 装进调用者自己 cspace**(`cap_derive_for(sched_get_current(),...)`)。无 user syscall 能注入他人 → 必须新 syscall 在 kernel-side 绕行。
5. **drop-CAP_GRANT 原语已存在**(`cap_derive_for` 强制 `rights & ~parent->rights == 0`),无需新 masking helper。
6. **fault 注入**:`kern_fault_notify(...)` 是 public 非静态符号,可在 kernel-resident 测试直接调。
7. **测试**:kernel-resident(`TEST_MODULE_REGISTER` + `.test_modules` section)。CI 只 build;`scripts/regression.sh` 硬件跑 + grep `All tests PASSED`。"2867" 是文档基准非代码断言。

---

## 工作分解(按依赖排序的提交切片)

### Slice A — review Important#1 修复(先行,最小)

修 `src/kernel/fault/fault_endpoint.c` drain 循环:去掉 release/re-acquire crit。
- 现状:`while(...) { snapshot; tail++; exit_crit; notify; enter_crit; } exit_crit;`
- 改为:`enter_crit; while(...) { snapshot; tail++; notify; } exit_crit;`
- 理由:PRIMASK save/restore 可重入;`endpoint_notify` 内部自己管 crit,逻辑嵌套安全;消除 producer overwrite 分支与 reader 并发改 tail 的 race。
- 顺带:函数内 `extern sched_get_tick_count` 移到文件顶部声明。

**验收**:`make`(rp2350 CMake + stm32 Make)通过;现有 fault 测试不退步。

---

### Slice B — `sys_task_restart` syscall + `cap_derive_for_restart`(S4 落点)

**新增 `SYSCALL_TASK_RESTART = 72`**(`syscall.h`),封装 kernel-side 原子重建:
```c
sys_task_restart(const char *name, void(*entry)(void*), void *arg,
                 int prio, int stack_size, uint8_t cap_rights_mask)
  → 返回新 task 的 cap_id(或负 kern_err_t)
```
kernel handler `sys_task_restart`(`syscall.c`):
1. `strncpy_from_user(name)`、`user_access_ok(entry, READ)`
2. `task_create_user(...)` 得 new_tcb
3. `#if CAP_ENABLE`:从 supervisor(当前任务)持有的同名 parent TASK cap 出发,仅在带 `CAP_GRANT` 时允许;调 **`cap_derive_for_restart(supervisor, parent_cap, new_tcb, cap_rights_mask)`** 把 reduced cap 装进 **new_tcb** cspace
4. `#if CAP_RESTART_SUBSET` 关闭时:退化为不注入 cap(行为同前,语义安全)
5. `task_start(new_task)`;返回 new task 的 TASK cap

**新增 `src/kernel/cap/cap_subset.c` + `cap_subset.h`**(`#if CAP_RESTART_SUBSET`):
```c
cap_id_t cap_derive_for_restart(tcb_t *supervisor, cap_id_t parent_cap,
                                tcb_t *new_task, uint8_t rights);
```
实现:校验 supervisor 持 parent_cap 且 parent 带 CAP_GRANT → `rights &= ~(CAP_GRANT)` → `rights &= parent->rights` → 用 `cap_init_child_slot` + `cap_task_add(new_task, child_cap)` **强制装进 new_task**。复用 capability.c 内部函数(需把这些 helper 从 static 暴露或加友元声明)。

**Makefile**:`KERN_SOURCES += src/kernel/cap/cap_subset.c`。
**Kconfig**:`CAP_RESTART_SUBSET` 已定义(Kconfig:817),`full_defconfig` 已开,`kernel_config.h` 已 #define —— **无需改 Kconfig**,首次让 `#if CAP_RESTART_SUBSET` 有真代码。
**SYSCALL_TABLE_SIZE**:71→72(Kconfig default + 各 defconfig + kernel_config.h 已是 72/128,核对即可)。
**user_api.h**:加 `sys_task_restart` 内联封装。

**验收**(加进 `test_capability.c`,`#if CAP_RESTART_SUBSET`):supervisor 持 CAP_FULL TASK cap → `cap_derive_for_restart(sup,sup_cap,new,CAP_FULL)` → 断言 new->cap_set 有 1 项且 rights==`CAP_FULL & ~CAP_GRANT`;supervisor 自己 cspace 不增;rights 越界时被拒。

---

### Slice C — fault_event_t 内嵌 task_name + supervisor monitor_loop(§2.2 核心)

**改 `src/kernel/fault/fault_endpoint.h`**:`fault_event_t` 加 `char task_name[KERN_TASK_NAME_LEN]`(或 16,看 sizeof)。**重算 `_Static_assert`**:`sizeof(fault_event_t) <= IPC_EP_MSG_SIZE`(当前 128,够)。`kern_fault_notify` 签名加 `const char *task_name` 参数。
**改 `src/kernel/fault/fault.c`**:notify 前 `current->name` 仍在(terminate 设 TERMINATED 未 reclaim),拷进 event。fault.c:309 调用点传 `current->name`。

**改 `src/user/supervisor/supervisor.c`**:在现有纯状态库上加运行时入口。
```c
typedef struct {
    const char *name; void (*entry)(void*); void *arg;
    uint8_t priority; uint32_t stack_size; uint8_t cap_rights_mask;
    uint32_t restart_count; uint32_t last_restart_tick; uint32_t backoff_ms;
} supervisor_recipe_t;
static supervisor_recipe_t recipes[SUPERVISOR_SERVICE_MAX];
```
API:
- `supervisor_register_recipe(name, entry, arg, prio, stack, cap_rights_mask)` —— init.c 启动时硬编码注册
- `void supervisor_monitor_loop(void *arg)`(新入口,user 任务体):
  ```c
  int ep = sys_fault_subscribe();           /* S1 已在 */
  fault_event_t evt;
  while (1) {
      if (sys_ep_recv(ep, &evt, 1000) < 0) continue;
      supervisor_handle_fault(&evt);
  }
  ```
- `supervisor_handle_fault(evt)`:用 `evt.task_name` 查 recipe → rate-limit(每 5s 最多 1 次,指数退避 1s/2s/4s/8s)→ `restart_count<3` 用 `sys_task_restart(...)` 重启 → `>=3` 永久 kill + 日志 → 不在表只记录

**UART 日志**:supervisor 用现有 uart_server 路径打 `[SUP] <name> faulted (pc=0x...) restarting (1/3)`;若无可用 printf 路径,降级为静默,测试用别的方式断言(记 known-limitation)。

**验收**(新 `test_supervisor_monitor.c`,`#if FAULT_ENDPOINT && SUPERVISOR`):
- **推荐先做注入式**(确定性):kernel-mode test task 调 `kern_fault_notify(...)` 伪造 crashy_app fault ×3 → 断言 supervisor restart_count 与 kill 决策、指数退避时序
- 真硬件 fault 留冒烟(Slice F)

---

### Slice E — init 进程(§2.3)

**新增 `src/user/init/init.c`**:
```c
void init_main(void *arg) {
    supervisor_register_recipe("crashy_app", crashy_app_entry, ...);
    /* ... 其它 recipe ... */
    int sup = sys_task_create("supervisor", supervisor_monitor_loop, NULL, 2, 2048);
    sys_task_start(sup);
    sys_task_exit(NULL);   /* init 自己退出 */
}
```
**改 boot 链路**:`test_runner_start` 在 `#if INIT_PROCESS` 下,用 `root_bootstrap_create("init", init_main, ...)` + `root_bootstrap_start()` 起 init;**shell 链路暂不动**(shell 仍由 test_runner kernel-mode 起 —— 见风险)。
**Kconfig**:`INIT_PROCESS` 已定义、已开,无需改。
**Makefile**:`APP_SOURCES += src/user/init/init.c`。

**验收**:启动后 supervisor 任务存在;crashy_app 被 supervisor 管理。回归 2867 不退步。

---

### Slice F — 端到端冒烟 + 文档

- **新增 `src/user/apps/crashy_app.c`**:NULL deref user 任务(demo + 冒烟用)
- **`scripts/regression.sh`**:已是硬件跑 + grep,无需改;确认 `full` preset 下新测试模块编进去
- **文档**:更新 `MICROKERNEL_OS_ROADMAP.md` §2(S2/S3/S4/init 标完成)、`docs/MICROKERNEL_GAP_ANALYSIS.md` radar、新增 `P5_PHASE2_COMPLETION_REPORT.md`、`docs/BOARD_SUPPORT.md` 测试基线

**退出条件**(roadmap §2):
1. crashy_app NULL deref → supervisor 收到 ✓
2. supervisor 重启(5s 窗口 + 指数退避)✓
3. 3 次后永久 kill ✓
4. shell 全程响应 ✓(链路不动)
5. UART `[SUP] crashy_app faulted (pc=0x...) restarting (1/3)` ×3 → `killed` ✓
6. 回归 2867/2867 不退步 ✓

---

## 风险与回退

| 风险 | 缓解 |
|---|---|
| user-mode supervisor 无法注入 cap(事实4) | Slice B 新 syscall kernel-side 绕行 + Slice D `cap_derive_for_restart` 强制装 new_tcb |
| shell 变 user-mode 会崩(直接调 kernel 函数) | Slice E **不动 shell 链路**,只 spawn supervisor;shell 拆 user-mode 留 Phase 3 |
| fault_event_t 加 name 打破 sizeof 约束 | `KERN_EP_MSG_SIZE=128` 远大于 event;`_Static_assert` 改为 `<= IPC_EP_MSG_SIZE`(已核实够装) |
| 真硬件 fault 时序 flaky | Slice C 优先 `kern_fault_notify` 注入做确定性单测;真 fault 只硬件冒烟 |
| root_bootstrap 改 boot 链路影响回归 | `#if INIT_PROCESS` 门控,默认 config 不变,仅 full preset 启新链路 |
| `cap_init_child_slot`/`cap_task_add` 是 static | Slice D 用友元声明或最小暴露(不改公共 API 语义) |

## 不做(明确排除)

- meta-supervisor(supervisor 自己 crash → panic,P6)
- user-mode shell(Phase 3)
- persistent FS / 网络 / SMP / address-space isolation(各自后续 Phase)
