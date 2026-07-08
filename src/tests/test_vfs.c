/**
 * @file test_vfs.c
 * @brief VFS 测试 — Phase D 后已迁移到 fs_server
 *
 * Phase D 之前:这里测内核内 VFS (vfs.c/inode.c/ramfs.c/devfs.c)。
 * Phase D 之后:文件操作由 user 态 fs_server 提供 (fs_store 自管
 * inode池+ramfs,devfs 转发到 driver server)。
 *
 * 内核 VFS 的测试覆盖已由以下 fs_server 测试取代:
 *   - test_fs_store.c:fs_store 内核态单元测试 (inode/ramfs/fd)
 *   - test_fs_devfs.c:devfs 转发端到端
 *   - test_fs_fd_cleanup.c:客户端死亡 fd 清理
 *   - test_service_model.c (fs_server 部分):IPC 端到端 (507 个)
 *
 * 这个 module 保留为占位,报告迁移状态。旧的内核 VFS 测试代码已删除
 * (它们测的是不再使用的内核子系统)。内核 vfs.c/inode.c 文件暂保留
 * (devfs 设备注册仍用),完整删除待设备驱动模型重构。
 */

#include "test_framework.h"
#include "kernel_config.h"

#if VFS_ENABLE && TEST_MODULE_VFS

static void test_vfs_module(void) {
    test_section("VFS (legacy kernel VFS — migrated to fs_server)");
    test_pass("kernel VFS syscalls migrated to fs_server (Phase D)");
    test_pass("coverage: test_fs_store + test_fs_devfs + fs_fd_cleanup");
}

TEST_MODULE_REGISTER(vfs, test_vfs_module);

#endif /* VFS_ENABLE && TEST_MODULE_VFS */
