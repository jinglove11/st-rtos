/**
 * @file test_fs_store.c
 * @brief Phase B — fs_store (用户态 FS 存储) 内核态单元测试
 *
 * 直接测试 fs_store 的 inode 池 + ramfs 逻辑,不经过 IPC。
 * 排查 fs_server 服务化时 fs_store 本身是否正确。
 * 测试在内核态跑 (特权),用 kmalloc 模拟 memblock。
 */

#include "test_framework.h"
#include "kernel.h"
#include "mem.h"
#include "fs_store.h"

#if TEST_MODULE_FS_STORE && VFS_ENABLE && CAP_ENABLE

/* 用内核 kmalloc 模拟 sys_mem_alloc 的 memblock (内核态测试) */
static uint8_t *g_test_store = NULL;

static void test_fs_store_basic(void) {
    test_section("Test 1: fs_store init + basic ramfs R/W");

    /* 分配 4KB 作为 store (模拟 memblock) */
    g_test_store = (uint8_t *)kmalloc(4096);
    TEST_ASSERT(g_test_store != NULL, "store buffer allocated");
    if (g_test_store == NULL) return;

    fs_store_ctx_t *ctx = fs_store_init(g_test_store, 4096);
    TEST_ASSERT(ctx != NULL, "fs_store_init OK");
    TEST_ASSERT(ctx->root != NULL, "root inode created");
    TEST_ASSERT(ctx->tmp_dir != NULL, "/tmp created");
    TEST_ASSERT(ctx->dev_dir != NULL, "/dev created");
    if (ctx == NULL) return;

    /* stat / */
    fs_statinfo_t st;
    int err = fs_store_stat(ctx, "/", &st);
    TEST_ASSERT_EQ((int)KERN_OK, err, "stat / OK");
    TEST_ASSERT_EQ((int)FS_INODE_DIR, (int)st.type, "/ is DIR");

    /* mkdir /tmp/sub */
    err = fs_store_mkdir(ctx, "/tmp/sub");
    TEST_ASSERT_EQ((int)KERN_OK, err, "mkdir /tmp/sub OK");

    /* open /tmp/testfile (O_CREAT) */
    int fd = fs_store_open(ctx, "/tmp/testfile", FS_O_RDWR | FS_O_CREAT | FS_O_TRUNC);
    test_print_num("[fs_store] open fd = ", (int32_t)fd);
    TEST_ASSERT(fd > 0, "open /tmp/testfile returns valid fd");
    if (fd <= 0) return;

    /* write "hello" (5 字节) */
    const char data[] = "hello";
    int32_t n = fs_store_write(ctx, fd, data, 5);
    TEST_ASSERT_EQ(5, (int)n, "write 5 bytes");

    /* lseek 0 */
    int pos = fs_store_lseek(ctx, fd, 0, FS_SEEK_SET);
    TEST_ASSERT_EQ(0, pos, "lseek to 0");

    /* read back */
    char buf[8];
    for (int i = 0; i < 8; i++) buf[i] = 0;
    n = fs_store_read(ctx, fd, buf, 5);
    TEST_ASSERT_EQ(5, (int)n, "read 5 bytes");
    TEST_ASSERT_EQ(0, (int)buf[0] - (int)'h', "buf[0]='h'");
    TEST_ASSERT_EQ(0, (int)buf[4] - (int)'o', "buf[4]='o'");

    /* close */
    err = fs_store_close(ctx, fd);
    TEST_ASSERT_EQ((int)KERN_OK, err, "close OK");

    /* stat /tmp/testfile */
    err = fs_store_stat(ctx, "/tmp/testfile", &st);
    TEST_ASSERT_EQ((int)KERN_OK, err, "stat /tmp/testfile OK");
    TEST_ASSERT_EQ((int)FS_INODE_FILE, (int)st.type, "type FILE");
    TEST_ASSERT_EQ(5, (int)st.size, "size 5");

    /* readdir /tmp:目录可以 open (用于 readdir) */
    fd = fs_store_open(ctx, "/tmp", FS_O_RDONLY);
    TEST_ASSERT(fd > 0, "open directory for readdir");
    if (fd > 0) {
        fs_dirent_t entry;
        /* readdir index 0 = "." */
        int rerr = fs_store_readdir(ctx, fd, &entry);
        TEST_ASSERT_EQ((int)KERN_OK, rerr, "readdir /tmp [0]='.' OK");
        TEST_ASSERT_EQ('.', (int)entry.name[0], "readdir returns '.'");
        fs_store_close(ctx, fd);
    }

    /* unlink */
    err = fs_store_unlink(ctx, "/tmp/testfile");
    TEST_ASSERT_EQ((int)KERN_OK, err, "unlink OK");

    err = fs_store_stat(ctx, "/tmp/testfile", &st);
    TEST_ASSERT(err != KERN_OK, "stat unlinked file fails");

    kfree(g_test_store);
    g_test_store = NULL;
}

static void test_fs_store_readdir(void) {
    test_section("Test 2: fs_store readdir");

    g_test_store = (uint8_t *)kmalloc(4096);
    TEST_ASSERT(g_test_store != NULL, "store buffer allocated");
    if (g_test_store == NULL) return;

    fs_store_ctx_t *ctx = fs_store_init(g_test_store, 4096);
    TEST_ASSERT(ctx != NULL, "fs_store_init OK");
    if (ctx == NULL) return;

    /* /tmp 下创建几个文件 */
    TEST_ASSERT_EQ((int)KERN_OK, (int)fs_store_mkdir(ctx, "/tmp/d1"), "mkdir d1");
    int fd = fs_store_open(ctx, "/tmp/f1", FS_O_CREAT | FS_O_WRONLY);
    TEST_ASSERT(fd > 0, "create f1");
    if (fd > 0) fs_store_close(ctx, fd);

    /* readdir /tmp 需要先 open 目录。
     * 临时:让 fs_store_open 对目录返回特殊 fd。
     * 当前 fs_store_open 对目录返回 -14。这是需要修复的设计点。
     * 先测 stat /tmp/d1 确认树结构。 */
    fs_statinfo_t st;
    TEST_ASSERT_EQ((int)KERN_OK, (int)fs_store_stat(ctx, "/tmp/d1", &st), "stat /tmp/d1");
    TEST_ASSERT_EQ((int)FS_INODE_DIR, (int)st.type, "d1 is DIR");

    kfree(g_test_store);
    g_test_store = NULL;
}

static void test_fs_store_module(void) {
    test_fs_store_basic();
    test_fs_store_readdir();
}

TEST_MODULE_REGISTER(fs_store, test_fs_store_module);

#endif /* TEST_MODULE_FS_STORE && VFS_ENABLE && CAP_ENABLE */
