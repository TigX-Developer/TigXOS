ENTRY(_start)
PHDRS {
    text PT_LOAD FLAGS(5); /* PF_R|PF_X */
    data PT_LOAD FLAGS(6); /* PF_R|PF_W */
}
SECTIONS
{
    . = 0x400000;
    .text : {
        *crt0.o(.text .text.*)
        *(.text .text.*)
    } : text
    .rodata : {
        *(.rodata .rodata.*)
        *(.note.gnu.build-id)
        *(.note.*)
    } : text
    /* Force data segment onto the next page so the two PT_LOAD
       headers never share the same VirtAddr (ld on i386 otherwise
       collapses them when .data is empty / tiny). */
    . = ALIGN(4096);
    .data : {
        *(.data .data.*)
    } : data
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } : data
}
