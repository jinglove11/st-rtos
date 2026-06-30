# Phase 3: VFS 虚拟文件系统 + inode — 功能完成表

> 状态说明: ⬜ 未开始 | ✅ 已完成并测试通过
> 前置: Phase 2 已完成 (224 tests, 0 failures)
> 编译: **make BOARD=stm32f767 -j8 → 0 warnings**
> 硬件: **313 tests, 0 failures on STM32F767 Nucleo**

---

## 一、inode 系统 (`src/kernel/vfs/`)

| # | 功能 | 文件 | 状态 |
|---|------|------|:----:|
| 1.1 | `inode.h` — inode_t, inode_type_t, file_ops_t, cdev_ops_t, dir_ops_t, dev_ops_t, fd_entry_t, dirent_t 类型定义 | `inode.h` | ✅ |
| 1.2 | `inode.c` — 静态 inode 池 `inode_pool[MAX_INODES]` + bitmap | `inode.c` | ✅ |
| 1.3 | `inode_init()` — 池清零、bitmap 复位、ino 计数器初始化 | `inode.c` | ✅ |
| 1.4 | `inode_alloc(type, name)` → inode* — 分配 inode, refcount=1 | `inode.c` | ✅ |
| 1.5 | `inode_free(inode)` — 直接摘除父节点链表 + 释放池槽位 | `inode.c` | ✅ |
| 1.6 | `inode_get/inode_put` — 引用计数 +/-, refcount=0 自动调用 inode_free | `inode.c` | ✅ |
| 1.7 | `inode_add_child(parent, child)` — 加入子节点链表, inode_get(child) 树持有引用 | `inode.c` | ✅ |
| 1.8 | `inode_lookup_child(dir, name)` — 按名查找子 inode | `inode.c` | ✅ |
| 1.9 | `inode_remove_child(parent, name)` — 从链表摘除子节点, inode_put(child) 释放树引用 | `inode.c` | ✅ |
| 1.10 | INODE_TYPE_FILE (0) / DIR (1) / CHRDEV (2) 枚举 | `inode.h` | ✅ |
| 1.11 | INODE_NAME_LEN=32, VFS_MAX_FDS=8, MAX_INODES=32 常量 | `inode.h` | ✅ |

---

## 二、操作表 (union ops_u + 独立 dir_ops)

| # | 功能 | 文件 | 状态 |
|---|------|------|:----:|
| 2.1 | `file_ops_t` — open/close/read/write (FILE 类型, 在 union ops_u 中) | `inode.h` | ✅ |
| 2.2 | `cdev_ops_t` — open/close/read/write/ioctl (CHRDEV 类型, 在 union ops_u 中) | `inode.h` | ✅ |
| 2.3 | `dir_ops_t` — lookup/create/unlink/readdir (DIR 类型, 独立字段不在 union 中) | `inode.h` | ✅ |
| 2.4 | `dev_ops_t` — 驱动操作表 (open/close/read/write/ioctl, 第一个参数 void *priv) | `inode.h` | ✅ |
| 2.5 | `cdev_shared_fops` — 所有字符设备共享, 通过 private_data→dev_ops_t 二级分发 | `devfs.c` | ✅ |
| 2.6 | cdev_open/close/read/write/ioctl — 从 inode→private_data 取 dev_ops_t*, 调用对应方法 | `devfs.c` | ✅ |
| 2.7 | `ramfs_file_fops` — 内存文件操作表 (read/write 操作动态 buffer) | `ramfs.c` | ✅ |
| 2.8 | `shared_dir_ops` — 共享目录操作表 (lookup→inode_lookup_child, readdir→遍历 children) | `ramfs.c` | ✅ |
| 2.9 | 操作表绑定: DIR→dir_ops, CHRDEV→ops_u.cdev_ops, FILE→ops_u.file_ops | 各 .c | ✅ |

---

## 三、文件描述符系统

