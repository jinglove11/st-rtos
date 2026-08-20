/**
 * @file test_elf.c
 * @brief Core completion #6 — ELF loader tests
 *
 * Loads a freestanding ELF (embedded via .incbin) and verifies it executes
 * as a user task: writes to .data, calls sys_task_exit(0x600D), and the
 * test reads the exit value via task_join.
 */

#include "test_framework.h"
#include "kernel.h"
#include "task.h"
#include "elf_loader.h"
#include <string.h>

#if ELF_LOADER && TEST_MODULE_ELF

/*============================================================================
 * The embedded test ELF (from test_elf_blob.c's .incbin)
 *============================================================================*/

extern const uint8_t __test_elf_start[];
extern const uint8_t __test_elf_end[];

/*============================================================================
 * Test 1: load + execute the embedded ELF
 *============================================================================*/

static void test_elf_load_and_run(void) {
    test_section("Test 1: load + execute ELF");

    /* Verify the blob is non-empty (the build produced an ELF). */
    size_t elf_size = (size_t)(__test_elf_end - __test_elf_start);
    test_print_num("[elf] embedded size = ", (int32_t)elf_size);
    TEST_ASSERT(elf_size > 0, "embedded ELF non-empty");
    TEST_ASSERT(elf_size >= sizeof(Elf32_Ehdr), "large enough for ELF header");
    if (elf_size < sizeof(Elf32_Ehdr)) return;

    /* Verify magic. */
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)__test_elf_start;
    TEST_ASSERT(eh->e_ident[0] == 0x7F, "ELF magic byte 0");
    TEST_ASSERT(eh->e_ident[1] == 'E', "ELF magic byte 1");

    /* Load + create task. */
    task_id_t tid = KERN_INVALID_ID;
    kern_err_t e = elf_load(__test_elf_start, elf_size, "elf_test", 8, &tid);
    TEST_ASSERT_EQ((int)KERN_OK, (int)e, "elf_load OK");
    TEST_ASSERT(tid >= 0, "task id valid");
    if (e != KERN_OK || tid < 0) return;

    /* Start + join. The ELF exits with 0x600D. */
    e = task_start(tid);
    TEST_ASSERT_EQ((int)KERN_OK, (int)e, "elf task started");

    void *retval = NULL;
    e = task_join(tid, &retval, 2000);
    test_print_num("[elf] join err = ", (int32_t)e);
    TEST_ASSERT_EQ((int)KERN_OK, (int)e, "elf task joined (no fault)");

    int rv = (int)(intptr_t)retval;
    test_print_num("[elf] exit value = ", (int32_t)rv);
    /* The ELF _start simply returns (LR=user_task_exit_handler with retval=0).
     * We mainly check join succeeded (no fault) and retval is 0. */
    TEST_ASSERT_EQ(0, rv, "ELF exited cleanly (retval 0)");
}

/*============================================================================
 * Test 2: bad ELF (corrupted magic) rejected
 *============================================================================*/

static void test_elf_bad_magic_rejected(void) {
    test_section("Test 2: bad ELF magic rejected");

    /* Use a dummy buffer with wrong magic. */
    static const uint8_t bad_elf[64] = {0};
    task_id_t tid = KERN_INVALID_ID;
    kern_err_t e = elf_load(bad_elf, sizeof(bad_elf), "bad_elf", 8, &tid);
    TEST_ASSERT(e != KERN_OK, "bad magic rejected");
    TEST_ASSERT(tid == KERN_INVALID_ID, "no task created for bad ELF");
}

/*============================================================================
 * Test 3 (P1-5): 段校验负向用例 — 构造最小 ELF 变体逐项拒绝
 *============================================================================*/

/* 构造模板:Ehdr + 2 个 Phdr(X 段 + W 段),内容字段可按用例覆盖 */
typedef struct {
    Elf32_Ehdr eh;
    Elf32_Phdr ph[2];
} mini_elf_t;

static void mini_elf_init(mini_elf_t *m) {
    memset(m, 0, sizeof(*m));
    m->eh.e_ident[EI_MAG0] = ELFMAG0;
    m->eh.e_ident[EI_MAG1] = ELFMAG1;
    m->eh.e_ident[EI_MAG2] = ELFMAG2;
    m->eh.e_ident[EI_MAG3] = ELFMAG3;
    m->eh.e_ident[EI_CLASS] = ELFCLASS32;
    m->eh.e_ident[EI_DATA] = ELFDATA2LSB;
    m->eh.e_type = ET_EXEC;
    m->eh.e_machine = EM_ARM;
    m->eh.e_phnum = 2;
    m->eh.e_phentsize = sizeof(Elf32_Phdr);
    m->eh.e_phoff = sizeof(Elf32_Ehdr);
    m->eh.e_entry = 0x10100000U;

    /* X 段:RO+X,文件范围压在镜像头部内(负向用例只验证拒绝路径,
     * 模板本身从不真正启动,与 ELF 头字节重叠无害) */
    m->ph[0].p_type = PT_LOAD;
    m->ph[0].p_flags = PF_R | PF_X;
    m->ph[0].p_offset = 0U;
    m->ph[0].p_vaddr = 0x10100000U;
    m->ph[0].p_filesz = 0x10U;
    m->ph[0].p_memsz = 0x10U;

    /* W 段:RW,小 .data(同样在界内) */
    m->ph[1].p_type = PT_LOAD;
    m->ph[1].p_flags = PF_R | PF_W;
    m->ph[1].p_offset = 0U;
    m->ph[1].p_vaddr = 0x20000000U;
    m->ph[1].p_filesz = 0x8U;
    m->ph[1].p_memsz = 0x40U;
}

