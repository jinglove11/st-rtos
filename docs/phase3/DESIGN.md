# Phase 3: VFS 虚拟文件系统 + inode — 实施计划

## Context

Phase 1+2 已完成 (224 tests, 0 failures)。RTOS 现在有完整的 syscall 机制、能力系统、内存管理、中断管理和故障处理。Phase 3 按路线图推进 VFS 虚拟文件系统，为 Phase 4 设备驱动框架提供基石。

核心设计原则：
- **inode 是核心抽象**，inode 类型决定 `file_operations` 表
- **用户通过 fd 访问**，绝不直接接触 inode->ops
- **file_operations 注册进 inode**（通过 inode 类型 → ops 绑定）
- **驱动通过 devfs_register_device 注册**，绑定 dev_ops 到 /dev 下的 CHRDEV inode

---

## 一、调用链全景图

```
用户态                         内核态 (SVC → syscall handler)
───────                        ──────────────────────────────

fd = open("/dev/uart0", O_RDWR)
  → sys_call2(SYSCALL_OPEN, path, flags)
    → SVC #1
      → kern_syscall_handler(32, path, flags, ...)
        → sys_open(path, flags)
          ├── vfs_lookup("/dev/uart0")          // 路径 → inode*
          │   ├── 跳过开头的 "/"
          │   ├── tokenize: ["dev", "uart0"]
          │   ├── cur = root_inode
          │   ├── "dev": cur = lookup_child(root, "dev")
          │   │   ├── 检测 /dev 是挂载点 → cur = devfs_root_inode
          │   ├── "uart0": cur = lookup_child(devfs_root, "uart0")
          │   │   ├── inode type=CHRDEV, ops=&cdev_fops
          │   └── return inode (refcount++)
          │
          ├── fd_alloc(current_task, inode, flags)  // 分配 fd
          │   ├── 扫描 task->fd_table[0..MAX_FDS-1]
          │   ├── fd_table[i] = {inode, flags, offset=0, in_use=1}
          │   └── return i (fd_index)
          │
          ├── inode->ops->open(inode, flags)    // 调用操作表
          │   └── cdev_open(inode, flags)
          │       └── dev_ops = inode->private_data
          │           └── dev_ops->open ? dev_ops->open(dev, flags) : KERN_OK
          │
          └── cap_create((void*)(fd_index+1), CAP_OBJ_FILE, CAP_FULL, owner)
              └── return cap_id_t (用户看到的是 cap token)

n = read(fd, buf, size)
  → sys_call3(SYSCALL_READ, fd, buf, size)
    → sys_read(fd, buf, size)
      ├── cap_resolve(fd, CAP_OBJ_FILE, CAP_READ) → object ptr
      ├── fd_index = (uintptr_t)object - 1
      ├── fd_entry = &task->fd_table[fd_index]
      ├── inode = fd_entry->inode
      ├── result = inode->ops->read(inode, buf, fd_entry->offset, size)
      │   └── cdev_read(inode, buf, offset, size)
      │       └── dev_ops = inode->private_data
      │           └── dev_ops->read(dev, buf, offset, size)
      ├── if result > 0: fd_entry->offset += result
      └── return result

close(fd)
  → sys_call1(SYSCALL_CLOSE, fd)
    → sys_close(fd)
      ├── cap_resolve(fd, CAP_OBJ_FILE, CAP_MANAGE) → object ptr
      ├── inode->ops->close(inode)
      ├── fd_free(task, fd_index)  → 清零 fd_entry
      └── cap_delete(fd)
```

---

## 二、文件描述符系统

### 2.1 fd 本质

fd 是一个 **cap_id_t 令牌**，cap_resolve 返回的 object 指针编码了 fd_index：

```
cap_create 时:
    object = (void *)(uintptr_t)(fd_index + 1)    // +1 避免 NULL (fd_index=0 → object=1)

cap_resolve 时:
    object = cap_resolve(cap_id, CAP_OBJ_FILE, CAP_READ)
    fd_index = (int)((uintptr_t)object - 1)

vfs_open 返回:
    return (int)cap_id_t    // 用户拿到能力令牌

用户调用 read(fd, ...):
    fd = cap_id_t           // 用户传入的是 cap token
    sys_read 内部:
        obj = cap_resolve(fd, CAP_OBJ_FILE, CAP_READ)
        // obj 指向 (fd_index + 1)
        // 通过 TCB 的 fd_table[obj-1] 找到 fd_entry
        // 从中取出 inode + offset
```

