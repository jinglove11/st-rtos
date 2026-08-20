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
#define EI_CLASS      4
#define EI_DATA       5
#define ELFCLASS32    1
#define ELFDATA2LSB   1
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
 * P1-6 (C1): 静态重定位支持 —— 节头/符号/REL 解析
 *============================================================================*/

typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint32_t sh_flags;
    uint32_t sh_addr;
    uint32_t sh_offset;
    uint32_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint32_t sh_addralign;
    uint32_t sh_entsize;
} Elf32_Shdr;

typedef struct {
    uint32_t st_name;
    uint32_t st_value;
    uint32_t st_size;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
} Elf32_Sym;

typedef struct {
    uint32_t r_offset;
    uint32_t r_info;
} Elf32_Rel;

#define SHT_SYMTAB    2
#define SHT_STRTAB    3
#define SHT_REL       9

#define SHN_UNDEF     0

/* ARM 重定位类型(仅支持可静态应用的子集;其余拒绝) */
#define R_ARM_ABS32            2   /* (S + A) |> word32 */
#define R_ARM_V4BX             40  /* NOP 标记,跳过 */
#define R_ARM_MOVW_ABS_NC      41  /* (S + A)[15:0] |> MOVW imm16 (A32) */
#define R_ARM_MOVT_ABS         42  /* (S + A)[31:16] |> MOVT imm16 (A32) */
#define R_ARM_THM_MOVW_ABS_NC  47  /* 同上, T32 编码 */
#define R_ARM_THM_MOVT_ABS     48

/*============================================================================
 * API
 *============================================================================*/

/**
 * elf_load — parse + load an ELF image and create a user task.
 *
 * @param image      pointer to the ELF file in memory (flash XIP or RAM)
 * @param image_size exact byte size of the image (P1-5: 段表与每个
 *                   PT_LOAD 的 [p_offset, p_offset+p_filesz) 必须落在
 *                   镜像内,溢出安全判定;越界/截断镜像一律 PARAM)
 * @param name       task name for the created task
 * @param prio       task priority
 * @param out_tid    receives the new task id
 * @return KERN_OK on success, KERN_ERR_* on failure.
 *
 * Validation contract (P1-5):
 *   - header: magic/32-bit/LSB/EXEC/ARM, e_phentsize == sizeof(Elf32_Phdr),
 *     phnum 1..16, ph 表完整落在镜像内
 *   - segment: W^X 强制(PF_W|PF_X 的 PT_LOAD 拒绝);p_memsz >= p_filesz;
 *     每个 PT_LOAD 的文件范围不越过镜像边界
 *   - 必须存在 PF_X PT_LOAD,且 e_entry 落在其 link 地址范围内
 *   - 可写段(.data/.bss)由 Frame 池分配(cap 归任务,退出自动回收)并经
 *     mpu_map_add 映射 RW+XN(参与 P1-3 软映射表/LRU 记账)
 *
 * The ELF's .text must be linked to a flash XIP address (0x10000000+).
 * .data/.bss are copied to a kmalloc'd RAM block and MPU-mapped.
 */
kern_err_t elf_load(const void *image, size_t image_size, const char *name,
                    uint8_t prio, task_id_t *out_tid);

#endif /* ELF_LOADER */
#endif /* ELF_LOADER_H */
