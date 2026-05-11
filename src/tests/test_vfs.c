/**
 * @file test_vfs.c
 * @brief VFS 虚拟文件系统测试 — inode、fd、devfs、ramfs、syscall
 */

#include "test_framework.h"
#include "kernel.h"
#include "vfs/devfs.h"
#include "vfs/ramfs.h"
#include "kernel_config.h"
#include "user_api.h"
#if DRIVER_ENABLE
#include "device.h"
#endif
#include <string.h>

#if VFS_ENABLE && TEST_MODULE_VFS

/*============================================================================
 * Test 1: inode alloc/free 生命周期 + 池耗尽
 *============================================================================*/

static void test_inode_alloc_free(void) {
    test_section("Test 1: inode alloc/free lifecycle");

    inode_t *ino = inode_alloc(INODE_TYPE_FILE, "test");
    TEST_ASSERT_NOT_NULL(ino, "inode_alloc returns valid pointer");
    TEST_ASSERT_EQ((int)ino->type, (int)INODE_TYPE_FILE, "inode type FILE");
    TEST_ASSERT_EQ(1u, ino->refcount, "inode refcount=1");

    uint32_t ino_num = ino->ino;
    inode_free(ino);

    /* re-allocate and verify it's recycled */
    inode_t *ino2 = inode_alloc(INODE_TYPE_DIR, "test2");
    TEST_ASSERT_NOT_NULL(ino2, "inode re-alloc after free");
    inode_free(ino2);
    (void)ino_num;
}

static void test_inode_pool_full(void) {
    test_section("Test 1b: inode pool exhaustion");

    /* Some inodes already used by vfs_init (/, /dev, /tmp, /dev/null). */
    /* Fill remaining slots, then verify overflow. */
    inode_t *inos[MAX_INODES];
    int count = 0;
    for (int i = 0; i < MAX_INODES; i++) {
        inos[i] = inode_alloc(INODE_TYPE_FILE, "x");
        if (inos[i]) count++;
        else break;
    }
    TEST_ASSERT(count > 0, "at least some inodes allocatable");

    inode_t *over = inode_alloc(INODE_TYPE_FILE, "overflow");
    TEST_ASSERT_NULL(over, "inode_alloc returns NULL when pool full");

    for (int i = 0; i < count; i++) inode_free(inos[i]);
}

/*============================================================================
 * Test 2: inode refcounting
 *============================================================================*/

static void test_inode_refcount(void) {
    test_section("Test 2: inode refcounting");

    inode_t *ino = inode_alloc(INODE_TYPE_FILE, "ref");
    TEST_ASSERT_EQ(1u, ino->refcount, "refcount starts at 1");

    inode_get(ino);
    TEST_ASSERT_EQ(2u, ino->refcount, "inode_get → refcount=2");

    inode_put(ino);
    TEST_ASSERT_EQ(1u, ino->refcount, "inode_put → refcount=1");

    uint32_t ino_num = ino->ino;
    inode_put(ino);  /* refcount=0 → auto-free */

    inode_t *ino2 = inode_alloc(INODE_TYPE_FILE, "reuse");
    TEST_ASSERT_NOT_NULL(ino2, "slot recycled after refcount hit 0");
    TEST_ASSERT(ino2->ino > 0, "recycled inode gets valid ino");
    (void)ino_num;
    inode_free(ino2);
}

/*============================================================================
 * Test 3: inode tree ops
 *============================================================================*/

static void test_inode_tree(void) {
    test_section("Test 3: inode tree operations");

    inode_t *root = inode_alloc(INODE_TYPE_DIR, "/");
    inode_t *child1 = inode_alloc(INODE_TYPE_FILE, "file1");
    inode_t *child2 = inode_alloc(INODE_TYPE_FILE, "file2");

    inode_add_child(root, child1);
    inode_add_child(root, child2);

    inode_t *found = inode_lookup_child(root, "file1");
    TEST_ASSERT_EQ((uintptr_t)child1, (uintptr_t)found, "lookup_child finds file1");

    found = inode_lookup_child(root, "file2");
    TEST_ASSERT_EQ((uintptr_t)child2, (uintptr_t)found, "lookup_child finds file2");

    found = inode_lookup_child(root, "missing");
    TEST_ASSERT_NULL(found, "lookup_child returns NULL for missing");

    /* sibling traversal count */
    int sib_count = 0;
    inode_t *sib = root->children;
    while (sib) { sib_count++; sib = sib->next_sibling; }
    TEST_ASSERT_EQ(2, sib_count, "two children in sibling list");

    /* remove child */
    kern_err_t err = inode_remove_child(root, "file1");
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "remove_child file1 OK");
    TEST_ASSERT_NULL(inode_lookup_child(root, "file1"), "file1 gone after remove");

    /* remove non-existent */
    err = inode_remove_child(root, "missing");
    TEST_ASSERT_EQ((int)KERN_ERR_NOEXIST, (int)err, "remove missing → NOEXIST");

    inode_free(child1);
    inode_free(child2);
    inode_free(root);
}

