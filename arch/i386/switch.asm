; arch/i386/switch.asm - context switch + trampolines
; TigxOS 1.3.8 — ring 0 / ring 3
[bits 32]
section .text
global context_switch
global task_trampoline
global user_trampoline

; void context_switch(uintptr_t **old_rsp, uintptr_t *new_rsp)
context_switch:
    mov eax, [esp + 4]
    mov edx, [esp + 8]
    push ebp
    push ebx
    push esi
    push edi
    mov [eax], esp
    mov esp, edx
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

task_trampoline:
    sti
    ret

; Stack top: EIP, CS, EFLAGS, ESP, SS
user_trampoline:
    iret

section .note.GNU-stack noalloc noexec nowrite progbits