| # | 功能 | 文件 | 状态 |
|---|------|------|:----:|
| 3.1 | `fd_entry_t` — inode*, flags, offset, in_use (每 fd 独立 offset) | `kernel_types.h` | ✅ |
| 3.2 | `fd_table[VFS_MAX_FDS]` — 嵌入 TCB, 每任务独立 fd 空间 | `kernel_types.h` | ✅ |
| 3.3 | `fd_alloc(task, inode, flags)` → fd_index — 扫描 fd_table 找空闲槽 | `vfs.c` | ✅ |
| 3.4 | `fd_free(task, fd_index)` — inode_put + 清零 fd_entry, 槽位回收 | `vfs.c` | ✅ |
| 3.5 | fd 本质 = cap_id_t — cap_create((void*)(fd_index+1), CAP_OBJ_FILE, ...) 包装 | `vfs.c` | ✅ |
| 3.6 | cap_resolve(fd) → object ptr → fd_index = (uintptr_t)obj-1 → fd_table[index] | `vfs.c` | ✅ |
| 3.7 | 同一 inode 可被多个 fd 打开 (不同 offset, 独立 fd_entry, 共享 inode refcount) | `vfs.c` | ✅ |
| 3.8 | close(fd) 时 inode_put → refcount--, 若 refcount==0 自动 free inode | `vfs.c` | ✅ |
| 3.9 | fd_table memset 自动清零 (task_create 中 memset(tcb, 0, sizeof(tcb_t))) | `task.c` | ✅ |

---

## 四、VFS 核心

| # | 功能 | 文件 | 状态 |
|---|------|------|:----:|
| 4.1 | `vfs.h` — vfs_init/vfs_lookup/vfs_open/vfs_close/vfs_read/write/ioctl/lseek API | `vfs.h` | ✅ |
| 4.2 | `vfs_init()` — 创建 root `/` (DIR) + 创建 `/dev` + 创建 `/tmp`, 调 devfs_init + ramfs_init | `vfs.c` | ✅ |
| 4.3 | `vfs_mount(path, root_inode)` — 注册挂载点到 mount_table[] | `vfs.c` | ✅ |
| 4.4 | `vfs_lookup(path)` — 手动路径解析 (字符级 tokenize → inode 树遍历) | `vfs.c` | ✅ |
| 4.5 | vfs_lookup("/") → root inode | `vfs.c` | ✅ |
| 4.6 | vfs_lookup("/nonexistent") → NULL | `vfs.c` | ✅ |
| 4.7 | vfs_lookup 多层路径 ("/dev/null") | `vfs.c` | ✅ |
| 4.8 | `fd_alloc(task, inode, flags)` → fd_index (扫描 TCB fd_table, 设置 in_use) | `vfs.c` | ✅ |
| 4.9 | `fd_free(task, fd_index)` → 清除 fd_entry | `vfs.c` | ✅ |
| 4.10 | `vfs_open(path, flags)` — 完整调用链: lookup → fd_alloc → inode_put(lookup ref) → ops->open → cap_create | `vfs.c` | ✅ |
| 4.11 | `vfs_close(fd)` — cap_resolve → ops->close → fd_free → cap_delete | `vfs.c` | ✅ |
| 4.12 | `vfs_read(fd, buf, size)` — cap_resolve → switch(type) dispatch → 更新 offset → 返回字节数 | `vfs.c` | ✅ |
| 4.13 | `vfs_write(fd, buf, size)` — cap_resolve → switch(type) dispatch → 更新 offset → 返回字节数 | `vfs.c` | ✅ |
| 4.14 | `vfs_ioctl(fd, cmd, arg)` — cap_resolve → dispatch (仅 CHRDEV 支持) | `vfs.c` | ✅ |
| 4.15 | `vfs_lseek(fd, offset, whence)` — SEEK_SET/SEEK_CUR/SEEK_END | `vfs.c` | ✅ |
| 4.16 | 挂载表 mount_table[MOUNT_MAX] (MOUNT_MAX=4) | `vfs.c` | ✅ |

---

## 五、devfs — 设备文件系统 + 驱动注册