/*============================================================================
 * Test 4: VFS init — root & /dev & /tmp
 *============================================================================*/

static void test_inode_pool(void) {
    /* VFS init tests depend on the global state from vfs_init() */
    test_section("Test 4: VFS init — root /dev /tmp");

    inode_t *root = vfs_lookup("/");
    TEST_ASSERT_NOT_NULL(root, "root / exists");
    if (root) {
        TEST_ASSERT_EQ((int)INODE_TYPE_DIR, (int)root->type, "root is DIR");
        inode_put(root);
    }

    inode_t *dev = vfs_lookup("/dev");
    TEST_ASSERT_NOT_NULL(dev, "/dev exists");
    if (dev) inode_put(dev);

    inode_t *tmp = vfs_lookup("/tmp");
    TEST_ASSERT_NOT_NULL(tmp, "/tmp exists");
    if (tmp) inode_put(tmp);
}

/*============================================================================
 * Test 5: vfs_lookup path resolution
 *============================================================================*/

static void test_vfs_lookup_paths(void) {
    test_section("Test 5: vfs_lookup path resolution");

    /* deep path */
    inode_t *null_dev = vfs_lookup("/dev/null");
    TEST_ASSERT_NOT_NULL(null_dev, "/dev/null exists");
    if (null_dev) {
        TEST_ASSERT_EQ((int)INODE_TYPE_CHRDEV, (int)null_dev->type,
                       "/dev/null is CHRDEV");
        inode_put(null_dev);
    }

    /* non-existent paths */
    inode_t *bad = vfs_lookup("/nonexistent");
    TEST_ASSERT_NULL(bad, "/nonexistent → NULL");

    bad = vfs_lookup("/dev/nonexistent");
    TEST_ASSERT_NULL(bad, "/dev/nonexistent → NULL");

    bad = vfs_lookup("");
    TEST_ASSERT_NULL(bad, "empty path → NULL");
}

/*============================================================================
 * Test 6: fd table operations
 *============================================================================*/

static void test_fd_table_ops(void) {
    test_section("Test 6: fd table operations");

    tcb_t *task = sched_get_current();
    TEST_ASSERT_NOT_NULL(task, "current task exists");

    inode_t *ino = inode_alloc(INODE_TYPE_FILE, "fdtest");

    /* fd_alloc */
    int fd = fd_alloc(task, ino, O_RDWR);
    TEST_ASSERT(fd >= 0, "fd_alloc returns valid index");
    TEST_ASSERT(task->fd_table[fd].in_use == 1, "fd_entry in_use=1");
    TEST_ASSERT_EQ((uintptr_t)ino, (uintptr_t)task->fd_table[fd].inode,
                   "fd_entry points to correct inode");

    /* fd_free */
    fd_free(task, fd);
    TEST_ASSERT(task->fd_table[fd].in_use == 0, "fd_entry freed");

    /* fd_alloc exhaustion */
    int fds[VFS_MAX_FDS];
    inode_t *tmp_inos[VFS_MAX_FDS];
    int i;
    for (i = 0; i < VFS_MAX_FDS; i++) {
        tmp_inos[i] = inode_alloc(INODE_TYPE_FILE, "tmp");
        fds[i] = fd_alloc(task, tmp_inos[i], O_RDONLY);
        if (fds[i] < 0) break;
    }
    TEST_ASSERT_EQ(VFS_MAX_FDS, i, "fd table filled to VFS_MAX_FDS");
    int over = fd_alloc(task, ino, O_RDONLY);
    TEST_ASSERT_EQ(-1, over, "fd_alloc returns -1 when full");

    for (int j = 0; j < i; j++) {
        fd_free(task, fds[j]);
        inode_put(tmp_inos[j]);  /* release alloc ref, fd_free already put its ref */
    }
    inode_free(ino);
}

/*============================================================================
 * Test 7: /dev/null read/write
 *============================================================================*/

