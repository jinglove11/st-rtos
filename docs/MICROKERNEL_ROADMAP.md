# my-rtos 微内核演进路线图

> 版本: 1.0 | 日期: 2026-04-30 | 目标硬件: STM32F767ZI (Cortex-M7)

---

## 一、当前状态评估

### 1.1 架构定位

```
当前 my-rtos = 单体 RTOS 内核

┌──────────────────────────────────────────┐
│  所有任务 (特权模式, MSP/PSP 未隔离)       │
│  ┌─────┐ ┌─────┐ ┌─────┐ ┌───────────┐  │
│  │task0│ │task1│ │task2│ │kernel      │  │
│  │     │ │     │ │     │ │scheduler   │  │
│  │     │ │     │ │     │ │ipc/timer/  │  │
│  │     │ │     │ │     │ │irq/mem...  │  │
│  └─────┘ └─────┘ └─────┘ └───────────┘  │
│         共享地址空间, 函数直接调用           │
└──────────────────────────────────────────┘
```

### 1.2 已有基础 (可直接利用)

| 组件 | 状态 | 微内核价值 |
|------|------|-----------|
| **PSP/MSP 分离** | `context.S` 已实现 | SVC 入口已切换到 PSP，扩展即可 |
| **SVC Handler** | 仅用于首次启动 | 可扩展为通用 syscall 入口 |
| **`kern_syscall_handler`** | 空桩 | 直接替换为 syscall 分发表 |
| **`capability_t`** | 类型已定义 | 能力系统数据结构已就位 |
| **LDREX/STREX** | `spinlock.h` 完整实现 | 多核就绪 |
| **RAM 向量表** | 已实现 + VTOR 重映射 | 每个任务可配独立向量 |
| **Cortex-M7 MPU** | 硬件存在，未编程 | 8 个 region，256B 子区域 |
| **I/D-Cache** | `hal.c` 已支持 | 地址空间切换时需维护 |
| **静态内存池** | `mempool.c` 已实现 | 内核对象分配基础 |
| **测试框架** | 121 项全部通过 | 回归保护 |

### 1.3 目标架构

```
完整微内核 my-rtos v2.0

┌─────────────────────────────────────────────────────┐
│                    用户空间                          │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐          │
│  │ 用户任务A │  │ 用户任务B │  │ 驱动服务  │          │
│  │ (非特权)  │  │ (非特权)  │  │ (非特权)  │          │
│  │ PSP+MPU  │  │ PSP+MPU  │  │ PSP+MPU  │          │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘          │
│       │              │              │                │
│       └──────────────┼──────────────┘                │
│                      │ SVC (系统调用)                 │
│              ┌───────┴───────┐                       │
│              │   微内核核心    │                       │
│              │ ┌───────────┐ │                       │
│              │ │ syscall   │ │  特权模式 (MSP)        │
│              │ │ scheduler │ │                       │
│              │ │ ipc       │ │                       │
│              │ │ cap       │ │                       │
│              │ │ mem       │ │                       │
│              │ │ mpu mgr   │ │                       │
│              │ └───────────┘ │                       │
│              └───────────────┘                       │
└─────────────────────────────────────────────────────┘
```

---

## 二、分阶段实施路线

```
Phase 1: 安全基础 (MPU + Syscall) ──────── 约 4-6 周
Phase 2: 能力系统 ──────────────────────── 约 2-3 周
Phase 3: VFS 虚拟文件系统 ──────────────── 约 3-4 周  ← 驱动框架的基石
Phase 4: 设备驱动框架 ──────────────────── 约 2-3 周  ← 依赖 Phase 3
Phase 5: IPC 升级 ──────────────────────── 约 3-4 周
Phase 6: 异常容错 ──────────────────────── 约 2-3 周
Phase 7: 诊断生态 ──────────────────────── 约 2-3 周
```

### 依赖关系图

```
Phase 1 (MPU+Syscall)
   │
   ├── Phase 2 (Capability)
   │      │
   │      ├── Phase 3 (VFS) ──→ Phase 4 (Driver Framework)
   │      │
   │      ├── Phase 5 (IPC Upgrade)
   │      │
   │      └── Phase 6 (Fault Tolerance)
   │             │
   │             └── Phase 7 (Diagnostic Ecosystem)
   │
   └── (所有上层都依赖 syscall)
```

---

## 三、Phase 1: 安全基础 — MPU 内存保护 + 系统调用

> **这是成为微内核的必经之路。没有 MPU + syscall，就没有微内核。**

### 3.1 目标

- 内核运行在特权模式 (MSP)
- 用户任务运行在非特权模式 (PSP)
- 每个用户任务有独立的 MPU 区域
- 用户任务通过 SVC 系统调用访问内核服务
- 用户任务无法直接访问内核内存、外设或其他任务的内存

### 3.2 系统调用接口设计

```
SVC 指令编码:
  svc #N  →  N 编码在指令的 [7:0] 位

当前 SVC #0 用于首次任务启动。
扩展方案:
  SVC #0 → 保留 (首次启动)
  SVC #1 → 通用系统调用 (参数在 R0=syscall_num, R1-R3=args)

内核提供 syscall 表:
┌──────┬──────────────────────┬─────────────────────┐
│ Num  │ 服务                  │ 替换的直接调用        │
├──────┼──────────────────────┼─────────────────────┤
│  0   │ task_yield           │ sched_yield()        │
│  1   │ task_delay           │ task_delay()         │
│  2   │ task_exit            │ task_exit()          │
│  3   │ task_create          │ task_create()        │
│  4   │ task_start           │ task_start()         │
│  5   │ task_suspend         │ task_suspend()       │
│  6   │ task_resume          │ task_resume()        │
│  7   │ task_delete          │ task_delete()        │
│  8   │ task_self            │ task_self()          │
│  9   │ sem_create           │ sem_create()         │
│ 10   │ sem_wait             │ sem_wait()           │
│ 11   │ sem_post             │ sem_post()           │
│ 12   │ sem_delete           │ sem_delete()         │
│ 13   │ mutex_create         │ mutex_create()       │
│ 14   │ mutex_lock           │ mutex_lock()         │
│ 15   │ mutex_unlock         │ mutex_unlock()       │
│ 16   │ mqueue_create        │ mqueue_create()      │
│ 17   │ mqueue_send          │ mqueue_send()        │
│ 18   │ mqueue_recv          │ mqueue_recv()        │
│ 19   │ event_create/wait/set│ event_*()            │
│ 20   │ timer_create/start   │ timer_*()            │
│ 21   │ irq_register         │ irq_register()       │
│ 22   │ bh_create/schedule   │ bh_*()               │
│ 23   │ mem_alloc/free       │ mem_*()              │
└──────┴──────────────────────┴─────────────────────┘
```