### 2.2 fd_entry_t 结构

```c
#define MAX_FDS_PER_TASK 8    // Kconfig 可配: VFS_MAX_FDS

typedef struct {
    inode_t    *inode;        // 指向打开的 inode
    uint32_t    flags;        // O_RDONLY / O_WRONLY / O_RDWR
    uint32_t    offset;       // 当前读写位置 (lseek 修改此值)
    uint8_t     in_use;       // 槽位是否使用中
} fd_entry_t;
```

### 2.3 fd_table 位置

```c
// TCB 中 (kernel_types.h):
typedef struct tcb {
    // ... 现有字段 ...
#if VFS_ENABLE
    fd_entry_t  fd_table[MAX_FDS_PER_TASK];   // 每任务独立 fd 空间
#endif
} tcb_t;
```

每个任务有独立的 fd 空间。fd=0 对任务 A 和任务 B 指向不同的 inode。
`memset(tcb, 0, sizeof(tcb_t))` 自动清零所有 fd_entry。

### 2.4 fd 生命周期

```
open(path, flags):
  1. vfs_lookup(path) → inode*
  2. fd_alloc(task, inode, flags) → fd_index
     - 扫描 fd_table[0..MAX_FDS-1]
     - 找第一个 in_use==0 的槽位
     - 设置 inode, flags, offset=0, in_use=1
     - 返回 fd_index
     - 如果全满: return KERN_ERR_RESOURCE
  3. if inode->ops->open:
       err = inode->ops->open(inode, flags)
       if err: fd_free(fd_index); return err
  4. cap = cap_create((void*)(fd_index+1), CAP_OBJ_FILE,
                       CAP_READ|CAP_WRITE, owner)
  5. return cap (作为 fd)

close(fd):
  1. obj = cap_resolve(fd, CAP_OBJ_FILE, CAP_MANAGE)
     - 失败 → KERN_ERR_CAP
  2. fd_index = (uintptr_t)obj - 1
  3. fd_entry = &task->fd_table[fd_index]
     - if !fd_entry->in_use → KERN_ERR_PARAM
  4. if fd_entry->inode->ops->close:
       fd_entry->inode->ops->close(fd_entry->inode)
  5. inode_put(fd_entry->inode)    // 减引用计数
  6. memset(fd_entry, 0, sizeof(fd_entry_t))
  7. cap_delete(fd)
  8. return KERN_OK

read(fd, buf, size):
  1. obj = cap_resolve(fd, CAP_OBJ_FILE, CAP_READ)
  2. fd_entry = &task->fd_table[(uintptr_t)obj - 1]
  3. result = fd_entry->inode->ops->read(
                fd_entry->inode, buf, fd_entry->offset, size)
  4. if result > 0: fd_entry->offset += result
  5. return result

write(fd, buf, size):
  1. obj = cap_resolve(fd, CAP_OBJ_FILE, CAP_WRITE)
  2. fd_entry = &task->fd_table[(uintptr_t)obj - 1]
  3. result = fd_entry->inode->ops->write(
                fd_entry->inode, buf, fd_entry->offset, size)
  4. if result > 0: fd_entry->offset += result
  5. return result

lseek(fd, offset, whence):
  1. obj = cap_resolve(fd, CAP_OBJ_FILE, CAP_READ)
  2. fd_entry = &task->fd_table[(uintptr_t)obj - 1]
  3. switch whence:
       SEEK_SET: fd_entry->offset = offset
       SEEK_CUR: fd_entry->offset += offset
       SEEK_END: fd_entry->offset = fd_entry->inode->size + offset
  4. return fd_entry->offset

ioctl(fd, cmd, arg):
  1. obj = cap_resolve(fd, CAP_OBJ_FILE, CAP_READ)  // 或 CAP_WRITE
  2. fd_entry = &task->fd_table[(uintptr_t)obj - 1]
  3. return fd_entry->inode->ops->ioctl(fd_entry->inode, cmd, arg)
```

