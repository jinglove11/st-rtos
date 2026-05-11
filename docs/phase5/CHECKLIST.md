# Phase 5: IPC 升级 — 完成表

> 日期: 2026-05-08
> 设计文档: `docs/phase5/DESIGN.md`
> 基线: 452 tests, 0 failures
> 最终: 489 tests, 0 failures (+37 新测试)

---

## Step 1: wait_queue 提取 + syscall 补全

### 1.1 wait_queue 提取

| # | 任务 | 文件 | 状态 |
|---|------|------|------|
| 1.1.1 | 创建 `wait_queue.h` — API 声明 | `src/kernel/ipc/wait_queue.h` | ✅ |
| 1.1.2 | 创建 `wait_queue.c` — 4 个函数实现 | `src/kernel/ipc/wait_queue.c` | ✅ |
| 1.1.3 | `semaphore.c` 删除 static 实现，include wait_queue.h | `src/kernel/ipc/semaphore.c` | ✅ |
| 1.1.4 | `mutex.c` 同上 | `src/kernel/ipc/mutex.c` | ✅ |
| 1.1.5 | `mqueue.c` 同上 | `src/kernel/ipc/mqueue.c` | ✅ |
| 1.1.6 | `event.c` 同上 | `src/kernel/ipc/event.c` | ✅ |
| 1.1.7 | Makefile 添加 `wait_queue.c` | `Makefile` | ✅ |
| 1.1.8 | 编译验证 0 warnings | — | ✅ |
| 1.1.9 | 回归测试全部通过 (452 tests) | — | ✅ |

### 1.2 syscall 补全

| # | 任务 | 文件 | 状态 |
|---|------|------|------|
| 1.2.1 | `kernel_types.h` — 添加 `BLOCK_REASON_EP`, `BLOCK_REASON_CH` | `src/kernel/include/kernel_types.h` | ✅ |
| 1.2.2 | `kernel_types.h` — 添加 `ep_id_t`, `ch_id_t` 类型 | `src/kernel/include/kernel_types.h` | ✅ |
| 1.2.3 | `syscall.h` — 添加 `SYSCALL_MUTEX_DELETE` (22) | `src/kernel/syscall/syscall.h` | ✅ |
| 1.2.4 | `syscall.h` — 添加 `SYSCALL_MQUEUE_DELETE` (34) | `src/kernel/syscall/syscall.h` | ✅ |
| 1.2.5 | `syscall.h` — 添加 `SYSCALL_EVENT_DELETE` (35) | `src/kernel/syscall/syscall.h` | ✅ |
| 1.2.6 | `syscall.h` — 添加 `SYSCALL_EVENT_CLEAR` (36) | `src/kernel/syscall/syscall.h` | ✅ |
| 1.2.7 | `syscall.h` — 添加 `SYSCALL_EVENT_GET` (37) | `src/kernel/syscall/syscall.h` | ✅ |
| 1.2.8 | `syscall.c` — 实现 `sys_mutex_delete` handler | `src/kernel/syscall/syscall.c` | ✅ |
| 1.2.9 | `syscall.c` — 实现 `sys_mqueue_delete` handler | `src/kernel/syscall/syscall.c` | ✅ |
| 1.2.10 | `syscall.c` — 实现 `sys_event_delete` handler | `src/kernel/syscall/syscall.c` | ✅ |
| 1.2.11 | `syscall.c` — 实现 `sys_event_clear` handler | `src/kernel/syscall/syscall.c` | ✅ |
| 1.2.12 | `syscall.c` — 实现 `sys_event_get` handler | `src/kernel/syscall/syscall.c` | ✅ |
| 1.2.13 | `syscall.c` — 更新 syscall 表 (号 + argc) | `src/kernel/syscall/syscall.c` | ✅ |
| 1.2.14 | `mutex.c` — 实现 `mutex_delete()` 内核函数 | `src/kernel/ipc/mutex.c` | ✅ |
| 1.2.15 | `mqueue.c` — 实现 `mqueue_delete()` 内核函数 | `src/kernel/ipc/mqueue.c` | ✅ |
| 1.2.16 | `event.c` — 实现 `event_delete()` 内核函数 | `src/kernel/ipc/event.c` | ✅ |
| 1.2.17 | `event.c` — 实现 `event_clear()` 内核函数 | `src/kernel/ipc/event.c` | ✅ |
| 1.2.18 | `event.c` — 实现 `event_get()` 内核函数 | `src/kernel/ipc/event.c` | ✅ |
| 1.2.19 | 编译验证 0 warnings | — | ✅ |
| 1.2.20 | 回归测试全部通过 (452 tests) | — | ✅ |

