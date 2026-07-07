/**
 * @file elf_loader.h
 * @brief Core completion #6 — ELF process loader
 *
 * Loads a position-dependent bare-metal ELF (ARM Thumb, linked to a flash
 * XIP address) and creates a user task to execute it. The .text segment
 * executes directly from flash (Region 0 covers all flash as RO+X); the
 * .data/.bss segment is copied to a kmalloc'd RAM block and mapped into the
 * task's MPU Region 3 as RW+XN.
 *
 * No dynamic relocations (R_ARM_*) in this slice — the ELF must be linked
 * to its final flash address. Relocations are a follow-on enhancement.
 *
 * Kconfig: ELF_LOADER.
 */

#ifndef ELF_LOADER_H
#define ELF_LOADER_H

#include "kernel_types.h"
#include "kernel_config.h"

#if ELF_LOADER

#include <stdint.h>

/*============================================================================
 * Minimal ELF32 structures (we don't ship a full <elf.h>)
 *============================================================================*/

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} Elf32_Phdr;

#define EI_MAG0       0
#define EI_MAG1       1
#define EI_MAG2       2
#define EI_MAG3       3
#define ELFMAG0       0x7F
#define ELFMAG1       'E'
#define ELFMAG2       'L'
#define ELFMAG3       'F'

#define ET_EXEC       2
#define EM_ARM        40

#define PT_LOAD       1
#define PF_X          1
#define PF_W          2
#define PF_R          4

/*============================================================================
 * API
 *============================================================================*/

/**
 * elf_load — parse + load an ELF image and create a user task.
 *
 * @param image   pointer to the ELF file in memory (flash XIP or RAM)
 * @param name    task name for the created task
 * @param prio    task priority
 * @param out_tid receives the new task id
 * @return KERN_OK on success, KERN_ERR_* on failure.
 *
 * The ELF's .text must be linked to a flash XIP address (0x10000000+).
 * .data/.bss are copied to a kmalloc'd RAM block and MPU-mapped.
 */
kern_err_t elf_load(const void *image, const char *name,
                    uint8_t prio, task_id_t *out_tid);

#endif /* ELF_LOADER */
#endif /* ELF_LOADER_H */
