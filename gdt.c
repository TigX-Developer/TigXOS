/* gdt.c — GDT + TSS for ring0/ring3 (TigxOS 1.2.0) */


#include "gdt.h"
#include "console.h"
#include "string.h"
#include <stdint.h>

#if defined(__i386__)

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  gran;
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

struct tss_entry {
    uint32_t prev_tss;
    uint32_t esp0;
    uint32_t ss0;
    uint32_t esp1, ss1, esp2, ss2;
    uint32_t cr3;
    uint32_t eip, eflags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed));

static struct gdt_entry gdt[7];
static struct tss_entry tss;
static struct gdt_ptr gp;

static void gdt_set(int idx, uint32_t base, uint32_t limit,
                    uint8_t access, uint8_t gran) {
    gdt[idx].base_low  = base & 0xFFFF;
    gdt[idx].base_mid  = (base >> 16) & 0xFF;
    gdt[idx].base_high = (base >> 24) & 0xFF;
    gdt[idx].limit_low = limit & 0xFFFF;
    gdt[idx].gran = (limit >> 16) & 0x0F;
    gdt[idx].gran |= gran & 0xF0;
    gdt[idx].access = access;
}

void tss_set_kernel_stack(uintptr_t stack_top) {
    tss.esp0 = (uint32_t)stack_top;
    tss.ss0  = GDT_KERNEL_DS;
}

void gdt_init(void) {
    memset(gdt, 0, sizeof(gdt));
    memset(&tss, 0, sizeof(tss));

    gdt_set(0, 0, 0, 0, 0);                      /* null */
    gdt_set(1, 0, 0xFFFFF, 0x9A, 0xCF);          /* kernel code 0x08 */
    gdt_set(2, 0, 0xFFFFF, 0x92, 0xCF);          /* kernel data 0x10 */
    gdt_set(3, 0, 0xFFFFF, 0xF2, 0xCF);          /* user data   0x18 DPL3 */
    gdt_set(4, 0, 0xFFFFF, 0xFA, 0xCF);          /* user code   0x20 DPL3 */

    /* TSS descriptor at index 5 → selector 0x28 */
    uint32_t tb = (uint32_t)(uintptr_t)&tss;
    gdt_set(5, tb, sizeof(tss) - 1, 0x89, 0x00);

    tss.ss0 = GDT_KERNEL_DS;
    tss.esp0 = 0x90000; /* temporary until first process */
    tss.iomap_base = sizeof(tss);

    gp.limit = sizeof(gdt) - 1;
    gp.base  = (uint32_t)(uintptr_t)gdt;

    __asm__ volatile("lgdt %0" : : "m"(gp) : "memory");

    /* Reload data segments + far jump to reload CS */
    __asm__ volatile(
        "mov %0, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "mov %%ax, %%ss\n"
        "ljmp %1, $1f\n"
        "1:\n"
        :
        : "i"(GDT_KERNEL_DS), "i"(GDT_KERNEL_CS)
        : "ax", "memory");

    __asm__ volatile("ltr %0" : : "r"((uint16_t)GDT_TSS) : "memory");

    kprintf("GDT+TSS loaded (i386, user CS=0x%02x DS=0x%02x)\n",
            USER_CS, USER_DS);
}

#else /* __x86_64__ */

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  gran;
    uint8_t  base_high;
} __attribute__((packed));

/* 64-bit TSS descriptor is 16 bytes */
struct gdt_tss_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  limit_high;
    uint8_t  base_high;
    uint32_t base_upper;
    uint32_t reserved;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

struct tss_entry {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1, ist2, ist3, ist4, ist5, ist6, ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

static struct {
    struct gdt_entry null;
    struct gdt_entry kcode;
    struct gdt_entry kdata;
    struct gdt_entry udata;
    struct gdt_entry ucode;
    struct gdt_tss_entry tss_desc;
} __attribute__((packed)) gdt;

static struct tss_entry tss;
static struct gdt_ptr gp;

static void set_entry(struct gdt_entry *e, uint8_t access, uint8_t gran) {
    e->limit_low = 0;
    e->base_low = 0;
    e->base_mid = 0;
    e->access = access;
    e->gran = gran;
    e->base_high = 0;
}

void tss_set_kernel_stack(uintptr_t stack_top) {
    tss.rsp0 = stack_top;
}

void gdt_init(void) {
    memset(&gdt, 0, sizeof(gdt));
    memset(&tss, 0, sizeof(tss));

    /* 0x00 null */
    set_entry(&gdt.null, 0, 0);
    /* 0x08 kernel code: L=1, D/B=0, P=1, DPL=0, type=0xA (exec/read) */
    set_entry(&gdt.kcode, 0x9A, 0x20);
    /* 0x10 kernel data */
    set_entry(&gdt.kdata, 0x92, 0x00);
    /* 0x18 user data DPL=3 */
    set_entry(&gdt.udata, 0xF2, 0x00);
    /* 0x20 user code L=1 DPL=3 */
    set_entry(&gdt.ucode, 0xFA, 0x20);

    /* 0x28 TSS (16-byte descriptor) */
    uint64_t tb = (uint64_t)(uintptr_t)&tss;
    uint32_t tlimit = sizeof(tss) - 1;
    gdt.tss_desc.limit_low  = tlimit & 0xFFFF;
    gdt.tss_desc.base_low   = tb & 0xFFFF;
    gdt.tss_desc.base_mid   = (tb >> 16) & 0xFF;
    gdt.tss_desc.access     = 0x89; /* present, type=9 (available TSS) */
    gdt.tss_desc.limit_high = (tlimit >> 16) & 0x0F;
    gdt.tss_desc.base_high  = (tb >> 24) & 0xFF;
    gdt.tss_desc.base_upper = (uint32_t)(tb >> 32);
    gdt.tss_desc.reserved   = 0;

    tss.rsp0 = 0x90000;
    tss.iomap_base = sizeof(tss);

    gp.limit = sizeof(gdt) - 1;
    gp.base  = (uint64_t)(uintptr_t)&gdt;

    __asm__ volatile("lgdt %0" : : "m"(gp) : "memory");

    /* Reload CS via retfq and data segments */
    __asm__ volatile(
        "mov %0, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%ss\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "pushq %1\n"
        "lea 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        :
        : "i"(GDT_KERNEL_DS), "i"(GDT_KERNEL_CS)
        : "rax", "memory");

    __asm__ volatile("ltr %0" : : "r"((uint16_t)GDT_TSS) : "memory");

    kprintf("GDT+TSS loaded (x86_64, user CS=0x%02x DS=0x%02x)\n",
            USER_CS, USER_DS);
}

#endif
