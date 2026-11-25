; arch/i386/interrupt.asm — 32-bit ISR + IRQ stubs (TigxOS 1.3.8)
; pusha + segment pushes give a fixed frame; we align ESP to 16 before the
; C call so the SysV i386 ABI (and any SSE spill) is satisfied.
[bits 32]

section .text
global load_idt
global isr_stub_table
global irq_stub_table
global syscall_stub
extern isr_handler
extern irq_handler

; void load_idt(struct idt_ptr *ptr)  — cdecl: [esp+4] = ptr
load_idt:
    mov eax, [esp + 4]
    lidt [eax]
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

isr_common_stub:
    pusha                        ; eax,ecx,edx,ebx,esp,ebp,esi,edi
    push ds
    push es
    push fs
    push gs

    mov ax, 0x10                 ; kernel data selector
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov eax, esp                 ; frame pointer
    and esp, 0xFFFFFFF0          ; 16-byte align for C call
    sub esp, 12                  ; + push below → 16-aligned after call push
    push eax                     ; arg0 = interrupt_frame *
    call isr_handler
    pop eax                      ; discard arg
    mov esp, eax                 ; restore real frame ESP

    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8                   ; int_num + error_code
    iret

irq_common_stub:
    pusha
    push ds
    push es
    push fs
    push gs

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov eax, esp
    and esp, 0xFFFFFFF0
    sub esp, 12
    push eax
    call irq_handler
    pop eax
    mov esp, eax

    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8
    iret

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

; === Syscall stub (int 0x80 from ring 3) ===
syscall_stub:
    push 0
    push 0x80
    jmp isr_common_stub

section .data
align 4
isr_stub_table:
    dd isr_0,  isr_1,  isr_2,  isr_3,  isr_4,  isr_5,  isr_6,  isr_7
    dd isr_8,  isr_9,  isr_10, isr_11, isr_12, isr_13, isr_14, isr_15
    dd isr_16, isr_17, isr_18, isr_19, isr_20, isr_21, isr_22, isr_23
    dd isr_24, isr_25, isr_26, isr_27, isr_28, isr_29, isr_30, isr_31

irq_stub_table:
    dd irq_0,  irq_1,  irq_2,  irq_3,  irq_4,  irq_5,  irq_6,  irq_7
    dd irq_8,  irq_9,  irq_10, irq_11, irq_12, irq_13, irq_14, irq_15

section .note.GNU-stack noalloc noexec nowrite progbits
