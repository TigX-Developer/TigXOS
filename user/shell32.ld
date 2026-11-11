ENTRY(_start)
PHDRS {
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
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
