# My-RTOS 微内核化修改计划

> 范围说明：当前计划以 STM32F767 为唯一主线目标。现阶段不处理 RP2350，不改默认构建目标，不把多板配置系统作为前置任务。所有改造都以当前 `make` 能直接编译 767 的工作流为基础。

## 当前阶段状态

- P0: completed.
- P1: completed.
- P2: completed and archived in `P2_COMPLETION_REPORT.md`.
- P3: brief execution plan available in `P3_MICROKERNEL_SERVICE_PLAN.md`.

P3 对应本文件中的阶段 2、阶段 3、阶段 5、阶段 7 的前置收敛工作：
先补 usercopy/syscall 边界、fault cleanup、request/reply IPC 和最小服务启动模型，
再推进完整 name server、driver server、FS server 迁移。

## 目标

把当前系统从“带微内核特性的 RTOS”逐步改造成“最小内核 + 用户态服务”的结构。

内核最终只保留：

- 调度与任务上下文切换
- 用户态/内核态隔离
- 地址空间与 MPU 映射
- IPC 与通知
- capability 管理
- 中断绑定与通知
- fault 捕获、任务终止和资源回收

应逐步迁出内核的部分：

- shell
- VFS / ramfs / devfs
- UART / GPIO 等驱动逻辑
- 设备注册框架中的业务分发
- 诊断、统计、文件访问等高层服务

## 非目标

- 不在当前阶段恢复或推进 RP2350。
- 不为了“架构干净”大规模重写已有调度器。
- 不一次性把所有驱动和文件系统迁到用户态。
- 不破坏现有 STM32F767 的直接 `make` 构建体验。

## 阶段 1：建立真正的用户态内存边界

### 背景

当前用户任务虽然能通过 `TASK_ATTR_USER` 切换到非特权模式，但 MPU 映射仍然过宽。`task_create_user()` 里把整个 SRAM 配成用户 RW，这意味着用户任务理论上可以写内核全局变量、TCB、IPC 队列、capability 表和其他任务栈。微内核化必须先解决这个边界。

### 修改内容

1. 重做 `task_create_user()` 的 MPU region 设置。
2. 不再给用户任务映射整个 SRAM。
3. 每个用户任务只允许访问：
   - 用户代码区：RO + executable
   - 用户只读数据区：RO + XN
   - 用户数据区：RW + XN
   - 自己的用户栈：RW + XN，底部 guard
   - 显式授权的共享内存区
4. 内核数据区、TCB、调度队列、capability 表、IPC 内部对象保持 privileged-only。
5. 在 `tcb_t` 中增加用户内存布局描述，避免 MPU region 只靠硬编码。

### 涉及文件

- `src/kernel/task/task.c`
- `src/kernel/task/task.h`
- `src/kernel/mpu/mpu.c`
- `src/kernel/mpu/mpu.h`
- `src/kernel/include/kernel_types.h`
- `link/stm32f767.ld`

### 验收标准

- 用户任务写内核全局变量触发 MemManage fault。
- 用户任务写其他任务栈触发 MemManage fault。
- 用户任务读写自己的栈和数据区正常。
- kernel task 仍按当前方式正常运行。
- 现有 `make` 构建方式不变。

## 阶段 2：增加 usercopy 与 syscall 指针校验

### 背景

当前 syscall handler 直接把用户传入的地址转为内核指针使用。只要用户传入非法地址、内核地址、跨边界地址，就可能导致内核 fault 或越权访问。微内核必须把 syscall 边界作为强校验点。

### 修改内容

1. 新增用户指针校验模块：
   - `user_access_ok(ptr, len, rights)`
   - `copy_from_user(dst, user_src, len)`
   - `copy_to_user(user_dst, src, len)`
   - `strncpy_from_user(dst, user_src, max_len)`
2. syscall handler 不再直接解引用用户指针。
3. 所有 syscall wrapper 按参数类型处理：
   - 标量参数直接传递。
   - buffer 参数先做范围校验，再 copy。
   - string 参数做 bounded copy。