---

## Step 2: Endpoint 实现

### 2.1 核心

| # | 任务 | 文件 | 状态 |
|---|------|------|------|
| 2.1.1 | 创建 `endpoint.h` — 数据结构 + API 声明 | `src/kernel/ipc/endpoint.h` | ✅ |
| 2.1.2 | 创建 `endpoint.c` — `endpoint_create` | `src/kernel/ipc/endpoint.c` | ✅ |
| 2.1.3 | `endpoint.c` — `endpoint_delete` | `src/kernel/ipc/endpoint.c` | ✅ |
| 2.1.4 | `endpoint.c` — `endpoint_send` (含环形缓冲区写入) | `src/kernel/ipc/endpoint.c` | ✅ |
| 2.1.5 | `endpoint.c` — `endpoint_recv` (含服务端阻塞) | `src/kernel/ipc/endpoint.c` | ✅ |
| 2.1.6 | `endpoint.c` — `endpoint_reply` (唤醒客户端) | `src/kernel/ipc/endpoint.c` | ✅ |
| 2.1.7 | `endpoint.c` — 超时处理 (send/recv) | `src/kernel/ipc/endpoint.c` | ✅ |
| 2.1.8 | `endpoint.c` — 多客户端并发排队 | `src/kernel/ipc/endpoint.c` | ✅ |

### 2.2 集成

| # | 任务 | 文件 | 状态 |
|---|------|------|------|
| 2.2.1 | `capability.h` — 添加 `CAP_OBJ_ENDPOINT` | `src/kernel/cap/capability.h` | ✅ |
| 2.2.2 | `syscall.h` — 添加 EP_CREATE/DELETE/SEND/RECV/REPLY (38-42) | `src/kernel/syscall/syscall.h` | ✅ |
| 2.2.3 | `syscall.c` — 实现 5 个 endpoint syscall handler | `src/kernel/syscall/syscall.c` | ✅ |
| 2.2.4 | `syscall.c` — 更新 syscall 表 | `src/kernel/syscall/syscall.c` | ✅ |
| 2.2.5 | `ipc.h` — 添加 `endpoint_init()` 声明 | `src/kernel/ipc/ipc.h` | ✅ |
| 2.2.6 | `kernel.c` — `kern_init` 调用 `endpoint_init()` | `src/kernel/kernel.c` | ✅ |
| 2.2.7 | `Kconfig` — 添加 `CONFIG_IPC_ENDPOINT_MAX` 等配置 | `Kconfig` | ✅ |
| 2.2.8 | `kernel_config.h` — 更新默认值 | `src/kernel/include/kernel_config.h` | ✅ |
| 2.2.9 | Makefile 添加 `endpoint.c` | `Makefile` | ✅ |
| 2.2.10 | 编译验证 0 warnings | — | ✅ |

---

## Step 3: Channel 实现

### 3.1 核心