关键设计决策：
- 内核对象引用使用**能力令牌**而非直接指针 (Phase 2 实现)
- 第一阶段可用对象 ID + 所有权检查作为过渡
- syscall 返回 `kern_err_t` 在 R0

### 3.3 MPU 配置策略

Cortex-M7 MPU 特性：
- 8 个 region (0-7)
- 256 字节子区域粒度
- 支持 XN (Execute Never) 位
- 背景区域 (特权模式自动访问全部，非特权受限)

```
Region 分配 (每任务):

Region 0: 内核代码+数据    → 特权访问, 用户不可见 (背景区域)
Region 1: 任务代码 (.text)  → 用户 RO+X
Region 2: 任务数据 (.data)  → 用户 RW
Region 3: 任务栈 (PSP)     → 用户 RW, 底部 32B 无访问 (栈溢出守卫)
Region 4: 外设 (可选)       → 按任务授权
Region 5: 共享内存 (可选)   → 跨任务通信
Region 6: 预留
Region 7: 预留
```

MPU 子区域守卫 (栈溢出硬件保护)：
```
任务栈 = 1024B, 子区域 = 128B (8 个子区域)

高地址 ┌────────────┐ Region start
       │  子区域 7   │ RW
       │  子区域 6   │ RW
       │  子区域 5   │ RW
       │  子区域 4   │ RW
       │  子区域 3   │ RW
       │  子区域 2   │ RW
       │  子区域 1   │ RW
低地址 │  子区域 0   │ NO ACCESS ── 栈溢出立即 MemManage Fault
       └────────────┘
```

### 3.4 上下文切换增强

当前 `context.S` 的 PendSV 需要增加：

```
PendSV 进入 →
  1. 保存 R4-R11 到当前任务栈 (已有)
  2. 保存当前任务 TCB->sp (已有)

  kern_pendsv_handler() →
    3. 处理当前任务状态 (已有)
    4. 选择下一个任务 (已有)

PendSV 返回前 (新增) →
  5. 配置 MPU region 为下一个任务的内存布局
  6. 从下一个任务 TCB 恢复 sp
  7. 恢复 R4-R11 (已有)
  8. 异常返回 (已有)
```

TCB 新增字段：
```c
typedef struct tcb {
    // ... 现有字段 ...
    uint32_t    attrs;              // 任务属性 (特权/非特权, 能力级别)
    uint32_t    mpu_regions[8][2];  // MPU region [RBAR, RASR] * 8
    cap_id_t    cap_set[8];         // 持有的能力集 (最大值)
    void       *kernel_stack;       // 内核栈指针 (用于 syscall 切换)
} tcb_t;
```

### 3.5 SVC Handler 重构

当前 SVC Handler 只处理 #0 (首次启动)。重构为通用 syscall 入口：

```asm
SVC_Handler:
    ; 1. 检查当前是否在用户模式 (非特权)
    ;    如果来自内核 (特权模式), 直接处理
    mrs     r2, ipsr
    ; 异常号 = IPSR & 0x1FF, SVC = 11

    ; 2. 从栈中提取 SVC 指令编码
    ;    异常栈帧中的 PC 指向 svc 指令后一条,
    ;    PC - 2 处是 svc 指令, 低 8 位是 SVC 号
    ldr     r3, [sp, #24]       ; R3 = 异常栈帧中的 PC
    ldrb    r3, [r3, #-2]       ; R3 = SVC 号的低 8 位

    ; 3. SVC #0 → 首次启动 (已有逻辑)
    cmp     r3, #0
    beq     .L_first_switch

    ; 4. SVC #1 → 通用系统调用
    cmp     r3, #1
    beq     .L_general_syscall

    ; 5. 切换到 MSP (内核栈)
    ;    保存用户 R4-R11 到 PSP
    ;    调用 kern_syscall_handler(syscall_num, args)
```

### 3.6 文件变更清单

| 操作 | 文件 | 说明 |
|------|------|------|
| **修改** | `src/kernel/kernel.c` | 实现 `kern_syscall_handler` 分发表 |
| **修改** | `src/arch/arm/cortex-m7/context.S` | SVC_Handler 重构，MPU 配置 |
| **新增** | `src/kernel/syscall/syscall.c` | syscall 表、参数拷贝、权限校验 |
| **新增** | `src/kernel/syscall/syscall.h` | syscall 号定义、用户态 API 包装 |
| **新增** | `src/kernel/mpu/mpu.c` | MPU region 编程、子区域守卫、地址空间切换 |
| **新增** | `src/kernel/mpu/mpu.h` | MPU 配置宏、region 定义 |
| **修改** | `src/kernel/task/task.c` | TCB 扩展、用户任务创建、MPU region 分配 |
| **修改** | `src/kernel/include/kernel_types.h` | TCB 新增字段 |
| **修改** | `link/stm32f767.ld` | 用户任务段 (可选, 简化版共用代码段) |
| **新增** | `src/tests/test_mpu.c` | MPU 违规测试、syscall 测试 |
| **修改** | `Kconfig` | 新增 MPU_ENABLE, SYSCALL_TABLE_SIZE |

---

## 四、Phase 2: 能力系统 (Capability)

### 4.1 目标

- 用能力令牌替代原始 object ID + 所有权检查
- 每个任务持有能力集合 (capability set)，限制可访问的对象
- 能力可传递 (move/copy between tasks)
- 所有 syscall 对象的引用都通过能力令牌

### 4.2 能力模型

```
capability_t {
    uint16_t token;      // 不透明令牌 (随机生成, 非 ID)
    uint16_t rights;     // 权限位图
    task_id_t owner;     // 当前持有者
    void    *object;     // 指向内核对象 (sem_t*, mutex_t*, ...)
}

权限位图:
  bit 0: READ     (sem_wait, mqueue_recv, event_wait)
  bit 1: WRITE    (sem_post, mqueue_send, event_set)
  bit 2: MANAGE   (delete, reset)
  bit 3: TRANSFER (可以传递给其他任务)
  bit 4: GRANT    (可以创建子能力)
```

### 4.3 能力操作

```
cap_create(object, rights) → cap_id
cap_derive(cap_id, subset_rights) → new_cap_id   // 降权派生
cap_transfer(cap_id, target_task) → KERN_OK       // 转移所有权
cap_revoke(cap_id) → KERN_OK                      // 撤销所有派生
cap_delete(cap_id) → KERN_OK
cap_verify(cap_id, required_rights) → KERN_OK     // 访问控制
```

### 4.4 syscall 能力校验流程