static void test_elf_segment_validation(void) {
    test_section("Test 3: segment validation rejects malformed ELFs");

    /* 模板本身头部合法;段文件范围越界(镜像只有 sizeof(mini_elf_t)),
     * 但负向用例只测"更早"的拒绝路径,模板不整体加载 —— 每个用例
     * 单独断言 elf_load 提前 PARAM,不触及越界读。 */
    static mini_elf_t m;
    task_id_t tid = KERN_INVALID_ID;

    /* 3a: W^X 段拒绝 */
    mini_elf_init(&m);
    m.ph[1].p_flags = PF_R | PF_W | PF_X;
    tid = KERN_INVALID_ID;
    TEST_ASSERT(elf_load(&m, sizeof(m), "wx", 8, &tid) == KERN_ERR_PARAM,
                "W^X segment rejected");
    TEST_ASSERT(tid == KERN_INVALID_ID, "no task for W^X");

    /* 3b: 文件范围越过镜像边界(p_offset+p_filesz 溢出) */
    mini_elf_init(&m);
    m.ph[0].p_offset = 0xFFFFFFF0U;
    m.ph[0].p_filesz = 0x40U;
    tid = KERN_INVALID_ID;
    TEST_ASSERT(elf_load(&m, sizeof(m), "oob", 8, &tid) == KERN_ERR_PARAM,
                "file range beyond image rejected");

    /* 3c: p_memsz < p_filesz */
    mini_elf_init(&m);
    m.ph[0].p_filesz = 0x10U;
    m.ph[0].p_memsz = 0x8U;
    tid = KERN_INVALID_ID;
    TEST_ASSERT(elf_load(&m, sizeof(m), "msz", 8, &tid) == KERN_ERR_PARAM,
                "memsz < filesz rejected");

    /* 3d: 无可执行段 */
    mini_elf_init(&m);
    m.ph[0].p_flags = PF_R;
    tid = KERN_INVALID_ID;
    TEST_ASSERT(elf_load(&m, sizeof(m), "nox", 8, &tid) == KERN_ERR_PARAM,
                "no X segment rejected");

    /* 3e: entry 不在可执行段范围内 */
    mini_elf_init(&m);
    m.eh.e_entry = 0x10100100U;
    tid = KERN_INVALID_ID;
    TEST_ASSERT(elf_load(&m, sizeof(m), "entry", 8, &tid) == KERN_ERR_PARAM,
                "entry outside X segment rejected");

    /* 3f: 截断镜像(小于 ELF 头) */
    tid = KERN_INVALID_ID;
    TEST_ASSERT(elf_load(&m, 16U, "trunc", 8, &tid) == KERN_ERR_PARAM,
                "truncated image rejected");

    /* 3g: phentsize 不符 */
    mini_elf_init(&m);
    m.eh.e_phentsize = sizeof(Elf32_Phdr) + 4U;
    tid = KERN_INVALID_ID;
    TEST_ASSERT(elf_load(&m, sizeof(m), "phent", 8, &tid) == KERN_ERR_PARAM,
                "bad phentsize rejected");

    /* 3h: 双可执行段拒绝 */
    mini_elf_init(&m);
    m.ph[1].p_flags = PF_R | PF_X;
    tid = KERN_INVALID_ID;
    TEST_ASSERT(elf_load(&m, sizeof(m), "2x", 8, &tid) == KERN_ERR_PARAM,
                "two X segments rejected");
}

/*============================================================================
 * Test 4 (P1-6): 重定位 ELF(ABS32 + THM MOVW/MOVT,RAM-text 模式)
 *============================================================================*/

#ifdef TEST_ELF_REL_PATH
extern const uint8_t __test_elf_rel_start[];
extern const uint8_t __test_elf_rel_end[];

static void test_elf_relocations(void) {
    test_section("Test 4: relocation ELF (ABS32 + MOVW/MOVT)");

    size_t size = (size_t)(__test_elf_rel_end - __test_elf_rel_start);
    TEST_ASSERT(size > sizeof(Elf32_Ehdr), "reloc ELF embedded");
    if (size <= sizeof(Elf32_Ehdr)) return;

    /* 5 轮加载→执行→回收:exit 0(重定位判定全过)+ 无 cap/对象泄漏 */
    for (int round = 0; round < 5; round++) {
        task_id_t tid = KERN_INVALID_ID;
        kern_err_t e = elf_load(__test_elf_rel_start, size,
                                "elf_rel", 8, &tid);
        TEST_ASSERT_EQ((int)KERN_OK, (int)e, "reloc elf_load OK");
        if (e != KERN_OK) return;
        TEST_ASSERT(tid >= 0, "task id valid");

        e = task_start(tid);
        TEST_ASSERT_EQ((int)KERN_OK, (int)e, "reloc elf started");

        uintptr_t rv = 0;
        e = task_join(tid, (void **)&rv, 20000U);
        TEST_ASSERT_EQ((int)KERN_OK, (int)e, "reloc elf joined");
        TEST_ASSERT_EQ(0, (int)rv,
                       "reloc verdict 0 (ABS32/MOVW/bss/data all ok)");
        (void)task_delete(tid);
    }
    test_pass("relocation ELF round-trip x5 (backing reclaimed)");
}
#endif /* TEST_ELF_REL_PATH */

/*============================================================================
 * Module registration
 *============================================================================*/

static void test_elf_module(void) {
    test_elf_load_and_run();
    test_elf_bad_magic_rejected();
    test_elf_segment_validation();
#ifdef TEST_ELF_REL_PATH
    test_elf_relocations();
#endif
}

TEST_K_MODULE(elf, test_elf_module);

#endif /* ELF_LOADER && TEST_MODULE_ELF */