| # | 任务 | 文件 | 状态 |
|---|------|------|------|
| 3.1.1 | 创建 `channel.h` — 数据结构 + API 声明 | `src/kernel/ipc/channel.h` | ✅ |
| 3.1.2 | 创建 `channel.c` — `channel_create` (含 shm 分配) | `src/kernel/ipc/channel.c` | ✅ |
| 3.1.3 | `channel.c` — `channel_delete` | `src/kernel/ipc/channel.c` | ✅ |
| 3.1.4 | `channel.c` — `channel_connect` (绑定 peer) | `src/kernel/ipc/channel.c` | ✅ |
| 3.1.5 | `channel.c` — `channel_send` (双向消息) | `src/kernel/ipc/channel.c` | ✅ |
| 3.1.6 | `channel.c` — `channel_recv` | `src/kernel/ipc/channel.c` | ✅ |
| 3.1.7 | `channel.c` — `channel_get_shm` | `src/kernel/ipc/channel.c` | ✅ |
| 3.1.8 | `channel.c` — 超时处理 | `src/kernel/ipc/channel.c` | ✅ |

### 3.2 集成

| # | 任务 | 文件 | 状态 |
|---|------|------|------|
| 3.2.1 | `capability.h` — 添加 `CAP_OBJ_CHANNEL` | `src/kernel/cap/capability.h` | ✅ |
| 3.2.2 | `syscall.h` — 添加 CH_CREATE/DELETE/CONNECT/SEND/RECV/GET_SHM (43-48) | `src/kernel/syscall/syscall.h` | ✅ |
| 3.2.3 | `syscall.c` — 实现 6 个 channel syscall handler | `src/kernel/syscall/syscall.c` | ✅ |
| 3.2.4 | `syscall.c` — 更新 syscall 表 | `src/kernel/syscall/syscall.c` | ✅ |
| 3.2.5 | `ipc.h` — 添加 `channel_init()` 声明 | `src/kernel/ipc/ipc.h` | ✅ |
| 3.2.6 | `kernel.c` — `kern_init` 调用 `channel_init()` | `src/kernel/kernel.c` | ✅ |
| 3.2.7 | `Kconfig` — 添加 `CONFIG_IPC_CHANNEL_MAX` 等配置 | `Kconfig` | ✅ |
| 3.2.8 | Makefile 添加 `channel.c` | `Makefile` | ✅ |
| 3.2.9 | 编译验证 0 warnings | — | ✅ |

---

## Step 4: VFS 集成

> 跳过 — 可后续补充

| # | 任务 | 文件 | 状态 |
|---|------|------|------|
| 4.1 | `devfs.c` — 添加 `ep/` 子目录 | `src/kernel/vfs/devfs.c` | ⬜ |
| 4.2 | `devfs.c` — endpoint 创建时自动注册到 `/dev/ep/<name>` | `src/kernel/vfs/devfs.c` | ⬜ |
| 4.3 | `devfs.c` — `open()` 查找 endpoint | `src/kernel/vfs/devfs.c` | ⬜ |
| 4.4 | `devfs.c` — `write()` → `endpoint_send()` | `src/kernel/vfs/devfs.c` | ⬜ |
| 4.5 | `devfs.c` — `read()` → `endpoint_recv()` | `src/kernel/vfs/devfs.c` | ⬜ |
| 4.6 | `devfs.c` — `close()` 无操作 | `src/kernel/vfs/devfs.c` | ⬜ |
| 4.7 | 编译验证 0 warnings | — | ⬜ |

---

## Step 5: 用户态封装 + 测试

### 5.1 用户态封装