static void test_dev_null(void) {
    test_section("Test 7: /dev/null read/write");

    int fd = open("/dev/null", O_RDWR);
    TEST_ASSERT(fd >= 0, "open /dev/null returns valid fd");

    char buf[16];
    int32_t n = read(fd, buf, 10);
    TEST_ASSERT_EQ(0, (int)n, "/dev/null read returns 0 (EOF)");

    n = write(fd, "hello", 5);
    TEST_ASSERT_EQ(5, (int)n, "/dev/null write returns 5 (eats data)");

    kern_err_t err = close(fd);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "close /dev/null OK");

    /* close again → error (cap already deleted) */
    n = read(fd, buf, 10);
    TEST_ASSERT(n < 0, "read after close fails");
}

/*============================================================================
 * Test 8: devfs_register_device
 *============================================================================*/

static dev_ops_t test_drv_ops = { NULL, NULL, NULL, NULL, NULL };

static void test_devfs_register(void) {
    test_section("Test 8: devfs_register_device");

#if DRIVER_ENABLE
    /* register via device_t */
    device_t *tdev = device_alloc("testdev", DEVICE_TYPE_CHAR);
    TEST_ASSERT_NOT_NULL(tdev, "device_alloc testdev");
    tdev->ops = &test_drv_ops;

    kern_err_t err = devfs_register_device("testdev", tdev);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "devfs_register_device OK");
#else
    /* register via dev_ops_t */
    kern_err_t err = devfs_register_device("testdev", &test_drv_ops);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "devfs_register_device OK");
#endif

    /* verify it exists in /dev */
    inode_t *dev = vfs_lookup("/dev/testdev");
    TEST_ASSERT_NOT_NULL(dev, "/dev/testdev exists after register");
    if (dev) {
        TEST_ASSERT_EQ((int)INODE_TYPE_CHRDEV, (int)dev->type,
                       "registered device is CHRDEV");
        inode_put(dev);
    }

    /* duplicate registration rejected */
#if DRIVER_ENABLE
    err = devfs_register_device("testdev", tdev);
#else
    err = devfs_register_device("testdev", &test_drv_ops);
#endif
    TEST_ASSERT(err != (int)KERN_OK, "duplicate register rejected");
}

/*============================================================================
 * Test 9: ramfs create/read/write/lseek
 *============================================================================*/

static void test_ramfs_basic(void) {
    test_section("Test 9: ramfs create/read/write/lseek");

    /* create a file in /tmp (vfs_lookup ref must be released) */
    inode_t *tmp_dir = vfs_lookup("/tmp");
    inode_t *file = ramfs_create_file(tmp_dir, "testfile");
    inode_put(tmp_dir);
    TEST_ASSERT_NOT_NULL(file, "ramfs_create_file OK");
    if (!file) return;

    /* open the file */
    int fd = open("/tmp/testfile", O_RDWR);
    TEST_ASSERT(fd >= 0, "open /tmp/testfile returns valid fd");

    /* write data */
    const char *data = "Hello VFS!";
    int32_t n = write(fd, data, 10);
    TEST_ASSERT_EQ(10, (int)n, "write returns 10 bytes");

    /* lseek to beginning */
    int32_t pos = lseek(fd, 0, SEEK_SET);
    TEST_ASSERT_EQ(0, (int)pos, "lseek SEEK_SET → 0");

    /* read back */
    char buf[32];
    memset(buf, 0, sizeof(buf));
    n = read(fd, buf, 10);
    TEST_ASSERT_EQ(10, (int)n, "read returns 10 bytes");
    TEST_ASSERT_EQ(0, memcmp("Hello VFS!", buf, 10), "data matches");

    /* lseek SEEK_CUR */
    pos = lseek(fd, 5, SEEK_CUR);
    TEST_ASSERT_EQ(15, (int)pos, "lseek SEEK_CUR +5 → 15");

    /* lseek SEEK_END */
    pos = lseek(fd, 0, SEEK_END);
    TEST_ASSERT_EQ(10, (int)pos, "lseek SEEK_END → size (10)");

    n = close(fd);
    TEST_ASSERT_EQ((int)KERN_OK, (int)n, "close /tmp/testfile OK");

    inode_put(file);  /* release from ramfs_create_file */
}

/*============================================================================
 * Test 10: O_CREAT — create new file via open
 *============================================================================*/

