; arch/x86_64/interrupt.asm — ISR + IRQ stubs (TigxOS 1.2.0)
; Stack alignment: SysV requires RSP % 16 == 0 immediately before CALL.
; CPL0 vs CPL3 frames differ by 16 bytes, so we save the real frame pointer
; and force-align a scratch RSP for the C call, then restore.
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

%macro ISR_NOERR 1
isr_%1:
    push 0
    push %1
    jmp isr_common_stub
%endmacro

%macro ISR_ERR 1
isr_%1:
    push %1
    jmp isr_common_stub
%endmacro

%macro IRQ 2
irq_%1:
    push 0
    push %2
    jmp irq_common_stub
%endmacro

; Common path: save GPRs, align for C ABI, call, restore, iretq.
; Frame layout matches struct interrupt_frame in interrupt.h.
isr_common_stub:
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

    mov rdi, rsp                ; arg0 = interrupt_frame *

    ; Align stack for the CALL without using the fragile "pop rsp" pattern:
    ; keep the true frame in RDI; build an aligned scratch stack in RAX/RSP.
    mov rax, rsp
    and rsp, -16                ; 16-byte aligned
    sub rsp, 8                  ; room so CALL's return-addr push lands aligned
    push rax                    ; [aligned_rsp] = original RSP (for restore)
    call isr_handler
    pop rsp                     ; restore original (pre-align) RSP

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

    add rsp, 16                 ; int_num + error_code
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
    sub rsp, 8
    push rax
    call irq_handler
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

; int 0x80 from ring 3 — CPU already pushed SS/RSP/RFLAGS/CS/RIP
syscall_stub:
    push 0
    push 0x80
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