```
用户调用 sem_wait(cap_id):
  → SVC #1, syscall_num=10, R1=cap_id
  → kern_syscall_handler:
      1. 查找能力令牌 → capability_t
      2. 验证 owner == current_task
      3. 验证 rights & CAP_READ
      4. 获取 object → sem_t*
      5. 调用 sem_wait_impl(sem)
      6. 返回结果
```

### 4.5 文件变更

| 操作 | 文件 | 说明 |
|------|------|------|
| **新增** | `src/kernel/cap/capability.c` | 能力池、create/derive/transfer/revoke/delete |
| **新增** | `src/kernel/cap/capability.h` | 能力 API |
| **修改** | `src/kernel/syscall/syscall.c` | syscall 入口增加能力校验 |
| **修改** | `src/kernel/include/kernel_types.h` | 更新 capability_t (已在 `#if` 块中) |
| **修改** | `src/kernel/task/task.c` | TCB 绑定能力集 |
| **新增** | `src/tests/test_capability.c` | 能力生命周期测试 |

---

## 五、Phase 3: VFS 虚拟文件系统 + inode 文件对象

> **VFS 和 inode 是设备驱动框架的基石。open/read/write/ioctl 必须通过 syscall → VFS (路径→inode) → inode->ops → 驱动。**

### 5.1 架构分层

```
                        ┌──────────────────────────┐
                        │      用户任务              │
                        │  fd = open("/dev/uart0")  │
                        │  read(fd, buf, n)         │
                        └────────────┬─────────────┘
                                     │ SVC syscall
                                     ▼
                        ┌──────────────────────────┐
                        │    syscall dispatch       │
                        │  sys_open / sys_read ...   │
                        └────────────┬─────────────┘
                                     │
          ┌──────────────────────────┼──────────────────────────┐
          │                          ▼                          │
          │    ╔══════════════════════════════════════════╗     │
          │    ║         VFS 层 (虚拟文件系统切换)         ║     │
          │    ║  • 文件系统类型注册 (fs_type_t)          ║     │
          │    ║  • 超级块 / 挂载表                      ║     │
          │    ║  • 路径解析 → 找到 inode                ║     │
          │    ║  • 文件描述符表 (per-task fd_table)      ║     │
          │    ║  • dentry 目录项缓存 (可选, 简化版省略)  ║     │
          │    ╚══════════════════════════════════════════╝     │
          │                          │                          │
          │                          │ inode*                    │
          │                          ▼                          │
          │    ╔══════════════════════════════════════════╗     │
          │    ║       inode 层 (文件对象)                ║     │
          │    ║  • inode 是核心: 代表"一个文件"          ║     │
          │    ║  • inode 类型决定操作表                  ║     │
          │    ║  • inode->ops → 字符设备/块设备/目录/... ║     │
          │    ╚══════════════════════════════════════════╝     │
          │                          │                          │
          │         ┌────────────────┼────────────────┐         │
          │         ▼                ▼                 ▼         │
          │  ┌────────────┐  ┌────────────┐  ┌────────────┐    │
          │  │ 字符设备    │  │  块设备     │  │  普通文件   │    │
          │  │ inode       │  │  inode      │  │  inode      │    │
          │  │ ops=cdev_ops│  │ ops=bdev_ops│  │ ops=file_ops│   │
          │  └─────┬──────┘  └──────┬──────┘  └──────┬──────┘    │
          │        │                │                  │          │
          │        ▼                ▼                  ▼          │
          │  ┌────────────┐  ┌────────────┐  ┌────────────┐    │
          │  │ UART 驱动   │  │ SD/Flash   │  │  ramfs     │    │
          │  │ (Phase 4)   │  │ 驱动(预留)  │  │  实现      │    │
          │  └────────────┘  └────────────┘  └────────────┘    │
          └─────────────────────────────────────────────────────┘
```

**关键设计原则：**
- **VFS 不做 IO** — VFS 只管路径→inode 的查找，实际的 open/read/write 委托给 `inode->ops`
- **inode 决定操作** — 不同类型 inode 绑定不同 `file_operations` 表
- **字符设备 inode** — `inode->ops->read = cdev_read`，内部再调具体驱动的 `dev_ops->read`

### 5.2 inode 层 — 文件对象核心

inode 是 VFS 中最核心的抽象，代表文件系统中的一个对象。**用户做的所有操作 (open/read/write/close) 最终都落在 inode 的 ops 上。**

```c
// ===== inode 类型 =====

typedef enum {
    INODE_TYPE_FILE    = 0,   // 普通文件 (ramfs)
    INODE_TYPE_DIR     = 1,   // 目录
    INODE_TYPE_CHRDEV  = 2,   // 字符设备 (UART, GPIO, null...)
    INODE_TYPE_BLKDEV  = 3,   // 块设备 (SD, Flash...)
    INODE_TYPE_PIPE    = 4,   // 管道 (FIFO)
    INODE_TYPE_SYMLINK = 5,   // 符号链接
    INODE_TYPE_SOCKET  = 6,   // (预留) 套接字
} inode_type_t;

// ===== inode 结构 =====
// 这是文件系统的核心对象

typedef struct inode {
    // --- 标识 ---
    uint32_t    ino;                       // inode 号 (唯一 ID)
    char        name[INODE_NAME_LEN];      // 文件名 (目录项中的名字)
    inode_type_t type;                     // 文件类型

    // --- 属性 ---
    uint32_t    flags;                     // 打开标志
    uint32_t    mode;                      // 权限模式 (r/w/x)
    uint32_t    size;                      // 文件大小 (普通文件)
    uint32_t    refcount;                  // 引用计数
    uint32_t    nlink;                     // 硬链接数

    // === 核心: 文件操作表 ===
    // inode 类型决定此表指向谁:
    //   字符设备 inode → cdev_fops (字符设备通用操作)
    //   块设备 inode   → bdev_fops
    //   目录 inode     → dir_fops
    //   普通文件 inode → file_fops (ramfs)
    struct file_operations *ops;

    // === 文件系统操作表 ===
    // 目录/元数据操作 (仅目录类型 inode 需要)
    struct inode_operations *iops;

    // --- 私有数据 ---
    void       *private_data;              // 文件系统/驱动私有数据

    // 字符设备 inode: private_data → device_t (Phase 4 设备描述符)
    // 普通文件 inode: private_data → ramfs 数据块
    // 目录 inode:     private_data → 子节点链表

    // --- 树结构 (目录层级) ---
    struct inode *parent;                  // 父目录
    struct inode *children;               // 子节点链表头
    struct inode *next_sibling;           // 下一个兄弟节点

    // --- 能力 ---
    cap_id_t    cap;                       // 此 inode 的能力令牌
} inode_t;
```