static void test_vfs_ocreat(void) {
    test_section("Test 10: O_CREAT");
    uint32_t saved_ino = 0;

    /* non-existent file without O_CREAT → fail */
    int fd = open("/tmp/ocreat_new", O_RDWR);
    TEST_ASSERT(fd < 0, "open non-existent without O_CREAT fails");

    /* O_CREAT creates the file */
    fd = open("/tmp/ocreat_new", O_RDWR | O_CREAT);
    TEST_ASSERT(fd >= 0, "O_CREAT creates new file");

    if (fd >= 0) {
        int32_t n = write(fd, "CREAT", 5);
        TEST_ASSERT_EQ(5, (int)n, "write to O_CREAT file OK");

        lseek(fd, 0, SEEK_SET);
        char buf[8] = {0};
        n = read(fd, buf, 5);
        TEST_ASSERT_EQ(5, (int)n, "read back OK");
        TEST_ASSERT_EQ(0, memcmp("CREAT", buf, 5), "data matches after O_CREAT");

        inode_t *ino = vfs_lookup("/tmp/ocreat_new");
        saved_ino = ino ? ino->ino : 0;
        if (ino) inode_put(ino);

        close(fd);
        TEST_ASSERT(saved_ino > 0, "O_CREAT file has valid inode");
    }

    /* O_CREAT on existing file → returns existing */
    fd = open("/tmp/ocreat_new", O_RDWR | O_CREAT);
    TEST_ASSERT(fd >= 0, "O_CREAT on existing returns existing");
    if (fd >= 0) {
        inode_t *ino = vfs_lookup("/tmp/ocreat_new");
        uint32_t ino2 = ino ? ino->ino : 0;
        if (ino) inode_put(ino);
        TEST_ASSERT_EQ((int)saved_ino, (int)ino2, "same inode (no duplicate)");
        close(fd);
    }

    /* clean up so Test 11 (append) starts fresh */
    inode_t *cleanup = vfs_lookup("/tmp/ocreat_new");
    if (cleanup) {
        inode_put(cleanup);
    }
}

/*============================================================================
 * Test 11: ramfs append + multi-write
 *============================================================================*/

static void test_ramfs_append(void) {
    test_section("Test 11: ramfs append write");

    int fd = open("/tmp/testfile", O_RDWR);
    if (fd < 0) {
        test_fail("open /tmp/testfile for append");
        return;
    }

    lseek(fd, 0, SEEK_END);   /* move past existing content */
    write(fd, "ABC", 3);
    write(fd, "DEF", 3);      /* consecutive writes append */

    char buf[16];
    memset(buf, 0, sizeof(buf));
    lseek(fd, 10, SEEK_SET);  /* skip original "Hello VFS!" */
    int32_t n = read(fd, buf, 6);
    TEST_ASSERT_EQ(6, (int)n, "read 6 bytes after append");
    TEST_ASSERT_EQ(0, memcmp("ABCDEF", buf, 6), "appended data matches");

    close(fd);
}

/*============================================================================
 * Test 11: Capability guard
 *============================================================================*/

static void test_vfs_cap_guard(void) {
    test_section("Test 12: capability guard");

    int fd = open("/dev/null", O_RDONLY);
    TEST_ASSERT(fd >= 0, "open /dev/null for cap test");

    /* can read (CAP_READ allowed) */
    char buf[8];
    int32_t n = read(fd, buf, 4);
    TEST_ASSERT_EQ(0, (int)n, "read allowed with READ-cap fd");

    close(fd);

    /* use after close → fails */
    n = read(fd, buf, 4);
    TEST_ASSERT(n < 0, "read after close fails");
}

/*============================================================================
 * Test 13: "." / ".." directory entries
 *============================================================================*/

static void test_dot_entries(void) {
    test_section("Test 13: dot entries (. and ..)");

    /* lookup "." on /tmp */
    inode_t *tmp = vfs_lookup("/tmp");
    TEST_ASSERT_NOT_NULL(tmp, "/tmp exists for dot test");
    if (!tmp) return;

    inode_t *dot = NULL;
    kern_err_t err = tmp->dir_ops->lookup(tmp, ".", &dot);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "lookup '.' OK");
    TEST_ASSERT_EQ((uintptr_t)tmp, (uintptr_t)dot, "'.' points to self");
    if (dot) inode_put(dot);

    /* lookup ".." on /tmp → should be root */
    inode_t *dd = NULL;
    err = tmp->dir_ops->lookup(tmp, "..", &dd);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "lookup '..' OK");
    TEST_ASSERT(dd == tmp->parent || dd == tmp, "'..' is parent or self");
    if (dd) inode_put(dd);

    /* readdir index 0 = ".", index 1 = ".." */
    dirent_t entry;
    err = tmp->dir_ops->readdir(tmp, 0, &entry);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "readdir[0] OK");
    TEST_ASSERT_EQ(0, strcmp(entry.name, "."), "readdir[0] is '.'");

    err = tmp->dir_ops->readdir(tmp, 1, &entry);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "readdir[1] OK");
    TEST_ASSERT_EQ(0, strcmp(entry.name, ".."), "readdir[1] is '..'");

    /* unlink "." → rejected */
    err = tmp->dir_ops->unlink(tmp, ".");
    TEST_ASSERT(err != KERN_OK, "unlink '.' rejected");

    /* unlink ".." → rejected */
    err = tmp->dir_ops->unlink(tmp, "..");
    TEST_ASSERT(err != KERN_OK, "unlink '..' rejected");

    inode_put(tmp);
}

