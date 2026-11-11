ENTRY(_start)
PHDRS {
    text PT_LOAD FLAGS(5); /* PF_R|PF_X = 4|1 */
    data PT_LOAD FLAGS(6); /* PF_R|PF_W = 4|2 */
}
SECTIONS
{
    . = 0x400000;
    .text : {
        *crt0.o(.text .text.*)
        *(.text .text.*)
    } : text
    .rodata : { *(.rodata .rodata.*) } : text
    . = ALIGN(4096);
    .data : { *(.data .data.*) } : data
    .bss : { *(.bss .bss.*) *(COMMON) } : data
}