### 2.5 一个 inode 可以被多个 fd 打开

```
Task A: fd=3 → /dev/uart0   (offset=0)
Task B: fd=1 → /dev/uart0   (offset=0, 独立的)
同一个 Task A: fd=5 → /dev/uart0   (offset=0, 另一个 fd)

每个 fd_entry 有独立的 offset，指向同一个 inode。
inode->refcount 跟踪有多少 fd 引用了它。
```

---

## 三、驱动注册系统 (devfs_register_device)

### 3.1 驱动操作表 dev_ops_t

```c
// inode.h 中定义 — 这是驱动需要实现的接口
typedef struct dev_ops {
    kern_err_t (*open)(void *priv, uint32_t flags);
    kern_err_t (*close)(void *priv);
    int32_t    (*read)(void *priv, void *buf, uint32_t offset, uint32_t size);
    int32_t    (*write)(void *priv, const void *buf, uint32_t offset, uint32_t size);
    kern_err_t (*ioctl)(void *priv, uint32_t cmd, void *arg);
} dev_ops_t;
```

### 3.2 注册流程

```
devfs_register_device("uart0", &uart_dev_ops, &uart_priv_data)
  │
  ├── 1. 分配 CHRDEV inode
  │      inode = inode_alloc(INODE_TYPE_CHRDEV, "uart0", &cdev_fops, NULL)
  │
  ├── 2. 绑定私有数据
  │      inode->private_data = &uart_dev_ops    // 驱动操作表
  │      // Phase 4 升级: private_data → device_t {ops, priv, irq, ...}
  │
  ├── 3. 加入 /dev 目录树
  │      inode_add_child(devfs_root_inode, inode)
  │
  └── 返回 KERN_OK
```

### 3.3 cdev_fops 如何分发到具体驱动

cdev_fops 是**所有字符设备共享的**操作表，它通过 inode->private_data 找到具体驱动：

```c
// devfs.c — 字符设备通用操作表

static int32_t cdev_read(struct inode *inode, void *buf,
                         uint32_t offset, uint32_t size) {
    dev_ops_t *ops = (dev_ops_t *)inode->private_data;
    if (!ops || !ops->read) return KERN_ERR;
    return ops->read(inode->private_data, buf, offset, size);
}

static int32_t cdev_write(struct inode *inode, const void *buf,
                          uint32_t offset, uint32_t size) {
    dev_ops_t *ops = (dev_ops_t *)inode->private_data;
    if (!ops || !ops->write) return KERN_ERR;
    return ops->write(inode->private_data, buf, offset, size);
}

static kern_err_t cdev_open(struct inode *inode, uint32_t flags) {
    dev_ops_t *ops = (dev_ops_t *)inode->private_data;
    if (ops && ops->open) return ops->open(inode->private_data, flags);
    return KERN_OK;  // open 是可选的
}

static kern_err_t cdev_close(struct inode *inode) {
    dev_ops_t *ops = (dev_ops_t *)inode->private_data;
    if (ops && ops->close) return ops->close(inode->private_data);
    return KERN_OK;
}

static kern_err_t cdev_ioctl(struct inode *inode, uint32_t cmd, void *arg) {
    dev_ops_t *ops = (dev_ops_t *)inode->private_data;
    if (!ops || !ops->ioctl) return KERN_ERR;
    return ops->ioctl(inode->private_data, cmd, arg);
}

file_operations_t cdev_fops = {
    .open  = cdev_open,
    .close = cdev_close,
    .read  = cdev_read,
    .write = cdev_write,
    .ioctl = cdev_ioctl,
};
```

### 3.4 Phase 4 升级预留

Phase 3 的 dev_ops_t 直接存在 inode->private_data 中。Phase 4 将升级为：

```c
// Phase 4: device_t 包装 dev_ops_t
typedef struct {
    char        name[16];
    dev_ops_t  *ops;
    void       *priv;          // 驱动私有数据 (寄存器基址等)
    uint32_t    irq_num;       // 硬件中断号
    cap_id_t    irq_cap;       // IRQ 能力
    uint8_t     in_use;
} device_t;
```

