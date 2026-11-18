/* arch/arch.h - Shared architecture interface (boot / paging / context switch)
 *
 * Machine-independent declarations. Implementations live under arch/<arch>/.
 * Supported: x86_64 (long mode), i386 (32-bit protected mode).
 */
#ifndef ARCH_ARCH_H
#define ARCH_ARCH_H

#include <stdint.h>
#include <stddef.h>

/* ---------- Context switch (used by process scheduler) ---------- */

/**
 * Save current stack pointer into *old_rsp, load new_rsp, restore callee-
 * saved registers, and return into the new context.
 *
 * x86_64 System V: rdi = old_rsp, rsi = new_rsp
 * i386 cdecl:      [esp+4] = old_rsp, [esp+8] = new_rsp
 */
void context_switch(uintptr_t **old_rsp, uintptr_t *new_rsp);

/**
 * First-entry trampoline for a newly created task: enables interrupts
 * then returns into the real entry point that was placed on the stack.
 */
void task_trampoline(void);

/** iret/iretq into ring 3; stack must hold a full return frame. */
void user_trampoline(void);

/* ---------- Paging primitives ---------- */

static inline void arch_invlpg(void *addr)
{
    __asm__ volatile ("invlpg (%0)" : : "r"(addr) : "memory");
}

static inline uintptr_t arch_read_cr3(void)
{
    uintptr_t val;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(val));
    return val;
}

static inline void arch_write_cr3(uintptr_t val)
{
    __asm__ volatile ("mov %0, %%cr3" : : "r"(val) : "memory");
}

static inline uintptr_t arch_read_cr2(void)
{
    uintptr_t val;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(val));
    return val;
}

/* ---------- Boot / early memory map (set by boot.asm) ---------- */

#define ARCH_BOOT_MEMMAP_COUNT   ((volatile uint32_t *)0x9000)
#define ARCH_BOOT_MEMMAP_DATA    ((void *)0x9004)
#define ARCH_BOOT_PAGE_ROOT      ((void *)0x1000)   /* PML4 (64) or PD (32) */

/* Kernel load address / entry (matches linker.ld and boot.asm) */
#define ARCH_KERNEL_OFFSET       0x10000UL

/* ---------- Interrupt / IDT ---------- */

struct idt_ptr;   /* full def in interrupt.h */

void load_idt(struct idt_ptr *ptr);

/* Stub tables defined in arch/<arch>/interrupt.asm */
extern uintptr_t isr_stub_table[];
extern uintptr_t irq_stub_table[];
extern void syscall_stub(void);

/* Code segment selector used in IDT gates */
/* After gdt_init(): kernel code is always selector 0x08 on both arches. */
#define ARCH_KERNEL_CS  0x08
#define ARCH_KERNEL_DS  0x10

#endif /* ARCH_ARCH_H */
