; arch/i386/entry.asm - 32-bit protected-mode kernel entry stub
; TigxOS V1.0.58
[bits 32]

section .text
global entry
extern kmain

entry:
    cli
    mov esp, 0x90000
    and esp, 0xFFFFFFF0          ; 16-byte align
    mov ebp, esp
    cld
    call kmain
.hang:
    hlt
    jmp .hang

section .note.GNU-stack noalloc noexec nowrite progbits
