/**
 * @file test_elf_app.c
 * @brief #6 ELF_LOADER — a freestanding program compiled into a separate ELF
 *
 * This is NOT compiled with the kernel. It is cross-compiled separately
 * (arm-none-eabi-gcc -ffreestanding -nostdlib -mthumb) into a standalone ELF
 * linked to the flash FS region (0x10100000), then embedded into the kernel
 * image via .incbin and loaded by elf_load() at runtime.
 *
 * The program writes to a .data global (proving .data is mapped RW), then
 * calls sys_task_exit(0x600D) via inline SVC (SYSCALL_TASK_EXIT = 2).
 */

/* Entry point. arg is passed as R0 (task_create arg — NULL from the loader). */
void _start(void *arg);

/* Minimal _start: just return. No globals, no data access — pure code.
 * The initial LR (set by task_create_user) points to user_task_exit_handler
 * which does svc #1 → SYSCALL_TASK_EXIT. This proves XIP execution works. */
void _start(void *arg) {
    (void)arg;
}