此时 inode->private_data 指向 device_t，cdev_fops 分发变为：
`dev->ops->read(dev->priv, buf, offset, size)`

---

## 四、核心数据结构 — union ops 设计

### 4.1 设计思路

**union ops_u 只包含文件/设备操作**（file_ops 和 cdev_ops 互斥），目录操作 dir_ops 独立存放：
```
inode->type == FILE   → 使用 ops_u.file_ops  (read/write 数据)
inode->type == CHRDEV → 使用 ops_u.cdev_ops  (open/read/write/ioctl)
inode->type == DIR    → 使用 dir_ops          (lookup/create/unlink/readdir)
```

vfs_read/vfs_write 根据 inode->type 选择正确的 ops。

### 4.2 inode_t

```c
#define INODE_NAME_LEN 32

typedef enum {
    INODE_TYPE_FILE   = 0,   // 普通文件 (ramfs)
    INODE_TYPE_DIR    = 1,   // 目录
    INODE_TYPE_CHRDEV = 2,   // 字符设备 (devfs)
} inode_type_t;

typedef struct inode {
    uint32_t    ino;                       // 唯一 inode 号 (自增)
    char        name[INODE_NAME_LEN];      // 文件名
    inode_type_t type;
    uint32_t    flags;
    uint32_t    size;                      // 文件大小 (FILE 类型使用)
    uint32_t    refcount;                  // 引用计数 (fd 打开次数)

    // ★ 核心: 文件/设备操作放入 union (互斥), 目录操作单独存放
    union {
        struct file_ops  *file_ops;       // INODE_TYPE_FILE
        struct cdev_ops  *cdev_ops;       // INODE_TYPE_CHRDEV
    } ops_u;

    struct dir_ops *dir_ops;              // INODE_TYPE_DIR (独立, 不在 union 中)

    void       *private_data;              // FS/驱动私有数据

    struct inode *parent;
    struct inode *children;               // 子节点链表头
    struct inode *next_sibling;           // 兄弟节点
} inode_t;
```

### 4.3 file_ops — 普通文件操作

```c
typedef struct file_ops {
    kern_err_t (*open)(struct inode *inode, uint32_t flags);
    kern_err_t (*close)(struct inode *inode);
    int32_t    (*read)(struct inode *inode, void *buf, uint32_t offset, uint32_t size);
    int32_t    (*write)(struct inode *inode, const void *buf, uint32_t offset, uint32_t size);
} file_ops_t;
```

### 4.4 cdev_ops — 字符设备操作

```c
typedef struct cdev_ops {
    kern_err_t (*open)(struct inode *inode, uint32_t flags);
    kern_err_t (*close)(struct inode *inode);
    int32_t    (*read)(struct inode *inode, void *buf, uint32_t offset, uint32_t size);
    int32_t    (*write)(struct inode *inode, const void *buf, uint32_t offset, uint32_t size);
    kern_err_t (*ioctl)(struct inode *inode, uint32_t cmd, void *arg);
} cdev_ops_t;
```

### 4.5 dir_ops — 目录操作

```c
typedef struct dir_ops {
    kern_err_t (*lookup)(struct inode *dir, const char *name, struct inode **result);
    kern_err_t (*create)(struct inode *dir, const char *name, inode_type_t type);
    kern_err_t (*unlink)(struct inode *dir, const char *name);
    kern_err_t (*readdir)(struct inode *dir, uint32_t index, struct dirent *entry);
} dir_ops_t;
```

### 4.6 dev_ops_t — 具体驱动操作表 (inode→private_data 指向)

字符设备 inode 的 ops_u.cdev_ops 指向一个通用的 cdev_fops (所有字符设备共享)。
cdev_fops 的方法从 inode->private_data 取出具体驱动的 dev_ops_t，再调用驱动的方法。

```c
typedef struct dev_ops {
    kern_err_t (*open)(void *priv, uint32_t flags);
    kern_err_t (*close)(void *priv);
    int32_t    (*read)(void *priv, void *buf, uint32_t offset, uint32_t size);
    int32_t    (*write)(void *priv, const void *buf, uint32_t offset, uint32_t size);
    kern_err_t (*ioctl)(void *priv, uint32_t cmd, void *arg);
} dev_ops_t;
```