### 5.3 操作表 — file_operations 和 inode_operations

```c
// ==========================================
// file_operations — 文件操作 (所有 inode 类型都需要)
// ==========================================
// 这是用户通过 fd 调用的接口:
//   read(fd, buf, n)  →  inode->ops->read(inode, buf, off, n)
//   write(fd, buf, n) →  inode->ops->write(inode, buf, off, n)

typedef struct file_operations {
    kern_err_t (*open)(struct inode *inode, uint32_t flags);
    kern_err_t (*close)(struct inode *inode);
    int32_t    (*read)(struct inode *inode, void *buf, uint32_t offset, uint32_t size);
    int32_t    (*write)(struct inode *inode, const void *buf, uint32_t offset, uint32_t size);
    kern_err_t (*ioctl)(struct inode *inode, uint32_t cmd, void *arg);

    // 可选
    kern_err_t (*mmap)(struct inode *inode, void *addr, uint32_t size);
    kern_err_t (*poll)(struct inode *inode, uint32_t events);
} file_operations_t;

// ==========================================
// inode_operations — inode 元数据操作 (仅目录类型需要)
// ==========================================
// 用于目录遍历、创建/删除节点

typedef struct inode_operations {
    kern_err_t (*lookup)(struct inode *dir, const char *name, struct inode **result);
    kern_err_t (*create)(struct inode *dir, const char *name, inode_type_t type);
    kern_err_t (*unlink)(struct inode *dir, const char *name);
    kern_err_t (*mkdir)(struct inode *dir, const char *name);
    kern_err_t (*rmdir)(struct inode *dir, const char *name);
    kern_err_t (*readdir)(struct inode *dir, uint32_t index, struct dirent *entry);
    kern_err_t (*rename)(struct inode *olddir, const char *oldname,
                         struct inode *newdir, const char *newname);
} inode_operations_t;
```

### 5.4 各 inode 类型的操作表绑定

```c
// ===== 字符设备 inode =====
// inode->type = INODE_TYPE_CHRDEV
// inode->ops  = &cdev_fops
// inode->private_data → device_t (设备描述符, Phase 4 注册)

static file_operations_t cdev_fops = {
    .open  = cdev_open,    // → device_t->ops->open(dev)
    .close = cdev_close,   // → device_t->ops->close(dev)
    .read  = cdev_read,    // → device_t->ops->read(dev, buf, off, size)
    .write = cdev_write,   // → device_t->ops->write(dev, buf, off, size)
    .ioctl = cdev_ioctl,   // → device_t->ops->ioctl(dev, cmd, arg)
};

// cdev_read 实现:
// static int32_t cdev_read(inode_t *inode, void *buf, uint32_t off, uint32_t size) {
//     device_t *dev = (device_t *)inode->private_data;
//     return dev->ops->read(dev, buf, off, size);  ← 调用具体驱动
// }

// ===== 普通文件 inode (ramfs) =====
// inode->type = INODE_TYPE_FILE
// inode->ops  = &ramfs_file_fops
// inode->private_data → ramfs 数据块

static file_operations_t ramfs_file_fops = {
    .open  = generic_open,
    .close = generic_close,
    .read  = ramfs_read,   // 从 private_data 数据块读取
    .write = ramfs_write,  // 写入 private_data 数据块
    .ioctl = NULL,         // 普通文件不支持 ioctl
};

// ===== 目录 inode =====
// inode->type = INODE_TYPE_DIR
// inode->ops  = &dir_fops        (目录不支持 read/write)
// inode->iops = &dir_iops        (目录支持 lookup/readdir/create)

static file_operations_t dir_fops = {
    .open  = generic_open,
    .close = generic_close,
    .read  = NULL,           // 目录不能用 read()
    .write = NULL,           // 目录不能用 write()
    .ioctl = NULL,
};

static inode_operations_t dir_iops = {
    .lookup  = generic_lookup,   // 在子节点中按名查找
    .readdir = generic_readdir,  // 遍历子节点
    .create  = generic_create,   // 创建新节点
    .unlink  = generic_unlink,
};

// ===== 管道 inode (预留) =====
// inode->type = INODE_TYPE_PIPE
// inode->ops  = &pipe_fops

static file_operations_t pipe_fops = {
    .open  = pipe_open,
    .close = pipe_close,
    .read  = pipe_read,    // 从环形缓冲区读
    .write = pipe_write,   // 向环形缓冲区写
    .ioctl = NULL,
};
```

### 5.5 VFS 层 — 文件系统切换

VFS 不关心 inode 的具体类型，只做路由：

```c
// ===== 文件系统类型注册 =====
typedef struct fs_type {
    const char   *name;                    // "devfs", "ramfs"
    kern_err_t   (*mount)(struct fs_type *fs, const char *path);
    kern_err_t   (*unmount)(struct fs_type *fs);
    struct fs_type *next;                  // 链表
} fs_type_t;

// ===== 超级块 (挂载点) =====
typedef struct super_block {
    fs_type_t   *fs_type;                 // 文件系统类型
    inode_t     *root_inode;             // 根 inode
    inode_t     *mount_point;            // 挂载到的目录 inode
    char         mount_path[64];         // 挂载路径
    uint32_t     flags;
} super_block_t;
```

### 5.6 文件描述符表

每个任务持有独立的 fd 表：

```c
#define MAX_FDS_PER_TASK 16

typedef struct {
    inode_t    *inode;        // 指向打开的 inode
    uint32_t    flags;        // O_RDONLY / O_WRONLY / O_RDWR
    uint32_t    offset;       // 当前读写偏移
    uint8_t     in_use;
} fd_entry_t;

// TCB 新增
typedef struct tcb {
    // ... 现有字段 ...
    fd_entry_t  fd_table[MAX_FDS_PER_TASK];
} tcb_t;
```

### 5.7 完整调用路径: open("/dev/uart0")

```
sys_open("/dev/uart0", O_RDWR)
  │
  │  [VFS 层] 路径解析
  ├─ vfs_lookup("/dev/uart0")
  │   ├─ 从根 inode "/" 开始
  │   ├─ 查找子节点 "dev" → 遇到 devfs 挂载点 → 进入 devfs 的 root_inode
  │   ├─ 在 devfs 中查找 "uart0" → 找到字符设备 inode
  │   └─ 返回 inode*
  │
  │  [VFS 层] 分配 fd
  ├─ fd_alloc(current_task)
  │   └─ fd_table[n] = { .inode = inode, .flags = O_RDWR, .offset = 0 }
  │
  │  [inode 层] 调用 inode 的操作表
  ├─ inode->ops->open(inode, O_RDWR)
  │   │
  │   │  (inode 类型是 CHRDEV, ops = &cdev_fops)
  │   │
  │   └─ cdev_open(inode, flags)
  │       └─ device_t *dev = inode->private_data
  │           └─ dev->ops->open(dev, flags)     ← 调用具体驱动的 open()
  │
  └─ 返回 fd
```

