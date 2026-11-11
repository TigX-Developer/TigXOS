/* arch/i386/linker.ld — 32-bit protected-mode kernel layout */
ENTRY(entry)

PHDRS
{
    text PT_LOAD FLAGS(5); /* R+E */
    data PT_LOAD FLAGS(6); /* R+W */
}

SECTIONS
{
    . = 0x10000;

    .text : {
        *(.text .text.*)
    } :text

    .rodata : {
        *(.rodata .rodata.*)
    } :text

    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data
}
