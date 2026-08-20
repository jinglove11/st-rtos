/**
 * @file elf_loader.c
 * @brief Core completion #6 — ELF process loader implementation
 * P1-5 (C1): 完整段校验 + 溢出检查 + W^X 强制;可写段走 Frame/mpu_map_add。
 */

#include "elf_loader.h"

#if ELF_LOADER

#include "task.h"
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

    /* 第一遍:全段校验 + 收集可执行段/可写段信息(不分配任何资源)。
     *
     * The ELF was linked to a flash address (e.g. 0x10100000) but is actually
     * embedded in the firmware at a different flash address (the .incbin
     * location). We adjust the entry point by the delta between the link
     * address and the actual embedded address. */
    int has_x_segment = 0;
    uint32_t text_link_addr = 0, text_file_offset = 0, text_filesz = 0;
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
     * adjusted_entry = e_entry + delta */
    uint32_t actual_text_addr = (uint32_t)(uintptr_t)base + text_file_offset;
    uint32_t delta = actual_text_addr - text_link_addr;
    uint32_t entry = eh->e_entry + delta;

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
        (void)task_delete(tid);
        return KERN_ERR_STATE;
    }

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
        void *frame_base = NULL;
        size_t frame_size = 0;
        if (kframe_info_for(tcb, frame_cap, &frame_base, &frame_size) !=
                KERN_OK ||
            frame_base == NULL) {
            (void)task_delete(tid);
            return KERN_ERR_STATE;
        }

        /* filesz 部分已在 phdr_validate 保证落在镜像内 */
        memset(frame_base, 0, alloc_size);  /* zero bss */
        memcpy(frame_base, base + wph->p_offset, wph->p_filesz);

        /* 经 P1-3 软映射表登记(参与 slot_owner/LRU 记账 —— 旧实现的
         * regions[3] 直写会让 demand-load 误判空槽并踩掉数据段)。 */
        int mid = mpu_map_add(tcb, (uintptr_t)frame_base,
                              (uint32_t)frame_size,
                              AP_FULL | ATTR_NORMAL_WBWA | XN_ENABLE);
        if (mid < 0) {
            (void)task_delete(tid);
            return KERN_ERR_RESOURCE;
        }
    }

    *out_tid = tid;
    return KERN_OK;
}

#endif /* ELF_LOADER */