### 5.8 文件系统实例

```c
// ===== devfs — 设备文件系统 =====
// 挂载点: /dev
// 所有字符设备/块设备在此注册
//
// /dev/uart0    → inode (CHRDEV, ops=cdev_fops, private_data=uart_device)
// /dev/gpio     → inode (CHRDEV, ops=cdev_fops, private_data=gpio_device)
// /dev/null     → inode (CHRDEV, ops=cdev_fops, private_data=null_device)

// devfs 注册一个设备:
kern_err_t devfs_register_device(device_t *dev) {
    inode_t *inode = inode_alloc();
    inode->type = INODE_TYPE_CHRDEV;
    inode->ops  = &cdev_fops;            // 字符设备通用操作表
    inode->private_data = dev;           // 绑定设备
    strncpy(inode->name, dev->name, INODE_NAME_LEN);
    inode_add_child(devfs_root, inode);  // 加入 /dev 目录
    return KERN_OK;
}

// ===== ramfs — 内存文件系统 =====
// 挂载点: /tmp
// 简单的内存文件, 用于临时数据、配置

typedef struct {
    uint8_t    *data;      // 数据缓冲区
    uint32_t    size;      // 当前大小
    uint32_t    capacity;  // 最大容量
} ramfs_block_t;

// ramfs 创建文件:
kern_err_t ramfs_create_file(inode_t *dir, const char *name) {
    inode_t *inode = inode_alloc();
    inode->type = INODE_TYPE_FILE;
    inode->ops  = &ramfs_file_fops;
    inode->private_data = ramfs_block_alloc();
    inode_add_child(dir, inode);
    return KERN_OK;
}
```

### 5.9 VFS syscall 接口

```c
// Phase 3 新增的 syscall (通过 SVC #1)

SYSCALL_OPEN       = 24,   // fd = open(path, flags)
SYSCALL_CLOSE      = 25,   // close(fd)
SYSCALL_READ       = 26,   // n = read(fd, buf, size)
SYSCALL_WRITE      = 27,   // n = write(fd, buf, size)
SYSCALL_IOCTL      = 28,   // ioctl(fd, cmd, arg)
SYSCALL_LSEEK      = 29,   // lseek(fd, offset, whence)
SYSCALL_STAT       = 30,   // stat(path, &statbuf)
SYSCALL_MOUNT      = 31,   // mount(fs_type, path)
SYSCALL_UMOUNT     = 32,   // umount(path)
SYSCALL_OPENDIR    = 33,   // opendir/readdir/closedir
```

### 5.10 文件变更

| 操作 | 文件 | 说明 |
|------|------|------|
| **新增** | `src/kernel/vfs/vfs.c` | VFS 层: 路径解析、挂载表、文件系统注册 |
| **新增** | `src/kernel/vfs/vfs.h` | fs_type_t, super_block_t, dentry |
| **新增** | `src/kernel/vfs/inode.c` | inode 层: 分配/释放、引用计数、树操作 |
| **新增** | `src/kernel/vfs/inode.h` | inode_t, inode_type_t, file_operations_t, inode_operations_t |
| **新增** | `src/kernel/vfs/fd.c` | 文件描述符表管理 (alloc/free/lookup) |
| **新增** | `src/kernel/vfs/cdev.c` | 字符设备通用操作 (cdev_fops: open/read/write/ioctl 转发) |
| **新增** | `src/kernel/vfs/cdev.h` | cdev 接口 |
| **新增** | `src/kernel/vfs/devfs.c` | 设备文件系统 (inode 类型=CHRDEV, 绑定 device_t) |
| **新增** | `src/kernel/vfs/devfs.h` | devfs 接口 |
| **新增** | `src/kernel/vfs/ramfs.c` | 内存文件系统 (inode 类型=FILE+DIR) |
| **新增** | `src/kernel/vfs/ramfs.h` | ramfs 接口 |
| **修改** | `src/kernel/syscall/syscall.c` | 新增 vfs_* syscall 分发表 (24-33) |
| **修改** | `src/kernel/task/task.c` | TCB 初始化 fd_table |
| **修改** | `src/kernel/include/kernel_types.h` | 新增 inode_t, fd_entry_t, file_operations_t 类型 |
| **新增** | `src/tests/test_vfs.c` | VFS 路径解析、挂载/卸载测试 |
| **新增** | `src/tests/test_inode.c` | inode 生命周期、各类型操作表测试 |
| **新增** | `src/tests/test_devfs.c` | devfs 设备注册和 open/read/write 测试 |
| **修改** | `Kconfig` | 新增 VFS_ENABLE, MAX_FDS, MAX_INODES |

---

## 六、Phase 4: 设备驱动框架

> **依赖 Phase 3 (VFS)。驱动挂载在 devfs 下，用户通过 fd 访问。**

### 6.1 分层关系

```
用户 syscall (open/read/write/ioctl)
   │
   ▼
VFS (vfs_open/vfs_read/vfs_write ...)
   │
   ▼
devfs (/dev/uart0, /dev/gpio, ...)
   │
   ▼
设备驱动 (dev_ops → 硬件操作)
```

驱动框架不做路径管理和 fd 分配，这些由 VFS 完成。驱动只负责：

1. 向 devfs 注册设备节点
2. 实现 `vnode_ops_t` 操作表
3. 处理硬件中断 (通过 BH 或线程化 IRQ)

### 6.2 设备驱动抽象

```c
// 设备操作表 (实现 vnode_ops_t 的子集)
// devfs 的 vnode_ops 转发到具体设备
typedef struct {
    kern_err_t (*open)(void *dev, uint32_t flags);
    kern_err_t (*close)(void *dev);
    int32_t    (*read)(void *dev, void *buf, uint32_t offset, uint32_t size);
    int32_t    (*write)(void *dev, const void *buf, uint32_t offset, uint32_t size);
    kern_err_t (*ioctl)(void *dev, uint32_t cmd, void *arg);
} dev_ops_t;

// 设备描述符
typedef struct {
    char        name[16];          // 设备名 ("uart0")
    dev_ops_t  *ops;              // 设备操作
    void       *priv;             // 驱动私有数据 (寄存器基址等)
    vnode_t    *vnode;            // 关联的 VFS 节点 (devfs 创建)
    cap_id_t    irq_cap;          // IRQ 能力
    uint32_t    irq_num;          // 硬件 IRQ 号
    uint8_t     in_use;
} device_t;

// 驱动向 devfs 注册
kern_err_t driver_register(device_t *dev);
// 内部:
//   1. vfs_create_vnode("/dev/<name>", VNODE_TYPE_DEVICE)
//   2. vnode->ops = &devfs_ops (devfs 的通用 vnode_ops)
//   3. vnode->device = dev
//   4. dev->vnode = vnode
//   5. vfs_mount_vnode("/dev/", vnode)
```