dev_ops_t 的方法第一个参数是 `void *priv` (驱动私有数据)，不是 inode。
cdev_ops_t 的方法第一个参数是 `struct inode *`，负责从 inode→private_data 取 dev_ops_t。

### 4.7 挂载点

```c
#define MOUNT_MAX 4

typedef struct {
    char        path[64];         // 挂载路径 ("/dev", "/tmp")
    inode_t    *root_inode;       // 文件系统根 inode
    uint8_t     in_use;
} mount_entry_t;
```

### 4.8 ops 绑定规则总表

| inode 类型 | ops_u 分支 | dir_ops | private_data |
|-----------|-----------|---------|-------------|
| `INODE_TYPE_FILE` | `.file_ops` → `&ramfs_file_ops` | NULL | `ramfs_data_t*` (buffer+size) |
| `INODE_TYPE_CHRDEV` | `.cdev_ops` → `&cdev_shared_fops` (共享) | NULL | `dev_ops_t*` (驱动操作表) |
| `INODE_TYPE_DIR` | NULL | `&shared_dir_ops` | NULL |

### 4.9 vfs_read 分发示例

```c
int32_t vfs_read(cap_id_t fd, void *buf, uint32_t size) {
    void *obj = cap_resolve(fd, CAP_OBJ_FILE, CAP_READ);
    if (!obj) return KERN_ERR_CAP;

    tcb_t *task = sched_get_current();
    int fd_index = (int)((uintptr_t)obj - 1);
    fd_entry_t *fe = &task->fd_table[fd_index];
    inode_t *inode = fe->inode;

    int32_t result = 0;
    switch (inode->type) {
    case INODE_TYPE_FILE:
        if (inode->ops_u.file_ops && inode->ops_u.file_ops->read)
            result = inode->ops_u.file_ops->read(inode, buf, fe->offset, size);
        break;
    case INODE_TYPE_CHRDEV:
        if (inode->ops_u.cdev_ops && inode->ops_u.cdev_ops->read)
            result = inode->ops_u.cdev_ops->read(inode, buf, fe->offset, size);
        break;
    case INODE_TYPE_DIR:
        result = KERN_ERR_ISDIR;   // 目录不可 read (dir_ops 不在这里)
        break;
    }
    if (result > 0) fe->offset += result;
    return result;
}
```

### 4.10 cdev_ops 共享实例如何分发到具体驱动

```c
// devfs.c — 所有字符设备共享这一套 cdev_ops

static int32_t cdev_shared_read(struct inode *inode, void *buf,
                                uint32_t offset, uint32_t size) {
    dev_ops_t *dev = (dev_ops_t *)inode->private_data;
    if (!dev || !dev->read) return KERN_ERR;
    return dev->read(inode->private_data, buf, offset, size);
}

// ... write/ioctl 同理 ...

cdev_ops_t cdev_shared_fops = {
    .open  = cdev_shared_open,
    .close = cdev_shared_close,
    .read  = cdev_shared_read,
    .write = cdev_shared_write,
    .ioctl = cdev_shared_ioctl,
};
```

---

## 五、路径解析算法

### 5.1 vfs_lookup 详细流程

```c
inode_t *vfs_lookup(const char *path) {
    if (!path || path[0] == '\0') return NULL;

    // "/" → root
    if (strcmp(path, "/") == 0) {
        inode_get(root_inode);
        return root_inode;
    }

    inode_t *cur = root_inode;
    inode_get(cur);

    char work[256];
    strncpy(work, path, sizeof(work) - 1);
    work[255] = '\0';

    char *saveptr;
    char *token = strtok_r(work, "/", &saveptr);

    while (token != NULL) {
        // 检测挂载点: 累积路径是否匹配 mount_table[i].path
        // (简化: 在进入子目录前检测)

        inode_t *child = inode_lookup_child(cur, token);
        if (!child) {
            inode_put(cur);
            return NULL;          // 路径不存在
        }

        inode_put(cur);
        cur = child;
        inode_get(cur);

        token = strtok_r(NULL, "/", &saveptr);
    }

    return cur;  // caller 负责 inode_put
}
```