| # | 功能 | 文件 | 状态 |
|---|------|------|:----:|
| 5.1 | `devfs.h` — devfs_init/devfs_register_device API | `devfs.h` | ✅ |
| 5.2 | `cdev_shared_fops` — 字符设备通用操作表 (cdev_open/cdev_read/cdev_write/cdev_close/cdev_ioctl) | `devfs.c` | ✅ |
| 5.3 | cdev_read 通过 private_data → dev_ops->read 分发 | `devfs.c` | ✅ |
| 5.4 | cdev_write 通过 private_data → dev_ops->write 分发 | `devfs.c` | ✅ |
| 5.5 | `/dev/null` 设备 — null_read 返回 0, null_write 吃掉数据 | `devfs.c` | ✅ |
| 5.6 | `devfs_init(parent)` — 挂载 devfs, 注册 /dev/null | `devfs.c` | ✅ |
| 5.7 | `devfs_register_device(name, ops)` — 创建 CHRDEV inode 加入 /dev, private_data=ops | `devfs.c` | ✅ |

---

## 六、ramfs — 内存文件系统

| # | 功能 | 文件 | 状态 |
|---|------|------|:----:|
| 6.1 | `ramfs.h` — ramfs_init/ramfs_create_file/ramfs_create_dir API | `ramfs.h` | ✅ |
| 6.2 | `ramfs_data_t` — buffer + size + capacity 内部结构 (块增长 RAMFS_BLOCK_SIZE=64) | `ramfs.c` | ✅ |
| 6.3 | `ramfs_file_fops` — open/close/read/write 操作表 | `ramfs.c` | ✅ |
| 6.4 | ramfs_read — memcpy from buffer+offset, 限制到 size | `ramfs.c` | ✅ |
| 6.5 | ramfs_write — grow buffer + memcpy, 更新 size | `ramfs.c` | ✅ |
| 6.6 | `shared_dir_ops` — lookup→inode_lookup_child, create→inode_alloc+add_child, unlink→remove_child+inode_put, readdir→遍历 siblings | `ramfs.c` | ✅ |
| 6.7 | `ramfs_init(parent)` — 存储 /tmp 根 inode | `ramfs.c` | ✅ |
| 6.8 | `ramfs_create_file(dir, name)` — 创建 FILE inode + ramfs_data_t + 加入目录树 | `ramfs.c` | ✅ |
| 6.9 | `ramfs_create_dir(dir, name)` — 创建 DIR inode + 设置 dir_ops | `ramfs.c` | ✅ |

---

## 七、Syscall 集成

| # | 功能 | Syscall # | 状态 |
|---|------|:---------:|:----:|
| 7.1 | SYSCALL_OPEN — path, flags → fd (cap_id_t) | 32 | ✅ |
| 7.2 | SYSCALL_CLOSE — fd → err | 33 | ✅ |
| 7.3 | SYSCALL_READ — fd, buf, size → bytes_read | 34 | ✅ |
| 7.4 | SYSCALL_WRITE — fd, buf, size → bytes_written | 35 | ✅ |
| 7.5 | SYSCALL_IOCTL — fd, cmd, arg → err | 36 | ✅ |
| 7.6 | SYSCALL_LSEEK — fd, offset, whence → new_offset | 37 | ✅ |
| 7.7 | SYSCALL_TABLE_SIZE 32 → 40 | — | ✅ |
| 7.8 | `user_api.h` — open/close/read/write/ioctl/lseek 内联包装 | — | ✅ |
| 7.9 | VFS syscall handler 遵循 6-arg 签名 + CAP_ENABLE guard 模式 | — | ✅ |
| 7.10 | CAP_OBJ_FILE=8, CAP_OBJ_TYPE_MAX→9, CAP_MAX_COUNT=64 | `capability.h` | ✅ |

---

## 八、配置集成