### 6.3 设备中断路由

```
硬件 IRQ →
  ISR (内核, bh_schedule 或 irq_request_threaded) →
    驱动任务 (用户态, 持有 IRQ 能力) →
      driver 内部处理 →
        (可选) 唤醒等待 read() 的用户任务
```

### 6.4 驱动实例: UART

```c
// uart_driver.c — 改造现有 uart_stm32.c

// 1. 实现 dev_ops_t
static dev_ops_t uart_ops = {
    .open  = uart_open,
    .close = uart_close,
    .read  = uart_read,     // 对接现有 UART 接收
    .write = uart_write,    // 对接现有 UART 发送
    .ioctl = uart_ioctl,    // 波特率、流控配置
};

// 2. 中断 → BH → 驱动任务
// ISR: uart_isr() → bh_schedule(uart_bh) → 驱动任务处理

// 3. 注册到 devfs
device_t uart_dev = {
    .name = "uart0",
    .ops  = &uart_ops,
    .priv = &uart_regs,
    .irq_num = USART3_IRQ,
};
driver_register(&uart_dev);

// 4. 用户任务使用
// fd = open("/dev/uart0", O_RDWR);
// read(fd, buf, 100);
// close(fd);
```

### 6.5 设备类型

| 类型 | vnode_type | 示例 |
|------|-----------|------|
| 字符设备 | VNODE_TYPE_DEVICE | UART, GPIO, I2C, SPI |
| 块设备 | VNODE_TYPE_BLOCK | (预留) SD 卡, Flash |
| 网络设备 | VNODE_TYPE_NET | (预留) Ethernet |
| 虚拟设备 | VNODE_TYPE_DEVICE | /dev/null, /dev/zero |

### 6.6 文件变更

| 操作 | 文件 | 说明 |
|------|------|------|
| **新增** | `src/kernel/dev/device.c` | 设备注册表, driver_register/unregister |
| **新增** | `src/kernel/dev/device.h` | dev_ops_t, device_t |
| **新增** | `src/drivers/uart_dev.c` | UART 驱动改造为设备模型 (用户态) |
| **新增** | `src/drivers/gpio_dev.c` | GPIO 驱动 |
| **修改** | `src/kernel/vfs/devfs.c` | 设备节点创建集成 |
| **修改** | `src/kernel/syscall/syscall.c` | 无需新 syscall (复用 open/read/write/ioctl) |
| **修改** | `src/kernel/irq/irq.c` | IRQ 能力绑定到设备 |
| **新增** | `src/tests/test_device.c` | 设备注册、open/read/write 测试 |

---

## 七、Phase 5: IPC 升级 — 同步消息传递

> **与 VFS/驱动框架并行开发，互不阻塞。共享能力系统和 syscall 基础设施。**

### 7.1 目标

- 实现客户端-服务器 (C/S) 通信模型
- 提供 endpoint (端点) / channel (通道) 抽象
- 支持跨地址空间的零拷贝消息
- 保留现有 sem/mutex/mqueue/event 作为内核内部原语
- 端点可暴露为 VFS 节点 (`/dev/endpoint/<name>`)，用户通过 fd 访问

### 7.2 消息传递模型

```
客户端                    服务端
   │                         │
   │  fd = open("/dev/ep/svc")│
   │  send(fd, msg)          │
   ├────────────────────────▶│
   │                         │ recv(fd, &msg)
   │  recv_reply(fd, &resp) │
   │◀────────────────────────┤ reply(fd, resp)
   │                         │
```

### 7.3 IPC 类型定义

```c
// 端点 (多对一通信) — 服务端监听, 多客户端可连接
typedef struct {
    vnode_t    *vnode;           // VFS 节点 (可 open 为 fd)
    cap_id_t    cap;             // 能力令牌
    wait_queue_t recv_waiters;   // 等待接收的客户端
    wait_queue_t reply_waiters;  // 等待回复的客户端
    void       *buffer;          // 消息缓冲区
    uint16_t    msg_size;        // 消息大小
    uint16_t    max_clients;     // 最大客户端数
    uint8_t     in_use;
} endpoint_t;

// 通道 (一对一通信) — 带共享内存
typedef struct {
    cap_id_t    cap;
    task_id_t   peer_a;
    task_id_t   peer_b;
    void       *shm;             // 共享内存 (MPU 映射)
    uint32_t    shm_size;
    uint8_t     in_use;
} channel_t;
```

### 7.4 文件变更

| 操作 | 文件 | 说明 |
|------|------|------|
| **新增** | `src/kernel/ipc/endpoint.c/h` | 端点实现, VFS 集成 |
| **新增** | `src/kernel/ipc/channel.c/h` | 通道实现 (共享内存) |
| **修改** | `src/kernel/syscall/syscall.c` | 新增 IPC syscall (34-38) |
| **新增** | `src/tests/test_endpoint.c` | C/S 通信测试 |
| **新增** | `src/tests/test_channel.c` | 共享内存通道测试 |

---

## 八、Phase 6: 异常容错 + 内核安全

### 7.1 目标

- 用户任务崩溃 (MemManage/BusFault/UsageFault) 不拖死内核
- HardFault 记录诊断信息后可控重启
- 看门狗完整实现
- 内核栈溢出保护

### 7.2 异常处理链

```
MemManage Fault (MPU 违规):
  → MemManage_Handler
    → 读取 CFSR, MMFAR
    → 判断违规地址
    → 如果来自用户任务:
        → 记录诊断信息到 TCB
        → task_terminate(当前任务)
        → 调度下一个任务
    → 如果来自内核:
        → kern_panic("kernel MPU fault")

HardFault (未恢复的 fault 或内核级错误):
  → HardFault_Handler
    → 保存完整上下文到保留 SRAM
    → 记录 fault 寄存器 (CFSR, HFSR, BFAR, MMFAR)
    → reboot_system()
```

### 7.3 Fault 诊断寄存器

