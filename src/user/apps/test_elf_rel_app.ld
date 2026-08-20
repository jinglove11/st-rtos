/*
 * Linker script for the P1-6 relocation test ELF.
 * .text XIP in flash; .data/.bss carry a RAM *link* VMA (0x20080000, a
 * scratch address never dereferenced at runtime) with initial values stored
 * in the flash image (AT > FLASH). The loader copies both into RAM frames
 * and re-binds every relocation (ABS32 / MOVW / MOVT) to the frame bases.
 * .rel/.symtab sections are kept (not discarded) — the loader parses them.
 */

ENTRY(_start)

MEMORY
{
    FLASH (rx) : ORIGIN = 0x10100000, LENGTH = 0x10000
    RAM   (rw) : ORIGIN = 0x20080000, LENGTH = 0x1000
}

SECTIONS
{
    .text : {
        *(.text*)
        *(.rodata*)
    } > FLASH

    .data : AT (0x10108000) {
        *(.data*)
        *(.bss*)
        *(COMMON)
    } > RAM

    /DISCARD/ : {
        *(.comment)
        *(.ARM.attributes)
        *(.ARM.exidx*)
        *(.ARM.extab*)
    }
}