| # | 功能 | 文件 | 状态 |
|---|------|------|:----:|
| 8.1 | Kconfig "VFS Configuration" menu (VFS_ENABLE, VFS_MAX_FDS=8, VFS_MAX_INODES=32) | `Kconfig` | ✅ |
| 8.2 | Kconfig TEST_MODULE_VFS (depends TEST_ENABLE && VFS_ENABLE) | `Kconfig` | ✅ |
| 8.3 | Kconfig SYSCALL_TABLE_SIZE default 32 → 40 | `Kconfig` | ✅ |
| 8.4 | `.config` 启用 VFS_ENABLE=y, VFS_MAX_FDS=8, VFS_MAX_INODES=32 | `.config` | ✅ |
| 8.5 | `.config` 启用 TEST_MODULE_VFS=y, SYSCALL_TABLE_SIZE=40, CAP_MAX_COUNT=64 | `.config` | ✅ |

---

## 九、内核集成

| # | 功能 | 文件 | 状态 |
|---|------|------|:----:|
| 9.1 | kernel_types.h — 新增 KERN_ERR_PERM(-12) / KERN_ERR_NOTDIR(-13) / KERN_ERR_ISDIR(-14) | `kernel_types.h` | ✅ |
| 9.2 | kernel_types.h — TCB 新增 fd_entry_t fd_table[VFS_MAX_FDS] + struct inode 前向声明 (guarded VFS_ENABLE) | `kernel_types.h` | ✅ |
| 9.3 | kernel.c — #include "vfs/vfs.h", kern_init() 调用 vfs_init() | `kernel.c` | ✅ |
| 9.4 | kernel.h — #include "vfs/vfs.h" | `kernel.h` | ✅ |
| 9.5 | Makefile — KERN_SOURCES += vfs/inode.c vfs/vfs.c vfs/devfs.c vfs/ramfs.c | `Makefile` | ✅ |
| 9.6 | Makefile — CFLAGS += -Isrc/kernel/vfs | `Makefile` | ✅ |
| 9.7 | Makefile — TEST_SOURCES += src/tests/test_vfs.c | `Makefile` | ✅ |

---

## 十、测试 — inode 系统

| # | 测试 | 状态 |
|:---:|------|:----:|
| 10.1 | inode_alloc → 返回有效指针, type 正确, refcount=1 | ✅ |
| 10.2 | inode_alloc 池满 → 返回 NULL | ✅ |
| 10.3 | inode_free → 槽位回收, 可重新分配 | ✅ |
| 10.4 | inode_get → refcount++ | ✅ |
| 10.5 | inode_put → refcount--, refcount=0 时自动 free | ✅ |
| 10.6 | inode_add_child → parent→children 链表包含 child, tree 持有 ref | ✅ |
| 10.7 | inode_lookup_child → 按名查找返回正确 inode | ✅ |
| 10.8 | inode_lookup_child 不存在 → NULL | ✅ |
| 10.9 | inode_remove_child → 摘除后再 lookup 返回 NULL, tree ref 释放 | ✅ |
| 10.10 | 多个子节点 sibling 遍历计数正确 | ✅ |

---

## 十一、测试 — VFS 核心 + 文件描述符

| # | 测试 | 状态 |
|:---:|------|:----:|
| 11.1 | vfs_init() 后 root "/" inode 存在且类型为 DIR | ✅ |
| 11.2 | vfs_init() 后 "/dev" inode 存在 | ✅ |
| 11.3 | vfs_init() 后 "/tmp" inode 存在 | ✅ |
| 11.4 | vfs_lookup("/") → root inode | ✅ |
| 11.5 | vfs_lookup("/dev") → devfs root inode | ✅ |
| 11.6 | vfs_lookup("/dev/null") → null CHRDEV inode | ✅ |
| 11.7 | vfs_lookup("/nonexistent") → NULL | ✅ |
| 11.8 | vfs_lookup("/dev/nonexistent") → NULL | ✅ |
| 11.9 | vfs_lookup("") → NULL | ✅ |
| 11.10 | fd_alloc → 返回有效 fd_index, fd_table 对应槽 in_use=1 | ✅ |
| 11.11 | fd_alloc 耗尽 → 返回 -1 | ✅ |
| 11.12 | fd_free → 槽位回收, in_use=0, 可重新分配 | ✅ |
| 11.13 | vfs_open+close → fd 生命周期完整 | ✅ |

---

## 十二、测试 — devfs + 驱动注册