4. VFS syscall、IPC syscall、task create syscall 全部接入 usercopy。
5. 对用户入口函数地址做校验，确保 `entry` 位于用户可执行区域。

### 涉及文件

- `src/kernel/syscall/syscall.c`
- `src/kernel/syscall/syscall.h`
- `src/kernel/syscall/user_api.h`
- `src/kernel/task/task.c`
- `src/kernel/include/kernel_types.h`
- 新增 `src/kernel/usercopy/usercopy.c`
- 新增 `src/kernel/usercopy/usercopy.h`
- `Makefile`

### 验收标准

- 用户传 NULL 指针给 syscall，不会 HardFault。
- 用户传内核地址给 syscall，返回权限或参数错误。
- 用户传跨 region buffer，返回错误。
- `open/read/write/ioctl/ipc_send/ipc_recv` 不直接解引用用户指针。
- 恶意用户任务不能通过 syscall 读取或写入内核内存。

## 阶段 3：收紧 fault 处理和任务终止路径

### 背景

用户任务 fault 后系统应该终止该任务并回收资源，而不是污染调度器、IPC 队列或 capability 表。当前已有 fault handler 和 task terminate 雏形，但资源清理还不完整。

### 修改内容

1. 增加统一任务退出清理入口：
   - `task_cleanup_resources(tcb)`
2. fault、task_exit、task_delete 都走同一套清理逻辑。
3. 清理内容包括：
   - capability 回收
   - IPC wait queue 清理
   - endpoint reply 等待状态清理
   - channel send/recv 等待状态清理
   - timer 归属清理
   - fd/session 清理
4. 用户 fault 后只杀当前用户任务。
5. 内核 fault 仍 panic，保留 crash dump。

### 涉及文件

- `src/kernel/fault/fault.c`
- `src/kernel/task/task.c`
- `src/kernel/task/task.h`
- `src/kernel/ipc/*`
- `src/kernel/cap/*`
- `src/kernel/timer/timer.c`

### 验收标准

- 用户任务 fault 后其他任务继续运行。
- fault 中任务如果阻塞在 IPC，不留下 wait queue 悬挂节点。
- fault 中任务持有的 capability 全部失效。
- joiner 能收到合理结果。

## 阶段 4：把 capability 改成 per-task CSpace

### 背景

当前 capability 是全局 token 表，能做基础权限检查，但还不够微内核。需要变成每任务独立 capability space，避免 token 猜测、旧 cap 复用、跨任务误用。

### 修改内容

1. 每个任务增加 CSpace：
   - 固定大小 capability slot 表
   - 每个 slot 带 generation
2. capability id 改为：
   - slot index
   - generation
3. capability entry 包含：
   - object id / object pointer
   - object type
   - rights
   - owner task
   - generation
   - parent
   - child/refcount 信息
4. capability API 改造：
   - `cap_alloc(task, object, type, rights)`
   - `cap_lookup(task, cap, type, rights)`
   - `cap_derive(task, cap, subset_rights)`
   - `cap_transfer(src, dst, cap)`
   - `cap_revoke(task, cap)`
5. syscall 层只允许解析当前任务 CSpace 中的 cap。

### 涉及文件

- `src/kernel/cap/capability.c`
- `src/kernel/cap/capability.h`
- `src/kernel/include/kernel_types.h`
- `src/kernel/syscall/syscall.c`
- `src/kernel/task/task.c`

### 验收标准

- A 任务不能使用 B 任务的 cap。
- cap revoke 后派生 cap 全部失效。
- cap slot 复用后旧 cap 不会指向新对象。
- privileged kernel path 可以显式绕过用户 CSpace，但普通用户 syscall 不能绕过。

## 阶段 5：重做 endpoint/reply IPC 语义

### 背景

当前 endpoint 已有多 client 到 server 的雏形，但 `current_sender` 只适合非常简单的同步场景。真正的微内核 IPC 需要 request/reply 绑定，避免多个请求、超时、取消、server 并发处理时串线。

### 修改内容

1. endpoint pending request 改成 request slot：
   - sender task
   - message buffer
   - reply capability
   - badge / client identity
   - transferred capabilities
