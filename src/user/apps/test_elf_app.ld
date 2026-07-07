/*
 * Linker script for the test ELF app — linked to the flash FS region.
 * The ELF's .text executes via XIP (Region 0 covers all flash).
 * .data is placed after .text (also in flash); the loader copies it to RAM.
 *
 * VMA (virtual/link address) for .data must be in the flash range so the
 * loader can find the initial values. The loader allocates RAM and remaps
 * via MPU at runtime.
 */

ENTRY(_start)

MEMORY
{
    FLASH (rx) : ORIGIN = 0x10100000, LENGTH = 0x10000  /* 64 KiB in FS region */
}

SECTIONS
{
    .text : {
        *(.text*)
        *(.rodata*)
    } > FLASH

    .data : {
        *(.data*)
        *(.bss*)
        *(COMMON)
    } > FLASH

    /DISCARD/ : {
        *(.comment)
        *(.ARM.attributes)
        *(.ARM.exidx*)
        *(.ARM.extab*)
    }
}
