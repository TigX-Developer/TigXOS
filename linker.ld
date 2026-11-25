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

    /* BSS lives at 1 MB, above the 0x9FC00 EBDA / 0xA0000 VGA hole. Frames below
       RESERVED_END (8 MB, memory.c) are never handed out, so this is safe. */
    . = 0x100000;
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data
}
