; arch/x86_64/entry.asm - 64-bit kernel entry stub
[bits 64]
[default rel]

section .text
global entry
extern kmain

entry:
    ; Make sure interrupts are off
    cli

    ; Set up a proper stack (16-byte aligned)
    mov rsp, 0x90000
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

section .note.GNU-stack noalloc noexec nowrite progbits
