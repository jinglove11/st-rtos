/**
 * @file elf_loader.c
 * @brief Core completion #6 — ELF process loader implementation
 * P1-5 (C1): 完整段校验 + 溢出检查 + W^X 强制;可写段走 Frame/mpu_map_add。
 */

#include "elf_loader.h"

#if ELF_LOADER

#include "task.h"
#include "scheduler.h"
#include "mem.h"
#include "mpu.h"
#include "capability.h"
#include "hal.h"
#include <string.h>

/*============================================================================
 * Validation — 全部检查在镜像内完成,任何越界/截断/W^X 违例拒绝
 *============================================================================*/

static int elf_validate_hdr(const Elf32_Ehdr *eh, size_t image_size) {
    if (image_size < sizeof(Elf32_Ehdr)) {
        return 0;  /* 截断:装不下 ELF 头 */
    }
    if (eh->e_ident[EI_MAG0] != ELFMAG0 ||
        eh->e_ident[EI_MAG1] != ELFMAG1 ||
        eh->e_ident[EI_MAG2] != ELFMAG2 ||
        eh->e_ident[EI_MAG3] != ELFMAG3) {
        return 0;  /* bad magic */
    }
    if (eh->e_ident[EI_CLASS] != ELFCLASS32 ||
        eh->e_ident[EI_DATA] != ELFDATA2LSB) {
        return 0;  /* 非 32 位小端 */
    }
    if (eh->e_type != ET_EXEC) {
        return 0;  /* not an executable */
    }
    if (eh->e_machine != EM_ARM) {
        return 0;  /* not ARM */
    }
    if (eh->e_phnum == 0 || eh->e_phnum > 16) {
        return 0;  /* no program headers (or too many) */
    }
    if (eh->e_phentsize != sizeof(Elf32_Phdr)) {
        return 0;  /* ph 表项大小不符(防止步进错位读越界) */
    }
    if (eh->e_phoff < sizeof(Elf32_Ehdr) ||
        eh->e_phoff > image_size) {
        return 0;  /* ph 表起点越界 */
    }
    /* 溢出安全:ph 表整体必须落在镜像内 */
    size_t ph_bytes = (size_t)eh->e_phnum * sizeof(Elf32_Phdr);
    if (ph_bytes > image_size - eh->e_phoff) {
        return 0;
    }
    return 1;
}

/* 单段检查:文件范围 [p_offset, p_offset+p_filesz) 不越过镜像,
 * p_memsz >= p_filesz,W^X 互斥。 */
static int phdr_validate(const Elf32_Phdr *ph, size_t image_size) {
    if (ph->p_type != PT_LOAD) {
        return 1;  /* 非 LOAD 段不占内存,只要求 ph 表本身在界内(头检查已保证) */
    }
    if ((ph->p_flags & PF_W) && (ph->p_flags & PF_X)) {
        return 0;  /* W^X 强制:可写可执行段拒绝 */
    }
    if (ph->p_memsz < ph->p_filesz) {
        return 0;  /* memsz 装不下文件内容 */
    }
    if (ph->p_offset > image_size ||
        (size_t)ph->p_filesz > image_size - ph->p_offset) {
        return 0;  /* 文件范围越过镜像边界(截断/恶意) */
    }
    return 1;
}

static const Elf32_Phdr *elf_phdr(const Elf32_Ehdr *eh, const uint8_t *base,
                                  uint16_t i) {
    return (const Elf32_Phdr *)
        (base + eh->e_phoff + (uint32_t)i * eh->e_phentsize);
}

/*============================================================================
 * P1-6: 静态重定位 —— 段 bias 应用(ABS32 / MOVW / MOVT,A32+T32)
 *
 * 链接地址 → 运行地址 bias:
 *   X 段符号 → x_bias(RAM 加载模式)或 x_delta(XIP)
 *   W 段符号 → w_bias(frame 基址 - link vaddr)
 * 重定位目标 r_offset 必须落在可写副本里(W 段恒在 frame;X 段仅当
 * RAM 加载模式)。XIP 模式下出现 text 重定位 → 拒绝(不可写 flash)。
 *============================================================================*/

typedef struct {
    const Elf32_Ehdr *eh;
    const uint8_t *base;
    size_t image_size;
    /* 段信息 */
    uint32_t x_vaddr, x_memsz;
    int32_t  x_bias;      /* X 段符号运行地址 = link + x_bias */
    uint32_t w_vaddr, w_memsz;
    int32_t  w_bias;
    /* 副本基址(重定位写入目标;NULL = 不可写) */
    uint8_t *x_copy;
    uint8_t *w_copy;
} reloc_ctx_t;

