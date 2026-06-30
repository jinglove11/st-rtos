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

static kern_err_t test_tmp_unlink(const char *name) {
    inode_t *tmp = vfs_lookup("/tmp");
    if (!tmp) return KERN_ERR_NOEXIST;

    kern_err_t err = KERN_ERR_NOEXIST;
    if (tmp->dir_ops && tmp->dir_ops->unlink) {
        err = tmp->dir_ops->unlink(tmp, name);
    }
    inode_put(tmp);
    return err;
}

static kern_err_t test_dir_unlink_child(const char *dir_path, const char *name) {
    inode_t *dir = vfs_lookup(dir_path);
    if (!dir) return KERN_ERR_NOEXIST;

    kern_err_t err = KERN_ERR_NOEXIST;
    if (dir->dir_ops && dir->dir_ops->unlink) {
        err = dir->dir_ops->unlink(dir, name);
    }
    inode_put(dir);
    return err;
}

static void vfs_fd_dummy_task(void *arg) {
    (void)arg;
}

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
 * Test 5b: root/dev directory readdir + normalized lookup
 *============================================================================*/

static void test_vfs_root_readdir_and_normalize(void) {
    test_section("Test 5b: root/dev readdir and path normalization");

    inode_t *root = vfs_lookup("/");
    TEST_ASSERT_NOT_NULL(root, "root lookup OK");
    TEST_ASSERT(root && root->dir_ops && root->dir_ops->readdir,
                "root readdir supported");

    int saw_dev = 0;
    int saw_tmp = 0;
    if (root && root->dir_ops && root->dir_ops->readdir) {
        dirent_t entry;
        for (uint32_t i = 0; i < 8; i++) {
            if (root->dir_ops->readdir(root, i, &entry) != KERN_OK) {
                break;
            }
            if (strcmp(entry.name, "dev") == 0) saw_dev = 1;
            if (strcmp(entry.name, "tmp") == 0) saw_tmp = 1;
        }
    }
    TEST_ASSERT(saw_dev == 1, "root readdir sees dev");
    TEST_ASSERT(saw_tmp == 1, "root readdir sees tmp");
    if (root) inode_put(root);

    inode_t *dev = vfs_lookup("/dev");
    TEST_ASSERT_NOT_NULL(dev, "/dev lookup OK");
    TEST_ASSERT(dev && dev->dir_ops && dev->dir_ops->readdir,
                "/dev readdir supported");

    int saw_null = 0;
    if (dev && dev->dir_ops && dev->dir_ops->readdir) {
        dirent_t entry;
        for (uint32_t i = 0; i < 8; i++) {
            if (dev->dir_ops->readdir(dev, i, &entry) != KERN_OK) {
                break;
            }
            if (strcmp(entry.name, "null") == 0) saw_null = 1;
        }
    }
    TEST_ASSERT(saw_null == 1, "/dev readdir sees null");
    if (dev) inode_put(dev);

    inode_t *null_dev = vfs_lookup("//tmp/../dev//./null");
    TEST_ASSERT_NOT_NULL(null_dev, "normalized path resolves /dev/null");
    if (null_dev) {
        TEST_ASSERT_EQ((int)INODE_TYPE_CHRDEV, (int)null_dev->type,
                       "normalized /dev/null is CHRDEV");
        inode_put(null_dev);
    }
}

/*============================================================================
 * Test 5c: fd-based directory iteration
 *============================================================================*/

static void test_vfs_fd_readdir(void) {
    test_section("Test 5c: fd-based readdir");

    int fd = vfs_open("/dev", O_RDONLY);
    TEST_ASSERT(fd >= 0, "open /dev directory");
    if (fd < 0) return;

    dirent_t entry;
    kern_err_t err = vfs_readdir(fd, &entry);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "readdir first entry OK");
    TEST_ASSERT_EQ(0, strcmp(entry.name, "."), "first entry is '.'");

    int saw_null = 0;
    for (int i = 0; i < 8; i++) {
        err = vfs_readdir(fd, &entry);
        if (err != KERN_OK) {
            break;
        }
        if (strcmp(entry.name, "null") == 0) {
            saw_null = 1;
        }
    }
    TEST_ASSERT(saw_null == 1, "fd readdir sees null");

    err = vfs_rewinddir(fd);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "rewinddir OK");
    err = vfs_readdir(fd, &entry);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "readdir after rewind OK");
    TEST_ASSERT_EQ(0, strcmp(entry.name, "."), "rewind returns to '.'");

    vfs_close(fd);
}