```
CFSR (Configurable Fault Status Register, 0xE000ED28):
  [7:0]   MMFSR  — MemManage Fault
  [15:8]  BFSR   — Bus Fault
  [31:16] UFSR   — Usage Fault

HFSR (HardFault Status Register, 0xE000ED2C):
  [30]    FORCED  — 由其他 fault 升级而来
  [1]     VECTTBL — 向量表读取错误

MMFAR (MemManage Fault Address Register, 0xE000ED34):
  触发 MemManage Fault 的地址

BFAR (Bus Fault Address Register, 0xE000ED38):
  触发 Bus Fault 的地址
```

### 7.4 文件变更

| 操作 | 文件 | 说明 |
|------|------|------|
| **新增** | `src/kernel/fault/fault.c/h` | 4 种 Fault handler 实现 |
| **新增** | `src/startup/arm/fault_handlers.S` | Fault 入口 (保存上下文到保留区域) |
| **修改** | `src/startup/arm/startup.S` | 向量表指向 fault handlers |
| **修改** | `src/kernel/kernel.c` | kern_panic 增强 (保存诊断, 尝试恢复) |
| **修改** | `src/arch/arm/cortex-m7/hal.c` | 看门狗 Kconfig 化 |
| **新增** | `src/tests/test_fault.c` | 故障注入测试 (MPU 违规、栈溢出) |
| **修改** | `link/stm32f767.ld` | 保留 crash dump SRAM 区域 |

---

## 九、Phase 7: 诊断与调试生态

### 8.1 目标

- 任务级 CPU 使用率实时统计
- 内核事件 trace buffer (任务切换、ISR、IPC、syscall)
- 运行时 shell (通过 UART)
- 崩溃转储分析工具

### 8.2 Trace 系统

```c
typedef enum {
    TRACE_TASK_SWITCH,
    TRACE_ISR_ENTER,
    TRACE_ISR_EXIT,
    TRACE_SYSCALL_ENTER,
    TRACE_SYSCALL_EXIT,
    TRACE_IPC_SEND,
    TRACE_IPC_RECV,
    TRACE_BH_SCHEDULE,
    TRACE_FAULT,
} trace_event_t;

typedef struct {
    uint32_t    timestamp;
    trace_event_t event;
    uint16_t    task_id;
    uint16_t    data;       // syscall_num, irq_num, ipc_id...
} trace_entry_t;

#define TRACE_BUFFER_SIZE 256
static trace_entry_t trace_buffer[TRACE_BUFFER_SIZE];
```

### 8.3 文件变更

| 操作 | 文件 | 说明 |
|------|------|------|
| **新增** | `src/kernel/trace/trace.c/h` | 环形 trace buffer |
| **新增** | `src/kernel/shell/shell.c/h` | UART 命令行 (ps, top, trace, mem) |
| **新增** | `src/kernel/stats/stats.c/h` | CPU 使用率统计 |
| **新增** | `tools/trace_parser.py` | trace 数据解析器 |
| **修改** | `src/kernel/core/scheduler.c` | 埋 trace 点 |
| **修改** | `src/tests/test_framework.c` | shell 集成 |

---

## 十、完整文件树 (最终形态)

```
src/
├── kernel/
│   ├── kernel.c                          # 内核主入口
│   ├── system_init.c/h
│   │
│   ├── include/
│   │   ├── kernel.h
│   │   ├── kernel_types.h               # 扩展: TCB attrs, MPU, cap_set
│   │   ├── kernel_config.h
│   │   └── spinlock.h
│   │
│   ├── core/
│   │   └── scheduler.c/h
│   │
│   ├── task/
│   │   └── task.c/h                      # 扩展: 用户任务创建, MPU 绑定
│   │
│   ├── syscall/                          ★ Phase 1 新增
│   │   ├── syscall.c                     # syscall 分发表 + 权限校验
│   │   ├── syscall.h                     # syscall 号定义
│   │   └── user_api.h                    # 用户态 API 包装 (svc 内联)
│   │
│   ├── mpu/                              ★ Phase 1 新增
│   │   ├── mpu.c                         # MPU region 管理
│   │   └── mpu.h                         # region 定义, 子区域宏
│   │
│   ├── cap/                              ★ Phase 2 新增
│   │   ├── capability.c                  # 能力池 + 操作
│   │   └── capability.h
│   │
│   ├── ipc/
│   │   ├── ipc.h
│   │   ├── semaphore.c/h
│   │   ├── mutex.c/h
│   │   ├── mqueue.c/h
│   │   ├── event.c/h
│   │   ├── endpoint.c/h                  ★ Phase 5 新增
│   │   └── channel.c/h                   ★ Phase 5 新增
│   │
│   ├── vfs/                              ★ Phase 3 新增
│   │   ├── vfs.c                         # VFS 核心: vnode, 路径解析, 挂载表
│   │   ├── vfs.h                         # vnode_t, vnode_ops_t, fs_type_t
│   │   ├── fd.c                          # 文件描述符表管理
│   │   ├── devfs.c                       # 设备文件系统
│   │   ├── devfs.h
│   │   ├── ramfs.c                       # 内存文件系统
│   │   └── ramfs.h
│   │
│   ├── dev/                              ★ Phase 4 新增
│   │   ├── device.c                      # 设备注册表, driver_register()
│   │   └── device.h                      # dev_ops_t, device_t
│   │
│   ├── irq/
│   │   ├── irq.c/h
│   │   └── bh.c/h
│   │
│   ├── fault/                            ★ Phase 6 新增
│   │   ├── fault.c                       # MemManage/Bus/Usage/HardFault
│   │   └── fault.h                       # crash_dump_t
│   │
│   ├── trace/                            ★ Phase 7 新增
│   │   ├── trace.c
│   │   └── trace.h
│   │
│   ├── shell/                            ★ Phase 7 新增
│   │   ├── shell.c
│   │   └── shell.h
│   │
│   ├── stats/                            ★ Phase 7 新增
│   │   ├── stats.c
│   │   └── stats.h
│   │
│   ├── timer/
│   │   └── timer.c/h
│   │
│   ├── mem/
│   │   ├── mem.c/h
│   │   └── mempool.c/h
│   │
│   └── lib/
│       └── kstring.c
│
├── arch/arm/cortex-m7/
│   ├── hal.c                             # 扩展: MPU/SCB 操作
│   ├── context.S                         # 扩展: SVC 通用入口, MPU 配置
│   └── fault_entry.S                     ★ Phase 5 新增
│
├── startup/arm/
│   ├── startup.S                         # 扩展: fault vector entries
│   ├── system.c
│   └── boot2.S
│
├── drivers/
│   ├── chip/stm32f7/
│   │   ├── uart_stm32.c
│   │   ├── gpio_stm32.c
│   │   └── uart_dev.c                    ★ Phase 4
│   └── include/
│       ├── uart.h
│       ├── gpio.h
│       └── device.h                      ★ Phase 4
│
├── board/stm32f767/
│   └── ...
│
├── tests/
│   ├── test_framework.c/h
│   ├── test_scheduler.c
│   ├── test_timer.c
│   ├── test_irq.c
│   ├── test_deadlock.c
│   ├── test_mpu.c                        ★ Phase 1
│   ├── test_syscall.c                    ★ Phase 1
│   ├── test_capability.c                 ★ Phase 2
│   ├── test_vfs.c                        ★ Phase 3
│   ├── test_devfs.c                      ★ Phase 3
│   ├── test_device.c                     ★ Phase 4
│   ├── test_endpoint.c                   ★ Phase 5
│   ├── test_channel.c                    ★ Phase 5
│   └── test_fault.c                      ★ Phase 6
│
└── app/
    └── main.c
```

