; arch/x86_64/entry.asm - 64-bit kernel entry stub
[bits 64]
[default rel]

section .text
global entry
extern kmain

entry:
    ; Make sure interrupts are off
    cli

    ; Set up a proper stack (16-byte aligned). 1.2.12-update1: the boot stack
    ; is a reserved .bss object. It used to live at a fixed 0x90000, which the
    ; kernel's own .bss (dmesg log buffer) grew over, so log text overwrote the stack.
    lea rsp, [boot_stack_top]
    and rsp, -16          ; force 16-byte alignment
    mov rbp, rsp

    ; Clear direction flag (required by ABI)
    cld

    ; Call the C kernel
    call kmain

    ; If kmain ever returns, halt
.hang:
    hlt
    jmp .hang

section .bss
align 16
global boot_stack_bottom
global boot_stack_top
boot_stack_bottom:
    resb 16384
boot_stack_top:

section .note.GNU-stack noalloc noexec nowrite progbits