/*============================================================================
 * Test 5d: mount point lookup redirects to mounted root
 *============================================================================*/

static void test_vfs_mount_redirect(void) {
    test_section("Test 5d: mount redirect");

    inode_t *tmp = vfs_lookup("/tmp");
    TEST_ASSERT_NOT_NULL(tmp, "/tmp exists for mount test");
    if (!tmp) return;

    inode_t *src = ramfs_create_dir(tmp, "mnt_src");
    inode_t *dst = ramfs_create_dir(tmp, "mnt_dst");
    TEST_ASSERT_NOT_NULL(src, "mount source dir created");
    TEST_ASSERT_NOT_NULL(dst, "mount point dir created");

    inode_t *file = NULL;
    if (src) {
        file = ramfs_create_file(src, "inside");
    }
    TEST_ASSERT_NOT_NULL(file, "mounted file created");

    kern_err_t err = vfs_mount("//tmp/./mnt_dst", src);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "mount source on destination");

    inode_t *found = vfs_lookup("/tmp/mnt_dst/inside");
    TEST_ASSERT_NOT_NULL(found, "lookup through mount finds file");
    if (found && file) {
        TEST_ASSERT_EQ((uintptr_t)file, (uintptr_t)found,
                       "mounted lookup returns source child");
    }
    if (found) inode_put(found);

    err = vfs_mount("/tmp/mnt_dst", src);
    TEST_ASSERT_EQ((int)KERN_ERR_BUSY, (int)err, "duplicate mount rejected");

    int fd = vfs_open("/tmp/mnt_dst", O_RDONLY);
    TEST_ASSERT(fd >= 0, "open mounted root directory");
    if (fd >= 0) {
        err = vfs_unmount("/tmp/mnt_dst");
        TEST_ASSERT_EQ((int)KERN_ERR_BUSY, (int)err,
                       "unmount busy while mounted root open");
        vfs_close(fd);
    }

    fd = vfs_open("/tmp/mnt_dst/inside", O_RDONLY);
    TEST_ASSERT(fd >= 0, "open file below mounted root");
    if (fd >= 0) {
        err = vfs_unmount("/tmp/mnt_dst");
        TEST_ASSERT_EQ((int)KERN_ERR_BUSY, (int)err,
                       "unmount busy while mounted child open");
        vfs_close(fd);
    }

    err = vfs_unmount("//tmp/./mnt_dst");
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "unmount after close OK");

    found = vfs_lookup("/tmp/mnt_dst/inside");
    TEST_ASSERT_NULL(found, "lookup through unmounted path fails");
    if (found) inode_put(found);

    err = vfs_unmount("/tmp/mnt_dst");
    TEST_ASSERT_EQ((int)KERN_ERR_NOEXIST, (int)err,
                   "duplicate unmount rejected");

    if (file) inode_put(file);
    (void)test_dir_unlink_child("/tmp/mnt_src", "inside");
    if (src) inode_put(src);
    if (dst) inode_put(dst);
    (void)test_tmp_unlink("mnt_src");
    (void)test_tmp_unlink("mnt_dst");
    inode_put(tmp);
}

/*============================================================================
 * Test 6: fd table operations
 *============================================================================*/