2. `endpoint_recv()` 返回 request 信息和 reply cap。
3. `endpoint_reply()` 使用 reply cap，而不是 endpoint 全局 `current_sender`。
4. 增加统一 IPC 调用：
   - `ipc_call(endpoint_cap, tx, rx, timeout)`
   - `ipc_send(endpoint_cap, tx, timeout)`
   - `ipc_recv(endpoint_cap, rx, timeout)`
   - `ipc_reply(reply_cap, rx)`
5. 支持 capability transfer：
   - IPC message header 带 cap slots。
   - kernel 负责 copy/move/derive cap。
6. 完整处理 timeout/delete/task exit 清理。

### 涉及文件

- `src/kernel/ipc/endpoint.c`
- `src/kernel/ipc/endpoint.h`
- `src/kernel/syscall/syscall.c`
- `src/kernel/syscall/user_api.h`
- `src/kernel/cap/capability.c`

### 验收标准

- 多 client 同时 call 同一个 endpoint，不会 reply 串线。
- server 可以按 request 独立 reply。
- client timeout 后 reply cap 失效。
- endpoint delete 后所有等待者返回 `KERN_ERR_NOEXIST`。
- IPC 能携带 capability。

## 阶段 6：修正 channel 等待队列模型

### 背景

当前 channel 在接收端读完数据后，通过遍历所有任务寻找阻塞发送者。这说明等待关系没有被 channel 对象完整管理。

### 修改内容

1. channel 内部增加四类等待队列：
   - `a_send_waiters`
   - `b_send_waiters`
   - `a_recv_waiters`
   - `b_recv_waiters`
2. 发送阻塞时挂入对应 send queue。
3. 接收完成后只唤醒对应 send queue。
4. 删除 channel 时清理所有 wait queue。
5. channel peer 校验：
   - 非 peer 不能 send/recv。
   - 未 connect 前不能 send/recv。
6. 共享内存不再直接裸返回给用户，必须通过 mapping/capability 控制。

### 涉及文件

- `src/kernel/ipc/channel.c`
- `src/kernel/ipc/channel.h`
- `src/kernel/ipc/wait_queue.c`
- `src/kernel/syscall/syscall.c`

### 验收标准

- channel 不再扫描所有 task。
- 非 peer 访问 channel 返回权限错误。
- channel delete 后所有等待者都被正确唤醒。
- shared memory 只能被授权任务访问。

## 阶段 7：引入 root/init task 和 name server

### 背景

现在系统启动后直接创建内核应用和 shell。微内核应该由一个 root/init 用户态任务持有初始资源 capability，再由它启动用户态服务。

### 修改内容

1. 内核启动后只创建：
   - idle task
   - root/init task
2. root/init task 持有初始 capability：
   - 创建任务
   - 创建 endpoint/channel
   - IRQ bind
   - 内存分配
3. 新增 name server：
   - 服务名到 endpoint capability 的映射
   - 支持 register/lookup/unregister
4. shell、fs、driver 都通过 name server 发现服务。

### 涉及文件

- `src/app/main.c`
- `src/kernel/kernel.c`
- `src/kernel/system_init.c`
- 新增 `src/user/init/init.c`
- 新增 `src/user/nameserver/nameserver.c`

### 验收标准

- 内核不直接启动 shell 业务逻辑。
- root/init 能启动 name server。
- client 能通过 name server 获取服务 endpoint cap。
- name server fault 不导致 kernel panic。

## 阶段 8：把 UART/GPIO 驱动迁到用户态服务

### 背景

当前 UART/GPIO 和 device/devfs 注册仍在内核态。微内核中驱动应该作为用户态 server，内核只提供受限的 IRQ 和 MMIO 授权机制。

### 修改内容

1. 增加 IRQ notification 机制：
   - `irq_bind(irq, endpoint/notification cap)`
   - IRQ 到来后 kernel mask IRQ 并通知 driver server
   - driver server 处理完成后 ack/unmask
2. 增加受控 MMIO 映射：
   - root/init 给 driver server 分配 MMIO capability
   - MPU 只映射对应外设寄存器窗口