static int shdr_table_ok(const Elf32_Ehdr *eh, size_t image_size,
                         const Elf32_Shdr **out_sh, uint16_t *out_shnum) {
    if (eh->e_shoff == 0 || eh->e_shnum == 0 ||
        eh->e_shentsize != sizeof(Elf32_Shdr)) {
        return 0;
    }
    if (eh->e_shoff > image_size) {
        return 0;
    }
    size_t bytes = (size_t)eh->e_shnum * sizeof(Elf32_Shdr);
    if (bytes > image_size - eh->e_shoff) {
        return 0;
    }
    *out_sh = (const Elf32_Shdr *)
        ((const uint8_t *)eh + eh->e_shoff);
    *out_shnum = eh->e_shnum;
    return 1;
}

/* 符号 link 地址 → 运行地址;不在任一段内 → -1 */
static int32_t reloc_sym_addr(const reloc_ctx_t *rc, uint32_t st_value,
                              uint32_t st_shndx) {
    if (st_shndx == SHN_UNDEF) {
        return -1;
    }
    if (st_value >= rc->x_vaddr &&
        st_value - rc->x_vaddr < rc->x_memsz) {
        return (int32_t)(st_value + (uint32_t)rc->x_bias);
    }
    if (rc->w_memsz != 0U &&
        st_value >= rc->w_vaddr &&
        st_value - rc->w_vaddr < rc->w_memsz) {
        return (int32_t)(st_value + (uint32_t)rc->w_bias);
    }
    return -1;
}

/* T32 MOVW/MOVT imm16 位域(实测编码 f240 0100 / f2c2 0108 对照验证):
 * hw0[3:0]=imm4, hw0[10]=i, hw1[14:12]=imm3, hw1[11:8]=Rd, hw1[7:0]=imm8。
 * 32 位 T32 指令 = 两个相邻 16 位半字(小端),必须经 u16 视图访问。 */
static void thm_patch_imm16(uint32_t *insn, uint16_t imm) {
    uint16_t *hw = (uint16_t *)insn;
    hw[0] = (uint16_t)((hw[0] & ~0x0FU) | ((imm >> 12) & 0x0FU));
    hw[0] = (uint16_t)((hw[0] & ~(1U << 10)) |
                       (((imm >> 11) & 1U) << 10));
    hw[1] = (uint16_t)((hw[1] & ~(0x7U << 12)) |
                       (((imm >> 8) & 0x7U) << 12));
    hw[1] = (uint16_t)((hw[1] & ~0xFFU) | (imm & 0xFFU));
}

/* A32 MOVW/MOVT imm16 位域: imm4[19:16], imm12[11:0] */
static void arm_patch_imm16(uint32_t *insn, uint16_t imm) {
    *insn = (*insn & ~(0xFU << 16)) | (((imm >> 12) & 0xFU) << 16);
    *insn = (*insn & ~0xFFFU) | (imm & 0xFFFU);
}

static kern_err_t apply_one_reloc(reloc_ctx_t *rc, const Elf32_Sym *symtab,
                                  uint16_t nsyms, const Elf32_Rel *rel) {
    uint32_t type = rel->r_info & 0xFFU;
    uint32_t sym_idx = rel->r_info >> 8;
    if (type == R_ARM_V4BX) {
        return KERN_OK;  /* NOP 标记 */
    }
    if (sym_idx >= nsyms) {
        return KERN_ERR_PARAM;
    }
    uint32_t s_link = symtab[sym_idx].st_value;
    int32_t s = reloc_sym_addr(rc, s_link, symtab[sym_idx].st_shndx);
    if (s < 0) {
        return KERN_ERR_PARAM;
    }

    /* 重定位目标必须落在某个可写副本内 */
    uint32_t *word = NULL;
    if (rc->x_copy != NULL && rel->r_offset >= rc->x_vaddr &&
        rel->r_offset - rc->x_vaddr < rc->x_memsz) {
        word = (uint32_t *)(rc->x_copy + (rel->r_offset - rc->x_vaddr));
    } else if (rc->w_copy != NULL && rc->w_memsz != 0U &&
               rel->r_offset >= rc->w_vaddr &&
               rel->r_offset - rc->w_vaddr < rc->w_memsz) {
        word = (uint32_t *)(rc->w_copy + (rel->r_offset - rc->w_vaddr));
    } else {
        return KERN_ERR_PARAM;  /* 目标在 flash(XIP text)或段外 */
    }

    switch (type) {
    case R_ARM_ABS32: {
        /* --emit-relocs 输出:原字已是 link 期解算值(S_link + A)。
         * 精确恢复 A 后按运行地址重算:new = S' + (old - S_link)
         *                          = old + bias(无二次计入) */
        *word = (uint32_t)s + (*word - s_link);
        return KERN_OK;
    }
    /* MOVW/MOVT 契约:生成侧 #:lower16:/:upper16:sym 纯符号引用(A=0),
     * imm 半字直接取 S' —— 回避 imm 域内 addend 恢复的回绕歧义。 */
    case R_ARM_MOVW_ABS_NC:
        arm_patch_imm16(word, (uint16_t)((uint32_t)s & 0xFFFFU));
        return KERN_OK;
    case R_ARM_MOVT_ABS:
        arm_patch_imm16(word, (uint16_t)((uint32_t)s >> 16));
        return KERN_OK;
    case R_ARM_THM_MOVW_ABS_NC:
        thm_patch_imm16(word, (uint16_t)((uint32_t)s & 0xFFFFU));
        return KERN_OK;
    case R_ARM_THM_MOVT_ABS:
        thm_patch_imm16(word, (uint16_t)((uint32_t)s >> 16));
        return KERN_OK;
    default:
        return KERN_ERR_PARAM;  /* 不支持的重定位类型 */
    }
}

