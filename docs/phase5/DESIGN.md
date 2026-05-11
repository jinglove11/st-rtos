# Phase 5: IPC 升级 — 同步消息传递

> 版本: 1.0 | 日期: 2026-05-08
> 前置: Phase 1-4 已完成 (452 tests, 0 failures)
> 目标: 实现 Endpoint (C/S) + Channel (P2P) + wait_queue 提取 + 缺失 syscall 补全

---

## 一、目标

1. **提取公共 wait_queue** — 消除 4 个 IPC 文件中的重复代码
2. **补全缺失 syscall** — mqueue_delete, event_delete, event_clear, event_get, mutex_delete
3. **实现 Endpoint** — 客户端-服务器 (C/S) 消息传递模型
4. **实现 Channel** — 点对点 (P2P) 双向通信，带共享内存
5. **VFS 集成** — endpoint 可通过 `open("/dev/ep/<name>")` 访问
6. **syscall 封装** — 用户态 API 通过 SVC 调用
7. **测试覆盖** — 全部新功能测试 + 回归测试

---

## 二、现有 IPC 架构分析

### 2.1 已有原语

| 原语 | 文件 | syscall | 状态 |
|------|------|---------|------|
| Semaphore | `semaphore.c/h` | 9-12 | 完整 |
| Mutex | `mutex.c/h` | 13-15 | 缺 delete syscall |
| Message Queue | `mqueue.c/h` | 16-18 | 缺 delete syscall |
| Event Flag | `event.c/h` | 19-21 | 缺 delete/clear/get syscall |

### 2.2 已知问题

| 问题 | 位置 | 说明 |
|------|------|------|
| wait_queue 重复代码 | 4 个 `.c` 文件 | `wait_queue_init/add/remove/get_highest` 各自实现 |
| 缺失 syscall | `syscall.h` | mqueue_delete, event_delete, event_clear, event_get, mutex_delete |
| event_wait 限制 | `syscall.c` | 硬编码 opt=0 (AND), received=NULL |
| 无 endpoint/channel | — | 需要全新实现 |

### 2.3 wait_queue 重复代码分析

以下 4 个函数在 `semaphore.c`, `mutex.c`, `mqueue.c`, `event.c` 中完全重复：

```c
static void wait_queue_init(wait_queue_t *wq);
static void wait_queue_add(wait_queue_t *wq, tcb_t *tcb);
static void wait_queue_remove(wait_queue_t *wq, tcb_t *tcb);
static tcb_t *wait_queue_get_highest(wait_queue_t *wq);
```

**方案**: 提取到 `src/kernel/ipc/wait_queue.c/h`，所有 IPC 原语共享。

---

## 三、设计

### 3.1 wait_queue 提取

**新文件**: `src/kernel/ipc/wait_queue.c` + `src/kernel/ipc/wait_queue.h`

```c
// wait_queue.h
#ifndef WAIT_QUEUE_H
#define WAIT_QUEUE_H

#include "kernel_types.h"

void     wait_queue_init(wait_queue_t *wq);
void     wait_queue_add(wait_queue_t *wq, tcb_t *tcb);
void     wait_queue_remove(wait_queue_t *wq, tcb_t *tcb);
tcb_t   *wait_queue_get_highest(wait_queue_t *wq);
uint16_t wait_queue_count(const wait_queue_t *wq);

#endif
```

**修改**: 4 个 IPC 文件删除各自的 `static` 实现，改为 `#include "wait_queue.h"`。

### 3.2 补全缺失 syscall

| syscall | 号 | 参数 | 说明 |
|---------|---|------|------|
| SYSCALL_MUTEX_DELETE | 22 | mutex_id | 删除互斥锁 |
| SYSCALL_MQUEUE_DELETE | 34 | queue_id | 删除消息队列 |
| SYSCALL_EVENT_DELETE | 35 | event_id | 删除事件组 |
| SYSCALL_EVENT_CLEAR | 36 | event_id, flags | 清除事件标志 |
| SYSCALL_EVENT_GET | 37 | event_id, &flags | 读取当前标志 |