/*============================================================================
 * Test 14: O_TRUNC
 *============================================================================*/

static void test_o_trunc(void) {
    test_section("Test 14: O_TRUNC");

    /* create a file and write data */
    int fd = open("/tmp/trunc_test", O_RDWR | O_CREAT);
    TEST_ASSERT(fd >= 0, "create trunc_test");
    if (fd < 0) return;

    write(fd, "ABCDEFGHIJ", 10);
    close(fd);

    /* reopen with O_TRUNC */
    fd = open("/tmp/trunc_test", O_RDWR | O_TRUNC);
    TEST_ASSERT(fd >= 0, "reopen with O_TRUNC");
    if (fd < 0) return;

    /* file should be empty now */
    char buf[16] = {0};
    int32_t n = read(fd, buf, 10);
    TEST_ASSERT_EQ(0, (int)n, "read after O_TRUNC returns 0 (empty)");

    /* write new data */
    write(fd, "XY", 2);
    lseek(fd, 0, SEEK_SET);
    memset(buf, 0, sizeof(buf));
    n = read(fd, buf, 2);
    TEST_ASSERT_EQ(2, (int)n, "read 2 bytes after O_TRUNC+write");
    TEST_ASSERT_EQ(0, memcmp("XY", buf, 2), "new data matches");

    close(fd);
}

/*============================================================================
 * Test 15: lseek boundary (negative offset rejection)
 *============================================================================*/

static void test_lseek_boundary(void) {
    test_section("Test 15: lseek boundary");

    int fd = open("/tmp/trunc_test", O_RDWR);
    TEST_ASSERT(fd >= 0, "open for lseek test");
    if (fd < 0) return;

    /* SEEK_SET with negative → error */
    int32_t pos = lseek(fd, -1, SEEK_SET);
    TEST_ASSERT(pos < 0, "SEEK_SET -1 rejected");

    /* write some data so offset > 0 */
    write(fd, "HELLO", 5);

    /* SEEK_CUR to before start → error */
    pos = lseek(fd, -100, SEEK_CUR);
    TEST_ASSERT(pos < 0, "SEEK_CUR -100 rejected");

    /* SEEK_CUR valid */
    lseek(fd, 0, SEEK_SET);
    pos = lseek(fd, 3, SEEK_CUR);
    TEST_ASSERT_EQ(3, (int)pos, "SEEK_CUR +3 → 3");

    /* SEEK_END to before start → error */
    pos = lseek(fd, -100, SEEK_END);
    TEST_ASSERT(pos < 0, "SEEK_END -100 rejected");

    /* SEEK_END valid */
    pos = lseek(fd, -2, SEEK_END);
    TEST_ASSERT(pos >= 0, "SEEK_END -2 valid");
    TEST_ASSERT_EQ(3, (int)pos, "SEEK_END -2 on 5-byte file → 3");

    close(fd);
}

/*============================================================================
 * Module registration
 *============================================================================*/

static void test_vfs_module(void) {
    test_inode_alloc_free();
    test_inode_pool_full();
    test_inode_refcount();
    test_inode_tree();
    test_inode_pool();          /* VFS init — depends on vfs_init() called */
    test_vfs_lookup_paths();
    test_fd_table_ops();
    test_dev_null();
    test_devfs_register();
    test_ramfs_basic();
    test_vfs_ocreat();
    test_ramfs_append();
    test_vfs_cap_guard();
    test_dot_entries();
    test_o_trunc();
    test_lseek_boundary();
}

TEST_MODULE_REGISTER(vfs, test_vfs_module);

#endif /* VFS_ENABLE && TEST_MODULE_VFS */
