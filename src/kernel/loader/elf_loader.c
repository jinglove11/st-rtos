/**
 * @file elf_loader.c
 * @brief Core completion #6 — ELF process loader implementation
 */

#include "elf_loader.h"

#if ELF_LOADER

#include "task.h"
#include "mem.h"
#include "mpu.h"
#include "hal.h"
#include <string.h>

/*============================================================================
 * Validation
 *============================================================================*/

static int elf_validate(const Elf32_Ehdr *eh) {
    if (eh->e_ident[EI_MAG0] != ELFMAG0 ||
        eh->e_ident[EI_MAG1] != ELFMAG1 ||
        eh->e_ident[EI_MAG2] != ELFMAG2 ||
        eh->e_ident[EI_MAG3] != ELFMAG3) {
        return 0;  /* bad magic */
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
    return 1;
}

/*============================================================================
 * Loader
 *============================================================================*/

kern_err_t elf_load(const void *image, const char *name,
                    uint8_t prio, task_id_t *out_tid) {
    if (image == NULL || out_tid == NULL) {
        return KERN_ERR_PARAM;
    }

    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)image;
    if (!elf_validate(eh)) {
        return KERN_ERR_PARAM;
    }

    const uint8_t *base = (const uint8_t *)image;

    /* The ELF was linked to a flash address (e.g. 0x10100000) but is actually
     * embedded in the firmware at a different flash address (the .incbin
     * location). We must adjust the entry point by the delta between the
     * link address and the actual embedded address. The executable PT_LOAD
     * segment's p_vaddr tells us where the linker thought .text would be,
     * and p_offset tells us where .text actually is in the ELF file. */
    uint32_t text_link_addr = 0;
    uint32_t text_file_offset = 0;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const Elf32_Phdr *ph = (const Elf32_Phdr *)
            (base + eh->e_phoff + (uint32_t)i * eh->e_phentsize);
        if (ph->p_type == PT_LOAD && (ph->p_flags & PF_X)) {
            text_link_addr = ph->p_vaddr;
            text_file_offset = ph->p_offset;
            break;
        }
    }

    /* actual_text_addr_in_memory = embedded_base + text_file_offset
     * delta = actual_text_addr - text_link_addr
     * adjusted_entry = e_entry + delta */
    uint32_t actual_text_addr = (uint32_t)(uintptr_t)base + text_file_offset;
    uint32_t delta = actual_text_addr - text_link_addr;
    uint32_t entry = eh->e_entry + delta;

    /* First pass: find the writable (.data/.bss) segment and allocate RAM.
     * The executable (.text) segment stays in flash (XIP). */
    void *data_ram = NULL;
    uint32_t data_size = 0;

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const Elf32_Phdr *ph = (const Elf32_Phdr *)
            (base + eh->e_phoff + (uint32_t)i * eh->e_phentsize);

        if (ph->p_type != PT_LOAD) {
            continue;
        }

        if (ph->p_flags & PF_W) {
            /* Writable segment: .data + .bss. Allocate RAM for p_memsz
             * (filesz = initialized data, memsz - filesz = bss). */
            data_size = ph->p_memsz;
            /* Round up to 32 bytes for MPU compliance. */
            uint32_t alloc_size = (ph->p_memsz + 31U) & ~31U;
            if (alloc_size == 0) alloc_size = 32U;
            data_ram = kmalloc(alloc_size);
            if (data_ram == NULL) {
                return KERN_ERR_RESOURCE;
            }
            memset(data_ram, 0, alloc_size);  /* zero bss */
            memcpy(data_ram, base + ph->p_offset, ph->p_filesz);
            break;  /* one writable segment for now */
        }
    }

    /* Create the user task. task_create_user sets up Region 0 (flash RO+X)
     * and Region 2 (stack). The entry point is the ELF's e_entry (a flash
     * XIP address). task_create handles hal_stack_init (forces Thumb bit). */
    task_id_t tid = task_create_user(name ? name : "elf_proc",
                                     (task_func_t)(uintptr_t)entry,
                                     NULL,  /* arg — could pass data_ram */
                                     prio,
                                     2048);
    if (tid < 0) {
        if (data_ram) kfree(data_ram);
        return KERN_ERR_RESOURCE;
    }

    /* If we allocated a .data block, map it into the task's MPU Region 3
     * (RW + XN, normal memory). This gives the ELF task private writable
     * data memory. */
    if (data_ram != NULL && data_size > 0) {
        tcb_t *tcb = task_get_tcb(tid);
        if (tcb != NULL) {
            uint32_t alloc_size = (data_size + 31U) & ~31U;
            uint32_t ap = AP_FULL;  /* RW for both privilege levels */
            mpu_region_encode(3, (uint32_t)(uintptr_t)data_ram,
                              alloc_size,
                              RASR_ENABLE | ap | ATTR_NORMAL_WBWA | XN_ENABLE,
                              &tcb->mpu_regions[3][0],
                              &tcb->mpu_regions[3][1]);
        }
        /* NOTE: we don't track data_ram for cleanup on task exit. A production
         * loader would stash it in the TCB or a side table. For Phase 6 the
         * ELF task is long-lived or immediately joined; memory reclamation is
         * deferred. */
    }

    *out_tid = tid;
    return KERN_OK;
}

#endif /* ELF_LOADER */