### 5.2 挂载点处理

vfs_mount 注册后，vfs_lookup 在进入目录前检测：

```c
// vfs_init() 中:
vfs_mount("/dev", dev_dir_inode);   // /dev → devfs 根
vfs_mount("/tmp", tmp_dir_inode);   // /tmp → ramfs 根

// vfs_lookup 中，遍历每一级路径时:
// 如果当前累积路径匹配挂载点 → 切换到文件系统根 inode
```

简化方案 (Phase 3)：`/dev` 和 `/tmp` 的 inode 直接在 root 下创建为 DIR 类型，
devfs_init/ramfs_init 得到这些目录 inode 后，直接在上面添加子节点。
不需要独立的挂载表查询——因为 devfs/ramfs 的根 inode 就是树中的那个 DIR inode。

---

## 六、新增文件清单 (9)

| # | 文件 | 核心内容 |
|---|------|---------|
| 1 | `src/kernel/vfs/inode.h` | inode_t, inode_type_t, file_operations_t, inode_operations_t, dev_ops_t, fd_entry_t, mount_entry_t, dirent, 所有常量与标志 |
| 2 | `src/kernel/vfs/inode.c` | 静态池 inode_pool[MAX_INODES] + bitmap, alloc/free/get/put, add_child/lookup_child/remove_child |
| 3 | `src/kernel/vfs/vfs.h` | vfs_init/lookup/open/close/read/write/ioctl/lseek/mount, fd_alloc/fd_free |
| 4 | `src/kernel/vfs/vfs.c` | vfs_init (创建树), vfs_lookup (路径解析), fd_alloc/fd_free, 全部 vfs_* 实现 |
| 5 | `src/kernel/vfs/devfs.h` | devfs_init, devfs_register_device |
| 6 | `src/kernel/vfs/devfs.c` | cdev_fops, /dev/null dev_ops, devfs_register_device |
| 7 | `src/kernel/vfs/ramfs.h` | ramfs_init, ramfs_create_file, ramfs_create_dir |
| 8 | `src/kernel/vfs/ramfs.c` | ramfs_data_t, ramfs_file_fops, dir_fops, dir_iops |
| 9 | `src/tests/test_vfs.c` | 测试模块 |

## 七、修改文件清单 (8)