/* 镜像内是否有 SHT_REL 节(有 → text 需 RAM 加载) */
static int elf_has_relocs(const Elf32_Ehdr *eh, size_t image_size) {
    const Elf32_Shdr *sh;
    uint16_t shnum;
    if (!shdr_table_ok(eh, image_size, &sh, &shnum)) {
        return -1;  /* 节表非法 → 头校验阶段拒绝 */
    }
    for (uint16_t i = 0; i < shnum; i++) {
        if (sh[i].sh_type == SHT_REL) {
            return 1;
        }
    }
    return 0;
}

/* 应用全部重定位;任一失败 → PARAM(整体 load 失败) */
static kern_err_t apply_relocations(reloc_ctx_t *rc) {
    const Elf32_Shdr *sh;
    uint16_t shnum;
    if (!shdr_table_ok(rc->eh, rc->image_size, &sh, &shnum)) {
        return KERN_ERR_PARAM;  /* has_relocs 时节表必须合法 */
    }

    const Elf32_Sym *symtab = NULL;
    uint32_t nsyms = 0;
    for (uint16_t i = 0; i < shnum; i++) {
        if (sh[i].sh_type == SHT_SYMTAB) {
            if (sh[i].sh_offset > rc->image_size ||
                sh[i].sh_size > rc->image_size - sh[i].sh_offset ||
                sh[i].sh_entsize != sizeof(Elf32_Sym) ||
                sh[i].sh_size % sizeof(Elf32_Sym) != 0U) {
                return KERN_ERR_PARAM;
            }
            symtab = (const Elf32_Sym *)(rc->base + sh[i].sh_offset);
            nsyms = sh[i].sh_size / sizeof(Elf32_Sym);
            break;
        }
    }
    if (symtab == NULL) {
        return KERN_ERR_PARAM;  /* 有 REL 却无 SYMTAB */
    }

    for (uint16_t i = 0; i < shnum; i++) {
        if (sh[i].sh_type != SHT_REL) {
            continue;
        }
        if (sh[i].sh_offset > rc->image_size ||
            sh[i].sh_size > rc->image_size - sh[i].sh_offset ||
            sh[i].sh_entsize != sizeof(Elf32_Rel) ||
            sh[i].sh_size % sizeof(Elf32_Rel) != 0U) {
            return KERN_ERR_PARAM;
        }
        const Elf32_Rel *rel =
            (const Elf32_Rel *)(rc->base + sh[i].sh_offset);
        uint32_t n = sh[i].sh_size / sizeof(Elf32_Rel);
        for (uint32_t r = 0; r < n; r++) {
            kern_err_t e = apply_one_reloc(rc, symtab, (uint16_t)nsyms,
                                           &rel[r]);
            if (e != KERN_OK) {
                return e;
            }
        }
    }
    return KERN_OK;
}

/*============================================================================
 * Loader
 *============================================================================*/