3. 新增 UART server：
   - 管理 RX/TX ring buffer
   - 对外暴露 read/write/ioctl IPC
4. 新增 GPIO server：
   - 管理 pin capability
   - 对外暴露 read/write/toggle IPC
5. shell 不直接调用底层 UART 驱动，通过 UART server 交互。

### 涉及文件

- `src/kernel/irq/irq.c`
- `src/kernel/irq/irq.h`
- `src/kernel/mpu/mpu.c`
- `src/board/stm32f767/board_drivers.c`
- `src/drivers/*`
- 新增 `src/user/drivers/uart_server.c`
- 新增 `src/user/drivers/gpio_server.c`

### 验收标准

- UART/GPIO 业务逻辑不再运行在 kernel task 中。
- UART server fault 后 kernel 不 panic。
- IRQ 能通知用户态 driver server。
- driver server 可以 ack/unmask IRQ。

## 阶段 9：把 VFS/devfs/ramfs 迁到用户态服务

### 背景

VFS、inode、ramfs、devfs 现在都在内核中。严格微内核中，文件系统应是用户态服务。

### 修改内容

1. 新增 FS server：
   - 管理 fd/session
   - 提供 open/read/write/close/ioctl/lseek 协议
2. ramfs 迁入 FS server。
3. devfs 迁入 FS server 或独立 dev manager。
4. syscall 层不再直接调用 `vfs_*` 内核函数。
5. user API 中的 open/read/write 变成 IPC client wrapper。

### 涉及文件

- `src/kernel/vfs/*`
- `src/kernel/syscall/syscall.c`
- `src/kernel/syscall/user_api.h`
- 新增 `src/user/fs/fs_server.c`
- 新增 `src/user/fs/ramfs_server.c`
- 新增 `src/user/fs/devfs_server.c`

### 验收标准

- FS server fault 后 kernel 不 panic。
- shell `ls/cat/echo` 通过 FS server 工作。
- 文件 descriptor 权限由 capability/session 控制。
- 内核不再持有 inode 业务逻辑。

## 阶段 10：测试与回归

### 新增测试方向

1. MPU 隔离测试：
   - 用户写内核内存失败。
   - 用户写其他任务栈失败。
   - 用户访问授权共享区成功。
2. syscall 指针测试：
   - NULL 指针。
   - 内核地址。
   - 跨 region buffer。
   - 超长字符串。
3. capability 测试：
   - 跨任务 cap 访问失败。
   - derive 不能扩大权限。
   - revoke 派生树。
   - generation 防旧 cap 复用。
4. IPC 测试：
   - 多 client call/reply。
   - timeout 清理。
   - endpoint delete 清理。
   - cap transfer。
5. fault 测试：
   - 用户 fault 后系统继续运行。
   - fault 中任务持有资源被回收。
6. driver server 测试：
   - IRQ notify。
   - driver fault/restart。
7. FS server 测试：
   - open/read/write 权限。
   - server crash 后 client 收到错误。

### 涉及文件

- `src/tests/test_mpu.c`
- `src/tests/test_syscall.c`
- `src/tests/test_capability.c`
- `src/tests/test_ipc_upgrade.c`
- `src/tests/test_fault.c`
- 新增 `src/tests/test_usercopy.c`
- 新增 `src/tests/test_service_model.c`

## 推荐执行顺序

1. 阶段 1：MPU 用户态内存边界。
2. 阶段 2：usercopy 和 syscall 指针校验。
3. 阶段 3：fault/exit 资源清理。
4. 阶段 4：per-task CSpace capability。
5. 阶段 5：endpoint/reply IPC。
6. 阶段 6：channel 等待队列修正。
7. 阶段 7：root/init task 和 name server。
8. 阶段 8：UART/GPIO 用户态 driver server。
9. 阶段 9：VFS/devfs/ramfs 用户态 FS server。
10. 阶段 10：补齐测试矩阵。

前三个阶段是微内核化的地基。只有 MPU 隔离、usercopy、安全清理完成后，后续把驱动、文件系统、shell 移到用户态才有实际安全意义。
