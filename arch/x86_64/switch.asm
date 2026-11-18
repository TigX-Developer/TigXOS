; arch/x86_64/switch.asm - context switch + trampolines
; TigxOS 1.1.12 — ring 0 / ring 3
[bits 64]
[default rel]
section .text
global context_switch
global task_trampoline
global user_trampoline

context_switch:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov [rdi], rsp
    mov rsp, rsi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; First entry of a new kernel task: enable interrupts, then ret into entry.
task_trampoline:
    sti
    ret

; First entry / resume into ring 3.  Stack top must be a full iretq frame:
;   RIP, CS, RFLAGS, RSP, SS
user_trampoline:
    iretq

section .note.GNU-stack noalloc noexec nowrite progbits
