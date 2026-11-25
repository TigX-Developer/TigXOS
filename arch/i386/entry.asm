; arch/i386/entry.asm - 32-bit protected-mode kernel entry stub
; TigxOS 1.3.8
[bits 32]

section .text
global entry
extern kmain

entry:
    cli
    mov esp, boot_stack_top      ; reserved .bss stack (see x86_64 entry.asm)
    and esp, 0xFFFFFFF0          ; 16-byte align
    mov ebp, esp
    cld
    call kmain
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