| # | 任务 | 文件 | 状态 |
|---|------|------|------|
| 5.1.1 | `user_api.h` — `endpoint_create()` 封装 | `src/kernel/syscall/user_api.h` | ✅ |
| 5.1.2 | `user_api.h` — `endpoint_delete()` 封装 | `src/kernel/syscall/user_api.h` | ✅ |
| 5.1.3 | `user_api.h` — `endpoint_send()` 封装 | `src/kernel/syscall/user_api.h` | ✅ |
| 5.1.4 | `user_api.h` — `endpoint_recv()` 封装 | `src/kernel/syscall/user_api.h` | ✅ |
| 5.1.5 | `user_api.h` — `endpoint_reply()` 封装 | `src/kernel/syscall/user_api.h` | ✅ |
| 5.1.6 | `user_api.h` — `channel_create()` 封装 | `src/kernel/syscall/user_api.h` | ✅ |
| 5.1.7 | `user_api.h` — `channel_delete()` 封装 | `src/kernel/syscall/user_api.h` | ✅ |
| 5.1.8 | `user_api.h` — `channel_connect()` 封装 | `src/kernel/syscall/user_api.h` | ✅ |
| 5.1.9 | `user_api.h` — `channel_send()` 封装 | `src/kernel/syscall/user_api.h` | ✅ |
| 5.1.10 | `user_api.h` — `channel_recv()` 封装 | `src/kernel/syscall/user_api.h` | ✅ |
| 5.1.11 | `user_api.h` — `channel_get_shm()` 封装 | `src/kernel/syscall/user_api.h` | ✅ |
| 5.1.12 | `user_api.h` — `mutex_delete()` 封装 | `src/kernel/syscall/user_api.h` | ✅ |
| 5.1.13 | `user_api.h` — `mqueue_delete()` 封装 | `src/kernel/syscall/user_api.h` | ✅ |
| 5.1.14 | `user_api.h` — `event_delete()` 封装 | `src/kernel/syscall/user_api.h` | ✅ |
| 5.1.15 | `user_api.h` — `event_clear()` 封装 | `src/kernel/syscall/user_api.h` | ✅ |
| 5.1.16 | `user_api.h` — `event_get()` 封装 | `src/kernel/syscall/user_api.h` | ✅ |

### 5.2 测试

| # | 任务 | 文件 | 状态 |
|---|------|------|------|
| 5.2.1 | 创建 `test_ipc_upgrade.c` | `src/tests/test_ipc_upgrade.c` | ✅ |
| 5.2.2 | 测试: wait_queue 公共实现功能无变化 | `src/tests/test_ipc_upgrade.c` | ✅ |
| 5.2.3 | 测试: `mutex_delete` 正常工作 | `src/tests/test_ipc_upgrade.c` | ✅ |
| 5.2.4 | 测试: `mqueue_delete` 正常工作 | `src/tests/test_ipc_upgrade.c` | ✅ |
| 5.2.5 | 测试: `event_delete` 正常工作 | `src/tests/test_ipc_upgrade.c` | ✅ |
| 5.2.6 | 测试: `event_clear` 正常工作 | `src/tests/test_ipc_upgrade.c` | ✅ |
| 5.2.7 | 测试: `event_get` 正常工作 | `src/tests/test_ipc_upgrade.c` | ✅ |
| 5.2.8 | 测试: Endpoint C/S — send → recv → reply → 收到回复 | `src/tests/test_ipc_upgrade.c` | ✅ |
| 5.2.9 | 测试: Endpoint 多客户端 — 3 个客户端并发 send | `src/tests/test_ipc_upgrade.c` | ✅ |
| 5.2.10 | 测试: Endpoint 超时 — send timeout 返回 KERN_ERR_TIMEOUT | `src/tests/test_ipc_upgrade.c` | ✅ |
| 5.2.11 | 测试: Endpoint 超时 — recv timeout 返回 KERN_ERR_TIMEOUT | `src/tests/test_ipc_upgrade.c` | ✅ |
| 5.2.12 | 测试: Endpoint delete wakes waiters | `src/tests/test_ipc_upgrade.c` | ✅ |
| 5.2.13 | 测试: Channel P2P — 双向 send/recv | `src/tests/test_ipc_upgrade.c` | ✅ |
| 5.2.14 | 测试: Channel 共享内存 — get_shm 返回可读写指针 | `src/tests/test_ipc_upgrade.c` | ✅ |
| 5.2.15 | 测试: Channel delete wakes waiters | `src/tests/test_ipc_upgrade.c` | ✅ |
| 5.2.16 | 测试: VFS — open("/dev/ep/svc") → fd | `src/tests/test_ipc_upgrade.c` | ⬜ (Step 4 跳过) |
| 5.2.17 | 测试: VFS — write(fd, msg) → endpoint_send | `src/tests/test_ipc_upgrade.c` | ⬜ (Step 4 跳过) |
| 5.2.18 | 测试: VFS — read(fd, buf) → endpoint_recv | `src/tests/test_ipc_upgrade.c` | ⬜ (Step 4 跳过) |
| 5.2.19 | 测试: capability — 无权限操作返回 KERN_ERR_CAP | `src/tests/test_ipc_upgrade.c` | ⬜ |
| 5.2.20 | Makefile 添加 `test_ipc_upgrade.c` | `Makefile` | ✅ |

