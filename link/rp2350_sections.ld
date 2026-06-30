SECTIONS
{
    .test_modules :
    {
        . = ALIGN(4);
        __test_modules_start = .;
        KEEP(*(.test_modules))
        __test_modules_end = .;
        . = ALIGN(4);
    }
}
INSERT AFTER .rodata;