**注意**: 22 原为 TIMER_CREATE，需要重新编号。查看 syscall.h 实际使用情况后调整。

### 3.3 Endpoint 设计

Endpoint 是**多对一**的 C/S 通信模型：
- 服务端创建 endpoint，调用 `endpoint_recv()` 阻塞等待
- 客户端通过 `endpoint_send()` 发送请求，阻塞等待回复
- 服务端处理后调用 `endpoint_reply()` 回复，唤醒客户端

```
客户端                    服务端
   │                         │
   │  ep = endpoint_create("svc", 64, 4)
   │                         │
   │  endpoint_send(ep, req) │
   ├────────────────────────▶│ endpoint_recv(ep, &req)
   │  (阻塞等待回复)          │
   │                         │ 处理请求...
   │                         │ endpoint_reply(ep, resp)
   │◀────────────────────────┤
   │  (收到回复, 返回)        │
```

#### 数据结构

```c
#define ENDPOINT_MAX        4
#define ENDPOINT_NAME_LEN   16

typedef struct {
    char        name[ENDPOINT_NAME_LEN];
    uint16_t    msg_size;           // 消息大小 (字节)
    uint16_t    max_pending;        // 最大挂起请求数
    uint16_t    pending_count;      // 当前挂起请求数

    /* 接收队列 — 客户端发来的请求等待服务端接收 */
    wait_queue_t recv_waiters;      // 等待发送的客户端

    /* 回复队列 — 服务端回复后唤醒等待的客户端 */
    wait_queue_t reply_waiters;     // 等待回复的客户端

    /* 消息缓冲区 (环形队列) */
    void       *msg_buffer;         // 指向静态缓冲区
    uint16_t    head;
    uint16_t    tail;
    uint16_t    count;

    uint8_t     in_use;
} endpoint_t;
```

#### API

```c
// 内核 API
ep_id_t  endpoint_create(const char *name, uint16_t msg_size, uint16_t max_pending);
kern_err_t endpoint_delete(ep_id_t ep_id);
kern_err_t endpoint_send(ep_id_t ep_id, const void *msg, uint32_t timeout);
kern_err_t endpoint_recv(ep_id_t ep_id, void *msg, uint32_t timeout);
kern_err_t endpoint_reply(ep_id_t ep_id, const void *msg);

// syscall (用户态)
SYSCALL_EP_CREATE    38  (name_ptr, msg_size, max_pending)
SYSCALL_EP_DELETE    39  (ep_id)
SYSCALL_EP_SEND      40  (ep_id, msg_ptr, timeout)
SYSCALL_EP_RECV      41  (ep_id, msg_ptr, timeout)
SYSCALL_EP_REPLY     42  (ep_id, msg_ptr)
```

#### 发送流程

```
endpoint_send(ep, msg, timeout):
  1. 进入临界区
  2. 如果 pending_count >= max_pending:
     - 挂入 recv_waiters 队列
     - 阻塞 (timeout)
     - 被唤醒后继续
  3. 拷贝 msg 到环形缓冲区
  4. pending_count++
  5. 如果有服务端在 reply_waiters 等待:
     - 唤醒服务端
  6. 挂入 reply_waiters 队列 (等待回复)
  7. 阻塞 (timeout)
  8. 被唤醒后，从 reply 缓冲区读取回复
  9. 返回 KERN_OK
```

#### 接收流程

```
endpoint_recv(ep, msg, timeout):
  1. 进入临界区
  2. 如果 pending_count == 0:
     - 挂入 recv_waiters 队列 (表示服务端就绪)
     - 阻塞 (timeout)
     - 被唤醒后继续
  3. 从环形缓冲区取出消息
  4. pending_count--
  5. 如果有客户端在 recv_waiters 等待 (等待发送):
     - 唤醒一个客户端 (让它发送)
  6. 保存客户端 TCB 引用 (用于 reply)
  7. 返回 KERN_OK，msg 填充请求内容
```