| # | 测试 | 状态 |
|:---:|------|:----:|
| 12.1 | vfs_open("/dev/null", O_RDWR) → 返回有效 cap_id (fd) | ✅ |
| 12.2 | vfs_write(null_fd, "hello", 5) → 返回 5 (成功吃掉数据) | ✅ |
| 12.3 | vfs_read(null_fd, buf, 10) → 返回 0 (EOF) | ✅ |
| 12.4 | vfs_close(null_fd) → KERN_OK | ✅ |
| 12.5 | vfs_close 后再 read → cap 已删除 | ✅ |
| 12.6 | devfs_register_device("testdev", &test_drv_ops) → KERN_OK | ✅ |
| 12.7 | devfs_register 后 vfs_lookup("/dev/testdev") → CHRDEV inode 存在 | ✅ |
| 12.8 | 重复注册同名设备 → 拒绝 | ✅ |

---

## 十三、测试 — ramfs

| # | 测试 | 状态 |
|:---:|------|:----:|
| 13.1 | ramfs_create_file("/tmp/testfile") → 有效 inode | ✅ |
| 13.2 | vfs_open("/tmp/testfile", O_RDWR) → 有效 fd | ✅ |
| 13.3 | vfs_write → vfs_lseek(fd, 0, SEEK_SET) → vfs_read → 数据一致 | ✅ |
| 13.4 | vfs_lseek SEEK_CUR → offset 正确累加 | ✅ |
| 13.5 | vfs_lseek SEEK_END → offset 等于 size | ✅ |
| 13.6 | 多次 append write 后 seek+read 验证追加写入 | ✅ |
| 13.7 | close 后 capability guard 生效 (read after close → fail) | ✅ |

---

## 十四、回归测试 — 硬件验证

| # | 测试集 | 状态 |
|:---:|--------|:----:|
| 14.1 | Phase 1+2 全部回归 tests 仍通过 | ✅ |
| 14.2 | VFS 测试模块全部通过 (test_vfs) | ✅ |
| 14.3 | 编译零警告 (-Wall -Wextra -Werror) | ✅ |

---

## 完成统计

| 类别 | 总数 | 已完成 | 完成率 |
|------|------|--------|--------|
| inode 系统 | 11 | 11 | 100% |
| 操作表 (union ops_u + dir_ops) | 9 | 9 | 100% |
| 文件描述符系统 | 9 | 9 | 100% |
| VFS 核心 | 16 | 16 | 100% |
| devfs + 驱动注册 | 7 | 7 | 100% |
| ramfs | 9 | 9 | 100% |
| Syscall 集成 | 10 | 10 | 100% |
| 配置集成 | 5 | 5 | 100% |
| 内核集成 | 7 | 7 | 100% |
| 测试 — inode | 10 | 10 | 100% |
| 测试 — VFS 核心 + fd | 13 | 13 | 100% |
| 测试 — devfs + 驱动注册 | 8 | 8 | 100% |
| 测试 — ramfs | 7 | 7 | 100% |
| 测试 — 回归 | 3 | 3 | 100% |
| **总计** | **124** | **124** | **100%** |

---

> 创建日期: 2026-05-07
> 最后更新: 2026-05-07
> 状态: **全部完成** — 313 tests, 0 failures on STM32F767 Nucleo
>
> **设计变更记录 (vs 原计划):**
> - `file_operations_t` / `inode_operations_t` → `file_ops_t` / `cdev_ops_t` / `dir_ops_t`
> - 采用 `union ops_u { file_ops_t *file_ops; cdev_ops_t *cdev_ops; }` + 独立 `dir_ops_t *dir_ops`
> - inode_add_child 持有 refcount (tree reference), inode_remove_child 释放 refcount
> - inode_free 直接 unlink 避免 refcount 循环; ramfs dir_unlink 使用 inode_put 替代 inode_free
> - CAP_MAX_COUNT 32 → 64 (为 VFS fd 和其他模块留足空间)
> - vfs_open 在 fd_alloc 后立即 inode_put 释放 vfs_lookup 的查找引用