---

## 十一、风险与约束

### 10.1 硬件限制

| 限制 | 影响 | 缓解 |
|------|------|------|
| STM32F767 MPU: 8 regions | 复杂任务可能需要更多 region | 共用代码段，region 复用 |
| 无 MMU (只有 MPU) | 不支持虚拟内存，地址空间固定 | 静态分配，链接脚本控制 |
| 384KB SRAM | 内核+任务总数受限 | 内存池优化，按需分配 |
| 单核 Cortex-M7 | 不支持真正的多核微内核 | 单核优化，spinlock 预留 |

### 10.2 技术风险

| 风险 | 概率 | 缓解 |
|------|------|------|
| SVC 参数拷贝开销 | 中 | 保持参数 ≤ 4 个，复杂参数通过共享内存 |
| MPU region 不足 | 低 | 精简 per-task 分配，代码段共享 |
| syscall 延迟影响实时性 | 低 | syscall 路径极短，临界区极小 |
| 用户态驱动中断延迟 | 中 | 关键 ISR 保留在内核，非关键用线程化 IRQ |
| 能力系统复杂度 | 中 | Phase 2 独立验证后再集成 |

---

## 十二、测试策略

每个 Phase 都需要 **全量回归 + 新增测试**：

| Phase | 新增测试数 (估计) | 关键验证点 |
|-------|-------------------|-----------|
| 1 | 15-20 | MPU 违规触发 MemManage；用户任务无法访问内核；syscall 参数校验 |
| 2 | 10-15 | 能力创建/派生/转移/撤销；权限不足拒绝；令牌不可伪造 |
| 3 | 12-18 | VFS 路径解析；devfs 设备注册；ramfs 读写；fd 表隔离；挂载/卸载 |
| 4 | 8-12  | 设备 open/read/write/ioctl；中断路由到用户驱动；设备注册/注销 |
| 5 | 10-15 | C/S 通信正确性；跨地址空间零拷贝；endpoint 通过 fd 访问 |
| 6 | 10-12 | 栈溢出→Fault→恢复；用户崩溃不拖死内核；看门狗超时重启 |
| 7 | 5-8   | trace 完整性；shell 命令正确性；CPU 使用率准确 |

---

## 十三、里程碑与交付

```
M0: 当前 ── 121/121 测试通过, 单体 RTOS, 功能完整

M1: Phase 1 完成 ── 用户态任务通过 SVC 调用内核
    - 第一个用户任务成功运行
    - MPU 违规被正确捕获
    - 所有 121 回归测试通过 (内核任务兼容模式)

M2: Phase 1+2 完成 ── 能力系统运作
    - syscall 通过能力令牌引用对象
    - 非特权任务无法访问未授权对象

M3: Phase 1-3 完成 ── VFS 基础设施就绪
    - 文件描述符、路径解析、挂载系统可用
    - devfs + ramfs 通过 VFS 访问
    - open/read/write/close 通过 syscall 正常工作

M4: Phase 1-5 完成 ── 真正的微内核
    - 设备驱动运行在用户态
    - C/S IPC 跨地址空间通信
    - 所有外部通信经过 VFS 或 endpoint/channel

M5: Phase 1-7 完成 ── 生产级微内核
    - 驱动框架、异常容错、诊断生态就绪
    - 可在真实项目中部署
```

---

## 附录 A: SVC 指令编码参考

```
ARMv7-M SVC 编码:
  svc #imm8

  指令字: [15:0]
  ┌────────────┬─────┬─────────┐
  │  15 14...8 │ 7:0 │  说明     │
  ├────────────┼─────┼─────────┤
  │ 1101 1111  │ imm │ SVC 指令 │
  └────────────┴─────┴─────────┘

  提取 SVC 号 (在 SVC_Handler 中):
    PC_in_frame → 指向 svc 后下一条指令
    svc_insn = *(uint16_t *)(PC_in_frame - 2)
    svc_num = svc_insn & 0xFF
```

## 附录 B: MPU RBAR/RASR 寄存器格式

```
RBAR (Region Base Address Register):
  [31:5]  ADDR   — 基地址 (32B 对齐)
  [4]     VALID  — 1=有效
  [3:0]   REGION — region 号 (0-7)

RASR (Region Attribute and Size Register):
  [31:29] 保留
  [28]    XN     — Execute Never
  [27:24] AP     — Access Permission (0b011=全访问, 0b110=特权RW/用户RO)
  [23:22] 保留
  [21:19] TEX    — Type Extension
  [18]    S      — Shareable
  [17]    C      — Cacheable
  [16]    B      — Bufferable
  [15:8]  SRD    — SubRegion Disable (每 bit 禁用一个 1/8 子区域)
  [7:6]   保留
  [5:1]   SIZE   — Region Size (log2(实际大小) - 1, 最小 32B=4)
  [0]     ENABLE — 1=使能
```

## 附录 C: 内核/用户栈切换机制

```
用户任务运行时:
  CONTROL.SPSEL = 1  (使用 PSP)
  PSP → 用户任务栈
  MSP → 内核栈

SVC 进入时 (硬件自动):
  1. 硬件保存 xPSR,PC,LR,R12,R3-R0 → PSP (用户栈)
  2. SP 切换到 MSP (CONTROL.SPSEL = 0)
  3. SVC_Handler 运行在内核栈

SVC_Handler 中:
  1. 手动保存 R4-R11 → PSP (用户栈)
  2. 保存 PSP 到 TCB->sp
  3. 调用 kern_syscall_handler (使用 MSP/内核栈)
  4. 恢复 PSP 从 TCB->sp
  5. 手动恢复 R4-R11
  6. 异常返回 → 硬件恢复 xPSR,PC,... → 回到用户模式

关键: 整个 syscall 执行期间使用 MSP (内核栈),
     确保用户栈溢出不会影响内核执行。
```