#### 回复流程

```
endpoint_reply(ep, msg):
  1. 进入临界区
  2. 拷贝 msg 到客户端的回复缓冲区
  3. 从 reply_waiters 唤醒对应客户端
  4. 返回 KERN_OK
```

### 3.4 Channel 设计

Channel 是**一对一**的双向通信，带共享内存区域：

```c
#define CHANNEL_MAX     4

typedef struct {
    task_id_t   peer_a;             // 端点 A
    task_id_t   peer_b;             // 端点 B
    void       *shm;                // 共享内存地址
    uint32_t    shm_size;           // 共享内存大小

    /* 双向消息队列 */
    wait_queue_t a_waiters;         // A 等待 B 发送
    wait_queue_t b_waiters;         // B 等待 A 发送

    /* 消息缓冲区 (各方向独立) */
    void       *a_to_b_buf;         // A→B 消息
    void       *b_to_a_buf;         // B→A 消息
    uint16_t    msg_size;

    uint8_t     in_use;
} channel_t;
```

#### API

```c
// 内核 API
ch_id_t  channel_create(uint16_t msg_size, uint32_t shm_size);
kern_err_t channel_delete(ch_id_t ch_id);
kern_err_t channel_connect(ch_id_t ch_id, task_id_t peer);
kern_err_t channel_send(ch_id_t ch_id, const void *msg, uint32_t timeout);
kern_err_t channel_recv(ch_id_t ch_id, void *msg, uint32_t timeout);
void      *channel_get_shm(ch_id_t ch_id);

// syscall
SYSCALL_CH_CREATE    43  (msg_size, shm_size)
SYSCALL_CH_DELETE    44  (ch_id)
SYSCALL_CH_CONNECT   45  (ch_id, peer_task_id)
SYSCALL_CH_SEND      46  (ch_id, msg_ptr, timeout)
SYSCALL_CH_RECV      47  (ch_id, msg_ptr, timeout)
SYSCALL_CH_GET_SHM   48  (ch_id)
```

### 3.5 VFS 集成

Endpoint 可挂载到 VFS，用户通过 `open/read/write` 访问：

```
/dev/ep/svc0    → endpoint "svc0"
/dev/ep/svc1    → endpoint "svc1"
```

实现方式：
- `devfs` 目录下新增 `ep/` 子目录
- endpoint 创建时自动注册到 `/dev/ep/<name>`
- `open()` → 查找 endpoint
- `write(fd, msg, n)` → `endpoint_send(ep, msg)`
- `read(fd, buf, n)` → `endpoint_recv(ep, buf)`
- `close(fd)` → 无操作

### 3.6 Kconfig 配置

```
# IPC 升级配置
CONFIG_IPC_ENDPOINT_MAX=4
CONFIG_IPC_CHANNEL_MAX=4
CONFIG_IPC_EP_MSG_SIZE=64
CONFIG_IPC_EP_MAX_PENDING=4
CONFIG_IPC_CH_MSG_SIZE=64
CONFIG_IPC_CH_SHM_SIZE=256
```

---

## 四、文件变更清单