| # | 文件 | 变更 |
|---|------|------|
| 1 | `src/kernel/include/kernel_types.h` | KERN_ERR_PERM(-12) / KERN_ERR_NOTDIR(-13) / KERN_ERR_ISDIR(-14)；TCB fd_table |
| 2 | `src/kernel/syscall/syscall.h` | SYSCALL_OPEN..LSEEK (32-37)；SYSCALL_TABLE_SIZE→40 |
| 3 | `src/kernel/syscall/syscall.c` | 6 个 sys_* handler + 分发表条目 |
| 4 | `src/kernel/syscall/user_api.h` | open/close/read/write/ioctl/lseek 内联封装 |
| 5 | `src/kernel/cap/capability.h` | CAP_OBJ_FILE=8；CAP_OBJ_TYPE_MAX→9 |
| 6 | `src/kernel/kernel.c` | #include "vfs.h"；kern_init() 调 vfs_init() |
| 7 | `Kconfig` | VFS Configuration menu；TEST_MODULE_VFS；SYSCALL_TABLE_SIZE default 40 |
| 8 | `Makefile` | KERN_SOURCES += vfs/*.c；CFLAGS += -Isrc/kernel/vfs；TEST_SOURCES += test_vfs.c |

---

## 八、实施步骤 (8 步)

### Step 1: 配置与类型基础设施
- Kconfig: VFS Configuration menu + TEST_MODULE_VFS + SYSCALL_TABLE_SIZE→40
- `.config`: VFS_ENABLE=y, VFS_MAX_FDS=8, VFS_MAX_INODES=32, TEST_MODULE_VFS=y
- `kernel_types.h`: 3 个新错误码, TCB 新增 `fd_entry_t fd_table[VFS_MAX_FDS]`
- `capability.h`: CAP_OBJ_FILE=8, CAP_OBJ_TYPE_MAX→9

### Step 2: inode 系统
- 创建 `inode.h` (全部类型定义、常量、标志)
- 创建 `inode.c` (池管理 + 树操作, 静态池 bitmap 模式)
- 验证编译通过

### Step 3: VFS 核心
- 创建 `vfs.h` + `vfs.c`
- vfs_init() 创建文件系统树 (/ → /dev + /tmp)
- vfs_lookup() 路径解析
- fd_alloc/fd_free 操作 TCB fd_table
- vfs_open/close/read/write/ioctl/lseek 完整实现
- kernel.c 集成 vfs_init()

### Step 4: devfs + 驱动注册
- 创建 `devfs.h` + `devfs.c`
- cdev_fops (cdev_open/read/write/close/ioctl)
- /dev/null dev_ops (null_read 返回 0, null_write 吃数据)
- devfs_init(): 在 /dev 下注册 null 设备
- devfs_register_device(name, dev_ops): 创建 CHRDEV inode

### Step 5: ramfs
- 创建 `ramfs.h` + `ramfs.c`
- ramfs_data_t (buffer/size/capacity)
- ramfs_file_fops (read/write 操作动态 buffer)
- dir_fops + dir_iops (目录树操作)
- ramfs_init(): 在 /tmp 下初始化

### Step 6: Syscall 集成
- `syscall.h`: SYSCALL_OPEN~LSEEK (32-37)
- `syscall.c`: 6 个 handler + 分发表条目 (guarded VFS_ENABLE)
- `user_api.h`: open/close/read/write/ioctl/lseek 内联函数

### Step 7: 测试模块
- `test_vfs.c`: 9 组测试用例
  1. inode alloc/free 生命周期 + 池耗尽
  2. inode refcounting (get/put 自动释放)
  3. inode 树操作 (add/lookup/remove/sibling)
  4. VFS 初始化 (root + /dev + /tmp 存在)
  5. vfs_lookup 路径解析
  6. fd table 操作
  7. /dev/null read/write
  8. ramfs create/read/write/lseek
  9. syscall 分发验证

### Step 8: 硬件验证
- `make BOARD=stm32f767 -j8` → 0 warnings
- Flash STM32F767 → 224 回归通过 + VFS 测试通过

---

## 九、关键设计决策

| # | 决策 | 理由 |
|---|------|------|
| 1 | **fd_entry 嵌入 TCB** | 遵循现有静态分配模式，每任务 8×12B=96B，memset 自动清零 |
| 2 | **cap_id_t 包装 fd_index** | 与其他 IPC 一致: cap_create((void*)(fd_index+1), ...) |
| 3 | **cdev_fops 通过 private_data 分发到具体驱动** | 所有字符设备共享同一套 file_operations，通过 inode→private_data(dev_ops_t*) 区分行为 |
| 4 | **devfs_register_device 绑定驱动** | 驱动注册 → 创建 CHRDEV inode → 绑定 dev_ops → 加入 /dev 树 |
| 5 | **挂载表简化** | devfs/ramfs 根 inode 直接在 root 下创建为 DIR 子节点，无需独立 mount_table |
| 6 | **无 dentry 缓存** | vfs_lookup 每次线性树遍历，嵌入式场景树深度 < 5 |
| 7 | **ramfs buffer kmalloc** | 保持静态池可预测，文件数据动态分配 |
| 8 | **dev_ops_t 预留 priv 参数** | Phase 3 传 private_data 本身，Phase 4 升级为 device_t 结构体 |

---

## 十、验证

1. `make BOARD=stm32f767` — 0 warnings
2. Flash STM32F767 — 224 回归 + VFS 新测试全部通过
3. /dev/null: write 成功丢弃, read 返回 0 (EOF)
4. ramfs: 写入数据 → lseek SEEK_SET → 读回一致
5. 驱动注册: devfs_register_device("test", &test_ops) → vfs_lookup("/dev/test") 存在
6. 路径解析: "/nonexistent" → NULL, "/dev/null" → CHRDEV inode