kern_err_t elf_load(const void *image, size_t image_size, const char *name,
                    uint8_t prio, task_id_t *out_tid) {
    if (image == NULL || out_tid == NULL || image_size == 0) {
        return KERN_ERR_PARAM;
    }
    *out_tid = KERN_INVALID_ID;

    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)image;
    if (!elf_validate_hdr(eh, image_size)) {
        return KERN_ERR_PARAM;
    }

    const uint8_t *base = (const uint8_t *)image;

    /* P1-6: 节表若存在必须合法;有 SHT_REL → text 必须 RAM 加载
     * (XIP flash 不可写,text 重定位无处应用)。 */
    int reloc_mode = elf_has_relocs(eh, image_size);
    if (reloc_mode < 0) {
        return KERN_ERR_PARAM;
    }

    /* 第一遍:全段校验 + 收集可执行段/可写段信息(不分配任何资源)。
     *
     * The ELF was linked to a flash address (e.g. 0x10100000) but is actually
     * embedded in the firmware at a different flash address (the .incbin
     * location). We adjust the entry point by the delta between the link
     * address and the actual embedded address. */
    int has_x_segment = 0;
    uint32_t text_link_addr = 0, text_file_offset = 0, text_filesz = 0;
    uint32_t x_memsz = 0;
    int w_segment_idx = -1;

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const Elf32_Phdr *ph = elf_phdr(eh, base, i);
        if (!phdr_validate(ph, image_size)) {
            return KERN_ERR_PARAM;
        }
        if (ph->p_type != PT_LOAD) {
            continue;
        }
        if (ph->p_flags & PF_X) {
            if (has_x_segment) {
                return KERN_ERR_PARAM;  /* 多个可执行段:布局不支持 */
            }
            has_x_segment = 1;
            text_link_addr = ph->p_vaddr;
            text_file_offset = ph->p_offset;
            text_filesz = ph->p_filesz;
            x_memsz = ph->p_memsz;
        }
        if (ph->p_flags & PF_W) {
            if (w_segment_idx >= 0) {
                return KERN_ERR_PARAM;  /* 多个可写段:当前映射模型只支持一个 */
            }
            w_segment_idx = (int)i;
        }
    }
    if (!has_x_segment) {
        return KERN_ERR_PARAM;  /* 无可执行段:入口无处安放 */
    }

    /* 入口必须落在可执行段的 link 地址范围内(拒绝对齐错乱的 entry) */
    if (eh->e_entry < text_link_addr ||
        eh->e_entry - text_link_addr >= text_filesz) {
        return KERN_ERR_PARAM;
    }

    /* actual_text_addr_in_memory = embedded_base + text_file_offset
     * delta = actual_text_addr - text_link_addr
     * adjusted_entry = e_entry + delta
     * (重定位模式下 text 会复制进 frame,entry 在帧分配后按 x_bias 重算) */
    uint32_t actual_text_addr = (uint32_t)(uintptr_t)base + text_file_offset;
    uint32_t delta = actual_text_addr - text_link_addr;
    uint32_t entry = eh->e_entry + delta;

    /* P1-6 重定位模式:text 先复制进 RX 帧(帧先以创建者名义分配,
     * 建任务后 cap_move_to 移交 —— frame 生命周期仍归任务,退出回收)。
     * entry = 帧 + (e_entry - x_vaddr)。W^X: text 帧 RO+X(无 W)。
     * XIP 模式(无重定位): 维持原有 entry delta 行为。 */
    cap_id_t text_frame_cap = KERN_INVALID_ID;
    void *text_frame_base = NULL;
    if (reloc_mode) {
        uint32_t t_alloc = (x_memsz + 31U) & ~31U;
        if (t_alloc == 0U) t_alloc = 32U;
        tcb_t *creator = sched_get_current();
        if (creator == NULL) {
            return KERN_ERR_STATE;
        }
        /* TRANSFER:建任务后经 cap_move_to 移交(内核内部中转授权) */
        text_frame_cap = kframe_create_cap_for(
            creator, t_alloc,
            CAP_READ | CAP_WRITE | CAP_MANAGE | CAP_TRANSFER);
        if (text_frame_cap < 0) {
            return KERN_ERR_RESOURCE;
        }
        if (kframe_info_for(creator, text_frame_cap,
                            &text_frame_base, NULL) != KERN_OK ||
            text_frame_base == NULL) {
            (void)cap_delete(text_frame_cap);
            return KERN_ERR_STATE;
        }
        memset(text_frame_base, 0, t_alloc);
        memcpy(text_frame_base, base + text_file_offset, text_filesz);
        entry = (uint32_t)(uintptr_t)text_frame_base +
                (eh->e_entry - text_link_addr);
    }

    /* 先建任务(task_create_user 设 region 0 flash RO+X 与 region 2 栈),
     * 可写段资源随后挂到任务上 —— 任一步失败统一 task_delete 回收
     * (cap 吊销路径自动释放已分配 frame,无手工泄漏面)。 */
    task_id_t tid = task_create_user(name ? name : "elf_proc",
                                     (task_func_t)(uintptr_t)entry,
                                     NULL,
                                     prio,
                                     2048);
    if (tid < 0) {
        return KERN_ERR_RESOURCE;
    }
    tcb_t *tcb = task_get_tcb(tid);
    if (tcb == NULL || tcb->aspace == NULL) {
        if (text_frame_cap >= 0) {
            (void)cap_delete(text_frame_cap);
        }
        (void)task_delete(tid);
        return KERN_ERR_STATE;
    }

    if (text_frame_cap >= 0) {
        /* text 帧移交任务并映射 RX(XN 关) */
        if (cap_move_to(NULL, text_frame_cap, tcb, NULL) != KERN_OK) {
            (void)cap_delete(text_frame_cap);
            (void)task_delete(tid);
            return KERN_ERR_STATE;
        }
        int mid = mpu_map_add(tcb, (uintptr_t)text_frame_base,
                              ((x_memsz + 31U) & ~31U),
                              AP_PRW_URO | ATTR_NORMAL_WBWA);
        if (mid < 0) {
            (void)task_delete(tid);
            return KERN_ERR_RESOURCE;
        }
    }

    void *w_frame_base = NULL;
    if (w_segment_idx >= 0) {
        const Elf32_Phdr *wph = elf_phdr(eh, base, (uint16_t)w_segment_idx);

        /* .data + .bss:Frame 池分配(cap 归任务,任务退出自动回收 ——
         * 修复旧实现 kmalloc 无跟踪的泄漏),round 到 32 字节 MPU 合规。 */
        uint32_t alloc_size = (wph->p_memsz + 31U) & ~31U;
        if (alloc_size == 0) alloc_size = 32U;

        cap_id_t frame_cap = kframe_create_cap_for(
            tcb, alloc_size, CAP_READ | CAP_WRITE | CAP_MANAGE);
        if (frame_cap < 0) {
            (void)task_delete(tid);
            return KERN_ERR_RESOURCE;
        }
        size_t frame_size = 0;
        if (kframe_info_for(tcb, frame_cap, &w_frame_base, &frame_size) !=
                KERN_OK ||
            w_frame_base == NULL) {
            (void)task_delete(tid);
            return KERN_ERR_STATE;
        }

        /* filesz 部分已在 phdr_validate 保证落在镜像内 */
        memset(w_frame_base, 0, alloc_size);  /* zero bss */
        memcpy(w_frame_base, base + wph->p_offset, wph->p_filesz);

        /* 经 P1-3 软映射表登记(参与 slot_owner/LRU 记账 —— 旧实现的
         * regions[3] 直写会让 demand-load 误判空槽并踩掉数据段)。 */
        int mid = mpu_map_add(tcb, (uintptr_t)w_frame_base,
                              (uint32_t)frame_size,
                              AP_FULL | ATTR_NORMAL_WBWA | XN_ENABLE);
        if (mid < 0) {
            (void)task_delete(tid);
            return KERN_ERR_RESOURCE;
        }
    }

    /* P1-6: 应用静态重定位(bias = 副本基址 - 段 link vaddr;
     * XIP 模式下 x_copy=NULL → text 重定位被拒,符合不可写 flash 约束) */
    if (reloc_mode) {
        const Elf32_Phdr *wph = w_segment_idx >= 0
            ? elf_phdr(eh, base, (uint16_t)w_segment_idx) : NULL;
        reloc_ctx_t rc = {
            .eh = eh,
            .base = base,
            .image_size = image_size,
            .x_vaddr = text_link_addr,
            .x_memsz = x_memsz,
            .x_bias = (int32_t)((uint32_t)(uintptr_t)text_frame_base -
                                text_link_addr),
            .w_vaddr = wph != NULL ? wph->p_vaddr : 0U,
            .w_memsz = wph != NULL ? wph->p_memsz : 0U,
            .w_bias = (int32_t)((uint32_t)(uintptr_t)w_frame_base -
                                (wph != NULL ? wph->p_vaddr : 0U)),
            .x_copy = (uint8_t *)text_frame_base,
            .w_copy = (uint8_t *)w_frame_base,
        };
        kern_err_t re = apply_relocations(&rc);
        if (re != KERN_OK) {
            (void)task_delete(tid);
            return re;
        }
    }

    *out_tid = tid;
    return KERN_OK;
}

#endif /* ELF_LOADER */