| # | 操作 | 文件 | 说明 |
|---|------|------|------|
| 1 | 新增 | `src/kernel/ipc/wait_queue.c` | 公共 wait_queue 实现 |
| 2 | 新增 | `src/kernel/ipc/wait_queue.h` | wait_queue API 声明 |
| 3 | 修改 | `src/kernel/ipc/semaphore.c` | 删除重复代码，include wait_queue.h |
| 4 | 修改 | `src/kernel/ipc/mutex.c` | 同上 + mutex_delete 实现 |
| 5 | 修改 | `src/kernel/ipc/mqueue.c` | 同上 + mqueue_delete 实现 |
| 6 | 修改 | `src/kernel/ipc/event.c` | 同上 + event_delete/clear/get 实现 |
| 7 | 修改 | `src/kernel/ipc/ipc.h` | 添加 endpoint_init/channel_init |
| 8 | 新增 | `src/kernel/ipc/endpoint.c` | Endpoint 实现 |
| 9 | 新增 | `src/kernel/ipc/endpoint.h` | Endpoint API |
| 10 | 新增 | `src/kernel/ipc/channel.c` | Channel 实现 |
| 11 | 新增 | `src/kernel/ipc/channel.h` | Channel API |
| 12 | 修改 | `src/kernel/syscall/syscall.h` | 新增 syscall 号 |
| 13 | 修改 | `src/kernel/syscall/syscall.c` | 新增 handler + 表条目 |
| 14 | 修改 | `src/kernel/syscall/user_api.h` | 用户态封装 |
| 15 | 修改 | `src/kernel/include/kernel_types.h` | 新增 BLOCK_REASON_EP/CH, cap 类型 |
| 16 | 修改 | `src/kernel/include/kernel_config.h` | 由 Kconfig 自动生成 |
| 17 | 修改 | `src/kernel/cap/capability.h` | CAP_OBJ_ENDPOINT, CAP_OBJ_CHANNEL |
| 18 | 修改 | `src/kernel/vfs/devfs.c` | ep/ 子目录 + endpoint VFS 注册 |
| 19 | 修改 | `src/kernel/kernel.c` | kern_init 添加 endpoint_init/channel_init |
| 20 | 修改 | `Makefile` | 新增源文件 |
| 21 | 修改 | `Kconfig` | 新增 IPC 升级配置项 |
| 22 | 新增 | `src/tests/test_ipc_upgrade.c` | Endpoint + Channel + syscall 补全测试 |
| 23 | 修改 | `.config` + `configs/stm32f767_defconfig` | 新配置默认值 |

---

## 五、实施步骤

### Step 1: wait_queue 提取 + syscall 补全
- 提取 wait_queue 到独立文件
- 4 个 IPC 文件改用公共 wait_queue
- 补全 mutex_delete/mqueue_delete/event_delete/clear/get syscall
- 构建验证 0 warnings
- 回归测试全部通过

### Step 2: Endpoint 实现
- endpoint.c/h — 核心逻辑
- syscall 注册
- capability 集成 (CAP_OBJ_ENDPOINT)
- Kconfig 配置
- 内核初始化
- Makefile 更新

### Step 3: Channel 实现
- channel.c/h — 核心逻辑
- syscall 注册
- capability 集成 (CAP_OBJ_CHANNEL)
- 共享内存 MPU region 支持

### Step 4: VFS 集成
- devfs ep/ 子目录
- endpoint 自动注册到 VFS
- open/read/write 转发

### Step 5: 用户态封装 + 测试
- user_api.h 封装
- test_ipc_upgrade.c 测试文件
- 全量回归测试

### Step 6: 构建验证
- `make -j8` → 0 warnings
- 全部测试通过 (目标: 480+ tests)

---

## 六、验证标准

| 验证项 | 标准 |
|--------|------|
| 编译 | `make BOARD=stm32f767 -j8` → 0 warnings |
| 回归 | 452 现有测试全部通过 |
| wait_queue | 4 个 IPC 原语使用公共 wait_queue，功能无变化 |
| syscall 补全 | mqueue_delete, event_delete, event_clear, event_get 正常工作 |
| Endpoint C/S | 客户端 send → 服务端 recv → 服务端 reply → 客户端收到回复 |
| Endpoint 多客户端 | 3 个客户端并发 send，服务端逐个处理 |
| Endpoint 超时 | send/recv timeout 正确返回 KERN_ERR_TIMEOUT |
| Channel P2P | 双向 send/recv 正确 |
| Channel 共享内存 | 通过 channel_get_shm 获取的指针可读写 |
| VFS 集成 | open("/dev/ep/svc") → fd, write(fd, msg) → endpoint_send |
| 新增测试 | 25+ 新测试全部通过 |
