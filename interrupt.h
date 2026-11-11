// interrupt.h
#ifndef INTERRUPT_H
#define INTERRUPT_H

#include <stdint.h>

#if defined(__i386__)

/* Classic 32-bit IDT gate (8 bytes) */
struct idt_entry {
    uint16_t base_low;
    uint16_t selector;
    uint8_t  zero;
    uint8_t  flags;
    uint16_t base_high;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

/*
 * Matches arch/i386/interrupt.asm stack layout at isr/irq_handler entry:
 *   gs,fs,es,ds, edi,esi,ebp,esp,ebx,edx,ecx,eax, int_no, err, eip,cs,eflags
 */
struct interrupt_frame {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp;
    uint32_t ebx, edx, ecx, eax;
    uint32_t interrupt_number;
    uint32_t error_code;
    uint32_t eip;
    uint32_t cs;
    uint32_t eflags;
} __attribute__((packed));

#else /* __x86_64__ */

struct idt_entry {
    uint16_t base_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t flags;
    uint16_t base_mid;
    uint32_t base_high;
    uint32_t zero;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* Register state for exception handlers (CPL0 → CPL0, no SS/RSP pushed) */
struct interrupt_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t interrupt_number;
    uint64_t error_code;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
} __attribute__((packed));

#endif

typedef void (*isr_handler_t)(struct interrupt_frame*);

void idt_init(void);
void idt_load(void);
void register_interrupt_handler(uint8_t vector, isr_handler_t handler);
void set_idt_gate(uint8_t vector, uintptr_t base, uint16_t selector, uint8_t flags);

void pic_remap(void);
void pic_enable_irq(uint8_t irq);
void pic_disable_irq(uint8_t irq);
void pic_send_eoi(uint8_t irq);

void isr_handler(struct interrupt_frame *frame);
void irq_handler(struct interrupt_frame *frame);

void panic_render(const char *reason, struct interrupt_frame *frame) __attribute__((noreturn));
void kernel_panic(const char *fmt, ...) __attribute__((noreturn));

#endif