static void test_fd_table_ops(void) {
    test_section("Test 6: fd table operations");

    task_id_t tid = task_create("vfsfd", vfs_fd_dummy_task, NULL, 10, 512);
    TEST_ASSERT(tid >= 0, "fd table test task created");

    tcb_t *task = task_get_tcb(tid);
    TEST_ASSERT_NOT_NULL(task, "fd table test task exists");

    inode_t *ino = inode_alloc(INODE_TYPE_FILE, "fdtest");
    TEST_ASSERT_NOT_NULL(ino, "fd test inode allocated");

    /* fd_alloc */
    int fd = fd_alloc(task, ino, O_RDWR);
    TEST_ASSERT(fd >= 0, "fd_alloc returns valid index");
    if (task != NULL && fd >= 0 && fd < VFS_MAX_FDS) {
        TEST_ASSERT(task->fd_table[fd].in_use == 1, "fd_entry in_use=1");
        TEST_ASSERT_EQ((uintptr_t)ino, (uintptr_t)task->fd_table[fd].inode,
                       "fd_entry points to correct inode");
    }

    /* fd_free */
    if (fd >= 0) {
        fd_free(task, fd);
    }
    if (task != NULL && fd >= 0 && fd < VFS_MAX_FDS) {
        TEST_ASSERT(task->fd_table[fd].in_use == 0, "fd_entry freed");
    }

    /* fd_alloc exhaustion */
    int fds[VFS_MAX_FDS];
    int i;
    for (i = 0; i < VFS_MAX_FDS; i++) {
        fds[i] = fd_alloc(task, ino, O_RDONLY);
        if (fds[i] < 0) break;
    }
    TEST_ASSERT_EQ(VFS_MAX_FDS, i, "fd table filled to VFS_MAX_FDS");
    int over = fd_alloc(task, ino, O_RDONLY);
    TEST_ASSERT_EQ(-1, over, "fd_alloc returns -1 when full");

    for (int j = 0; j < i; j++) {
        fd_free(task, fds[j]);
    }
    if (ino != NULL) {
        inode_free(ino);
    }
    if (tid >= 0) {
        (void)task_delete(tid);
    }
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
    (void)test_tmp_unlink("ocreat_new");
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
    (void)test_tmp_unlink("trunc_test");
}

/*============================================================================
 * Test 16: task cleanup closes fd table entries
 *============================================================================*/

static volatile int fd_cleanup_ready;
static volatile int fd_cleanup_cap;

static void fd_cleanup_worker(void *arg) {
    (void)arg;

    fd_cleanup_cap = vfs_open("/tmp/fd_cleanup", O_RDWR);
    fd_cleanup_ready = 1;

    while (1) {
        task_delay(1);
    }
}

static void test_task_fd_cleanup(void) {
    test_section("Test 16: task fd cleanup");

    int setup = vfs_open("/tmp/fd_cleanup", O_RDWR | O_CREAT);
    TEST_ASSERT(setup >= 0, "create fd_cleanup file");
    if (setup >= 0) {
        vfs_close(setup);
    }

    inode_t *ino = vfs_lookup("/tmp/fd_cleanup");
    TEST_ASSERT_NOT_NULL(ino, "lookup fd_cleanup inode");
    if (!ino) return;

    uint32_t base_ref = ino->refcount;
    fd_cleanup_ready = 0;
    fd_cleanup_cap = KERN_ERR;

    task_id_t tid = task_create("fd_clean", fd_cleanup_worker, NULL, 12, 0);
    TEST_ASSERT(tid >= 0, "fd cleanup worker created");
    if (tid < 0) {
        inode_put(ino);
        return;
    }

    task_start(tid);
    for (int i = 0; i < 50 && fd_cleanup_ready == 0; i++) {
        task_delay(1);
    }

    TEST_ASSERT(fd_cleanup_ready == 1, "worker opened fd");
    TEST_ASSERT(fd_cleanup_cap >= 0, "worker vfs_open returned cap");
    TEST_ASSERT_EQ((int)(base_ref + 1), (int)ino->refcount,
                   "open fd holds inode ref");

    kern_err_t err = task_delete(tid);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err, "delete worker OK");
    TEST_ASSERT_EQ((int)base_ref, (int)ino->refcount,
                   "task delete released fd inode ref");

    inode_put(ino);
    (void)test_tmp_unlink("fd_cleanup");
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
    test_vfs_root_readdir_and_normalize();
    test_vfs_fd_readdir();
    test_vfs_mount_redirect();
    test_fd_table_ops();
    test_dev_null();
    test_devfs_register();
    test_ramfs_basic();
    test_vfs_ocreat();
    test_ramfs_append();
    (void)test_tmp_unlink("testfile");
    test_vfs_cap_guard();
    test_dot_entries();
    test_o_trunc();
    test_lseek_boundary();
    test_task_fd_cleanup();
}

TEST_MODULE_REGISTER(vfs, test_vfs_module);

#endif /* VFS_ENABLE && TEST_MODULE_VFS */
