; arch/x86_64/interrupt.asm - ISR + IRQ stubs with stack alignment
[bits 64]
[default rel]

section .text
global load_idt
global isr_stub_table
global irq_stub_table
global syscall_stub
extern isr_handler
extern irq_handler

load_idt:
    lidt [rdi]
    ret

; ---------- Exception macros ----------
%macro ISR_NOERR 1
isr_%1:
    push 0          ; dummy error code
    push %1         ; interrupt number
    jmp isr_common_stub
%endmacro

%macro ISR_ERR 1
isr_%1:
    ; CPU already pushed error code
    push %1
    jmp isr_common_stub
%endmacro

; ---------- IRQ macros (no error code) ----------
%macro IRQ 2
irq_%1:
    push 0          ; dummy error code
    push %2         ; vector number (32 + irq)
    jmp irq_common_stub
%endmacro

isr_common_stub:
    ; Save all GPRs
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp            ; pass frame pointer (struct interrupt_frame *)

    ; SysV ABI: RSP % 16 == 0 immediately before CALL.
    ; CPL0 vs CPL3 frames differ by 16 bytes (SS/RSP), so a fixed sub is wrong.
    ; Save original RSP, force-align, call, then restore.
    mov rax, rsp
    and rsp, -16
    push rax                ; keep original rsp at [rsp] (also re-misaligns by 8)
    push rax                ; pad to 16-byte alignment for the CALL
    call isr_handler
    pop rax                 ; discard pad
    pop rsp                 ; restore pre-align rsp

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16             ; remove int_num + error_code
    iretq

irq_common_stub:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp

    mov rax, rsp
    and rsp, -16
    push rax
    push rax
    call irq_handler
    pop rax
    pop rsp

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16
    iretq

; === Exception stubs 0-31 ===
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30
ISR_NOERR 31

; === IRQ stubs 0-15 → vectors 32-47 ===
IRQ 0,  32
IRQ 1,  33
IRQ 2,  34
IRQ 3,  35
IRQ 4,  36
IRQ 5,  37
IRQ 6,  38
IRQ 7,  39
IRQ 8,  40
IRQ 9,  41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47


; === Syscall stub (int 0x80 from ring 3) → isr_common_stub ===
; CPU already pushed SS,RSP,RFLAGS,CS,RIP (privilege change).
syscall_stub:
    push 0              ; dummy error code
    push 0x80           ; vector
    jmp isr_common_stub

section .data
align 8
isr_stub_table:
%assign i 0
%rep 32
    dq isr_%+i
%assign i i+1
%endrep

align 8
irq_stub_table:
%assign i 0
%rep 16
    dq irq_%+i
%assign i i+1
%endrep

section .note.GNU-stack noalloc noexec nowrite progbits
