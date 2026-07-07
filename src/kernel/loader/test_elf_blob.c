/**
 * @file test_elf_blob.c
 * @brief Embeds the pre-compiled test ELF into the firmware image.
 *
 * The ELF file is built by a CMake/Make custom command before this file is
 * compiled. The .incbin directive pulls the raw ELF bytes into a const
 * section that elf_load() can parse at runtime.
 */

#include "kernel_config.h"

#if ELF_LOADER

/* Symbols exposed to the test. The linker script (rp2350_sections.ld) places
 * the .elf_payload section into flash via INSERT AFTER .rodata. */
/* CMake passes -DTEST_ELF_PATH=/abs/path/test_elf.elf (unquoted macro).
 * We use C-preprocessor string concatenation: ".incbin \"" TOSTRING(path) "\""
 * becomes ".incbin \"/abs/path/test_elf.elf\"" in the asm string literal. */
#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

#define INCBIN_DIRECTIVE ".incbin \"" TOSTRING(TEST_ELF_PATH) "\"\n"

__asm__(
    ".section .elf_payload,\"a\",%progbits\n"
    ".global __test_elf_start\n"
    ".global __test_elf_end\n"
    "__test_elf_start:\n"
    INCBIN_DIRECTIVE
    "__test_elf_end:\n"
    ".section .text\n"
);

#endif /* ELF_LOADER */