---

## Step 6: 构建验证

| # | 任务 | 状态 |
|---|------|------|
| 6.1 | `make BOARD=stm32f767 -j8` → 0 warnings | ✅ |
| 6.2 | 452 现有测试全部通过 (回归) | ✅ |
| 6.3 | 37 新测试全部通过 | ✅ |
| 6.4 | 总测试数 489 ≥ 477 | ✅ |
| 6.5 | `.config` + `stm32f767_defconfig` 更新默认值 | ⬜ |

---

## Bug 修复记录 (调试过程中发现并修复)

| # | Bug | 文件 | 原因 | 修复 |
|---|-----|------|------|------|
| B1 | Test 13 Task Yield hang | `context.S` | TCB struct 偏移量不匹配: state 应为 offset 24 (非 26), attrs 应为 offset 96 (非 80) | 更新汇编指令 `ldrb r1,[r0,#24]` + `ldrb r2,[r0,#96]` |
| B2 | Timer 模块 hang | `scheduler.c` | `sched_wakeup()` 清除 `block_obj` 导致 IPC 无法清理等待队列 → wait_next 成环 | 保留 `block_obj` 由 IPC 原语清理 |
| B3 | Channel P2P 测试失败 | `channel.c` | `channel_connect()` 用调用者作为 peer_a，而非显式指定两个 peer | 改为 `channel_connect(ch_id, peer_a, peer_b)` |
| B4 | Channel delete 测试失败 | 所有 IPC delete | 唤醒任务后未清 `wait_next`/`wait_prev` → memset 后链表成环 | 6 个 IPC delete 函数清除 `wait_next`/`wait_prev` |

---

## 文件变更汇总

| 操作 | 文件数 | 文件列表 |
|------|--------|----------|
| 新增 | 6 | `wait_queue.c`, `wait_queue.h`, `endpoint.c`, `endpoint.h`, `channel.c`, `channel.h` |
| 修改 | 14 | `semaphore.c`, `mutex.c`, `mqueue.c`, `event.c`, `endpoint.c`, `channel.c`, `ipc.h`, `syscall.h`, `syscall.c`, `user_api.h`, `kernel_types.h`, `kernel_config.h`, `capability.h`, `scheduler.c` |
| 修改 (汇编) | 1 | `context.S` (TCB 偏移量修正) |
| 新增测试 | 1 | `test_ipc_upgrade.c` |
| 配置 | 2 | `.config`, `configs/stm32f767_defconfig` (未更新) |
| **合计** | **24** | |

---

## 进度统计

| Step | 总任务数 | 已完成 | 进度 |
|------|----------|--------|------|
| Step 1: wait_queue + syscall | 29 | 29 | 100% |
| Step 2: Endpoint | 18 | 18 | 100% |
| Step 3: Channel | 17 | 17 | 100% |
| Step 4: VFS | 7 | 0 | 0% (跳过) |
| Step 5: 用户态 + 测试 | 36 | 30 | 83% |
| Step 6: 构建验证 | 5 | 4 | 80% |
| Bug 修复 | 4 | 4 | 100% |
| **总计** | **116** | **102** | **88%** |
