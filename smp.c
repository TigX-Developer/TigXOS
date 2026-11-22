/* smp.c — SMP bring-up + 1.2.9 IPI/TLB/balance (TigxOS)
 *
 * Robustness:
 *  - Every early-exit path leaves smp_online = 1 and never touches the
 *    trampoline or ICR.
 *  - Mailbox wait is bounded; a silent AP is logged and skipped.
 *  - Trampoline is identity-mapped low RAM that the PMM never hands out.
 *  - i386 / no-LAPIC paths are pure UP — no speculative INIT/SIPI.
 */

#include "smp.h"
#include "acpi.h"
#include "atomic.h"
#include "console.h"
#include "lapic.h"
#include "log.h"
#include "memory.h"
#include "arch/arch.h"
#include "process.h"
#include "string.h"
#include <stdint.h>

#define TRAMPOLINE_PHYS  0x8000
#define MAILBOX_PHYS     0x8FF0
#define TRAMPOLINE_SIZE  0x200

/* Logical id → APIC id; -1 = unused */
static int           smp_logical[NCPU_MAX];
static int           smp_online   = 1;
static int           smp_madt_n   = 1;
static int           smp_ap_ok    = 0;
static volatile int  ap_ready[NCPU_MAX];

/* 1.2.9 IPI / balance state */
static volatile uint64_t ipi_resched_total;
static volatile uint64_t ipi_tlb_total;
static volatile uint64_t balance_steals;
static volatile int      need_resched[NCPU_MAX];
static volatile int      rq_depth[NCPU_MAX];
static volatile uintptr_t tlb_shoot_page;   /* 0 = full flush */
static volatile uint32_t  tlb_shoot_gen;
static volatile uint32_t  tlb_ack_gen[NCPU_MAX];



/* AP boot block at 0x8F00 (identity-mapped, below 1 MB). */
#define AP_BOOT_PHYS     0x8F00
struct ap_boot_info {
    volatile uint32_t ready;   /* mailbox: incremented by trampoline */
    uint32_t          _pad0;
    uint64_t          cr3;     /* PML4 physical */
    uint64_t          entry;   /* ap_main */
    uint64_t          stack;   /* top of AP stack */
    uint32_t          cpu_id;  /* logical id assigned by BSP */
    uint32_t          _pad1;
};
#define AP_BOOT ((struct ap_boot_info *)(uintptr_t)AP_BOOT_PHYS)

/* Per-AP stacks (16 KiB each), allocated once on first smp_init. */
#define AP_STACK_SIZE  0x4000
static uint8_t *ap_stacks[NCPU_MAX];

#if defined(__x86_64__)
/* 64-bit AP entry — runs after trampoline enters long mode. */
static void ap_main(void) {
    struct ap_boot_info *b = AP_BOOT;
    int id = (int)b->cpu_id;
    if (id < 0 || id >= NCPU_MAX)
        id = 0;

    if (lapic_present())
        lapic_eoi();

    process_bind_ap(id);
    ap_ready[id] = 1;
    /* smp_ap_ok counted once here */
    {
        static int counted[NCPU_MAX];
        if (id < NCPU_MAX && !counted[id]) {
            counted[id] = 1;
            atomic_inc((volatile int *)&smp_ap_ok);
        }
    }

    pr_info("smp: CPU %d (APIC %u) online — scheduler idle loop\n", id, lapic_id());

    /* Cross-core scheduling: schedule() requires IF=0 (same as BSP). */
    for (;;) {
        __asm__ volatile("cli" ::: "memory");
        if (smp_need_resched())
            smp_clear_need_resched();
        if (scheduler_is_enabled())
            schedule();
        __asm__ volatile("sti; hlt" ::: "memory");
    }
}
#else
static void ap_main(void) {
    for (;;)
        __asm__ volatile("hlt");
}
#endif

/*
 * Trampoline at 0x8000:
 *  real mode → protected mode → long mode → call ap_main
 *
 * Layout:
 *  [0x00]  16-bit SIPI entry
 *  [0x60]  GDTR
 *  [0x68]  GDT (null, code32, data32, code64, data64)
 *  [0x100] 32-bit: segments, load CR3/PAE/LME/PG, far jump to 64-bit
 *  [0x180] 64-bit: stack, call entry, hlt
 */
static void install_trampoline(void) {
    uint8_t *t = (uint8_t *)(uintptr_t)TRAMPOLINE_PHYS;
    memset(t, 0, TRAMPOLINE_SIZE);

    /* --- 16-bit entry --- */
    int i = 0;
    t[i++] = 0xFA;                         /* cli */
    t[i++] = 0x31; t[i++] = 0xC0;         /* xor ax,ax */
    t[i++] = 0x8E; t[i++] = 0xD8;         /* mov ds,ax */
    /* lgdt [0x8060] */
    t[i++] = 0x0F; t[i++] = 0x01; t[i++] = 0x16;
    t[i++] = 0x60; t[i++] = 0x80;
    /* mov eax,cr0; or al,1; mov cr0,eax */
    t[i++] = 0x0F; t[i++] = 0x20; t[i++] = 0xC0;
    t[i++] = 0x0C; t[i++] = 0x01;
    t[i++] = 0x0F; t[i++] = 0x22; t[i++] = 0xC0;
    /* jmp far 0x08:0x8100 */
    t[i++] = 0x66; t[i++] = 0xEA;
    t[i++] = 0x00; t[i++] = 0x81; t[i++] = 0x00; t[i++] = 0x00;
    t[i++] = 0x08; t[i++] = 0x00;

    /* --- GDT at 0x8068 --- */
    uint8_t *gdt = t + 0x68;
    memset(gdt, 0, 8); /* null */
    /* 0x08 code32 */
    gdt[8]=0xFF; gdt[9]=0xFF; gdt[10]=0; gdt[11]=0;
    gdt[12]=0; gdt[13]=0x9A; gdt[14]=0xCF; gdt[15]=0;
    /* 0x10 data32 */
    gdt[16]=0xFF; gdt[17]=0xFF; gdt[18]=0; gdt[19]=0;
    gdt[20]=0; gdt[21]=0x92; gdt[22]=0xCF; gdt[23]=0;
    /* 0x18 code64: L=1 D=0, type 0x9A, gran 0xAF */
    gdt[24]=0x00; gdt[25]=0x00; gdt[26]=0; gdt[27]=0;
    gdt[28]=0; gdt[29]=0x9A; gdt[30]=0xA0; gdt[31]=0;
    /* 0x20 data64 */
    gdt[32]=0x00; gdt[33]=0x00; gdt[34]=0; gdt[35]=0;
    gdt[36]=0; gdt[37]=0x92; gdt[38]=0xA0; gdt[39]=0;

    /* GDTR at 0x8060 */
    uint8_t *gdtr = t + 0x60;
    gdtr[0] = 39; gdtr[1] = 0; /* limit */
    uint32_t gdt_phys = TRAMPOLINE_PHYS + 0x68;
    gdtr[2] = (uint8_t)(gdt_phys);
    gdtr[3] = (uint8_t)(gdt_phys >> 8);
    gdtr[4] = (uint8_t)(gdt_phys >> 16);
    gdtr[5] = (uint8_t)(gdt_phys >> 24);

    /* --- 32-bit stub at 0x8100 --- */
    uint8_t *p = t + 0x100;
    i = 0;
    /* mov ax,0x10; mov ds/es/ss,ax */
    p[i++]=0x66; p[i++]=0xB8; p[i++]=0x10; p[i++]=0x00;
    p[i++]=0x8E; p[i++]=0xD8;
    p[i++]=0x8E; p[i++]=0xC0;
    p[i++]=0x8E; p[i++]=0xD0;

    /* lock incl [0x8F00] (ready mailbox) */
    p[i++]=0xF0; p[i++]=0xFF; p[i++]=0x05;
    p[i++]=0x00; p[i++]=0x8F; p[i++]=0x00; p[i++]=0x00;

#if defined(__x86_64__)
    /* mov eax, [AP_BOOT.cr3] @ 0x8F08 */
    p[i++]=0xA1;
    p[i++]=0x08; p[i++]=0x8F; p[i++]=0x00; p[i++]=0x00;
    /* mov cr3, eax */
    p[i++]=0x0F; p[i++]=0x22; p[i++]=0xD8;

    /* mov eax, cr4; or eax, 0x20; mov cr4, eax  (PAE) */
    p[i++]=0x0F; p[i++]=0x20; p[i++]=0xE0;
    p[i++]=0x83; p[i++]=0xC8; p[i++]=0x20;
    p[i++]=0x0F; p[i++]=0x22; p[i++]=0xE0;

    /* mov ecx, 0xC0000080; rdmsr; or eax, 0x100; wrmsr (LME) */
    p[i++]=0xB9; p[i++]=0x80; p[i++]=0x00; p[i++]=0x00; p[i++]=0xC0;
    p[i++]=0x0F; p[i++]=0x32;
    p[i++]=0x0D; p[i++]=0x00; p[i++]=0x01; p[i++]=0x00; p[i++]=0x00;
    p[i++]=0x0F; p[i++]=0x30;

    /* mov eax, cr0; or eax, 0x80000000; mov cr0, eax (PG) */
    p[i++]=0x0F; p[i++]=0x20; p[i++]=0xC0;
    p[i++]=0x0D; p[i++]=0x00; p[i++]=0x00; p[i++]=0x00; p[i++]=0x80;
    p[i++]=0x0F; p[i++]=0x22; p[i++]=0xC0;

    /* jmp far 0x18:0x8180 (64-bit CS) */
    p[i++]=0xEA;
    p[i++]=0x80; p[i++]=0x81; p[i++]=0x00; p[i++]=0x00;
    p[i++]=0x18; p[i++]=0x00;

    /* --- 64-bit stub at 0x8180 --- */
    uint8_t *q = t + 0x180;
    int j = 0;
    /* mov rsp, [0x8F10] stack */
    q[j++]=0x48; q[j++]=0x8B; q[j++]=0x24; q[j++]=0x25;
    q[j++]=0x10; q[j++]=0x8F; q[j++]=0x00; q[j++]=0x00;
    /* mov rax, [0x8F08] — wait entry is at 0x8F10? struct:
       ready@0, pad@4, cr3@8, entry@16, stack@24
    */
    /* Fix: entry at 0x8F10, stack at 0x8F18 */
    /* reload rsp from 0x8F18 */
    j = 0;
    q[j++]=0x48; q[j++]=0x8B; q[j++]=0x24; q[j++]=0x25;
    q[j++]=0x18; q[j++]=0x8F; q[j++]=0x00; q[j++]=0x00;
    /* and rsp, ~0xF */
    q[j++]=0x48; q[j++]=0x83; q[j++]=0xE4; q[j++]=0xF0;
    /* mov rax, [0x8F10] entry */
    q[j++]=0x48; q[j++]=0xA1;
    q[j++]=0x10; q[j++]=0x8F; q[j++]=0x00; q[j++]=0x00;
    q[j++]=0x00; q[j++]=0x00; q[j++]=0x00; q[j++]=0x00;
    /* call rax */
    q[j++]=0xFF; q[j++]=0xD0;
    /* hlt; jmp $ */
    q[j++]=0xF4;
    q[j++]=0xEB; q[j++]=0xFD;
#else
    /* i386: just hlt loop after mailbox */
    p[i++]=0xFB; /* sti */
    p[i++]=0xF4;
    p[i++]=0xEB; p[i++]=0xFD;
#endif

    __asm__ volatile("mfence" ::: "memory");
}


void smp_init(void) {
    for (int i = 0; i < NCPU_MAX; i++) {
        smp_logical[i] = -1;
        ap_ready[i] = 0;
    }
    smp_logical[0] = 0;
    smp_online = 1;
    smp_madt_n = 1;
    smp_ap_ok = 0;

    const struct acpi_info *ai = acpi_get();
    if (!ai || !ai->present) {
        pr_info("smp: ACPI not present — single CPU\n");
        return;
    }

    smp_madt_n = ai->cpu_count > 0 ? ai->cpu_count : 1;
    if (smp_madt_n > NCPU_MAX)
        smp_madt_n = NCPU_MAX;

    if (ai->cpu_count <= 1) {
        pr_info("smp: single CPU (MADT reports %d)\n", ai->cpu_count);
        if (ai->cpu_count == 1 && ai->cpus[0].enabled)
            smp_logical[0] = (int)ai->cpus[0].apic_id;
        return;
    }

    if (!lapic_present()) {
        pr_warn("smp: LAPIC not available — staying UP\n");
        return;
    }

    if (TRAMPOLINE_PHYS < 0x1000 || TRAMPOLINE_PHYS + TRAMPOLINE_SIZE > 0x100000) {
        pr_warn("smp: trampoline address out of range — aborting\n");
        return;
    }

    /* Record MADT order → APIC id */
    int n = smp_madt_n;
    int bsp_found = 0;
    for (int i = 0; i < n; i++) {
        smp_logical[i] = (int)ai->cpus[i].apic_id;
        if (ai->cpus[i].is_bsp)
            bsp_found = 1;
        pr_info("smp: MADT CPU[%d] APIC=%u enabled=%u bsp=%u\n",
                i, ai->cpus[i].apic_id, ai->cpus[i].enabled, ai->cpus[i].is_bsp);
    }
    if (!bsp_found)
        pr_warn("smp: no BSP flagged in MADT — proceeding anyway\n");

    /* Prepare AP boot block + stacks */
    struct ap_boot_info *boot = AP_BOOT;
    memset((void *)boot, 0, sizeof(*boot));
#if defined(__x86_64__)
    boot->cr3 = (uint64_t)(uintptr_t)ARCH_BOOT_PAGE_ROOT;
    boot->entry = (uint64_t)(uintptr_t)ap_main;
#endif
    __asm__ volatile("mfence" ::: "memory");

    install_trampoline();

    int started = 1; /* BSP */
    int attempted = 0;
    int logical = 1; /* next logical id for AP */
    for (int i = 0; i < n; i++) {
        if (ai->cpus[i].is_bsp || !ai->cpus[i].enabled)
            continue;
        uint8_t apic = ai->cpus[i].apic_id;
        if (logical >= NCPU_MAX)
            break;
        attempted++;

        /* Stack for this AP */
        if (!ap_stacks[logical]) {
            void *frames = pmm_alloc_frames(AP_STACK_SIZE / PAGE_SIZE);
            if (!frames) {
                pr_warn("smp: no stack for AP logical %d\n", logical);
                continue;
            }
            ap_stacks[logical] = (uint8_t *)frames;
        }
        boot->stack = (uint64_t)(uintptr_t)(ap_stacks[logical] + AP_STACK_SIZE);
        boot->cpu_id = (uint32_t)logical;
        smp_logical[logical] = (int)apic;
        process_prepare_ap_idle(logical);
        ap_ready[logical] = 0;
        uint32_t before = boot->ready;
        __asm__ volatile("mfence" ::: "memory");

        pr_info("smp: starting APIC %u as CPU %d ...\n", apic, logical);
        lapic_start_ap(apic, TRAMPOLINE_PHYS >> 12);

        int ok = 0;
        for (int t = 0; t < 1000000; t++) {
            if (boot->ready > before && ap_ready[logical]) {
                ok = 1;
                break;
            }
            if (boot->ready > before && t > 100000) {
                /* trampoline ran; wait a bit more for ap_main */
                ok = ap_ready[logical];
                if (ok) break;
            }
            __asm__ volatile("pause");
        }
        if (ok || boot->ready > before) {
            started++;
            if (ok)
                pr_info("smp: CPU %d APIC %u in long mode (ready=%u)\n",
                        logical, apic, boot->ready);
            else
                pr_info("smp: APIC %u trampoline ok (C entry pending, ready=%u)\n",
                        apic, boot->ready);
            logical++;
        } else {
            pr_warn("smp: APIC %u did not respond\n", apic);
        }
    }

    smp_online = started;
    pr_info("smp: %d CPU(s) online (%d AP attempted)\n", smp_online, attempted);

    extern void smp_mark_online(int count);
    smp_mark_online(smp_online);
}

int smp_cpu_count(void) {
    return smp_online > 0 ? smp_online : 1;
}

int smp_cpu_id(void) {
    if (!lapic_present())
        return 0;
    uint32_t aid = lapic_id();
    for (int i = 0; i < NCPU_MAX; i++)
        if (smp_logical[i] == (int)aid)
            return i;
    return 0;
}

int smp_apic_id(void) {
    if (!lapic_present())
        return 0;
    return (int)lapic_id();
}

int smp_is_bsp(void) {
    return smp_cpu_id() == 0;
}

int smp_max_cpus(void) {
    return smp_madt_n > 0 ? smp_madt_n : 1;
}

int smp_ap_started(void) {
    return smp_ap_ok;
}


/* ---------- 1.2.9: IPI, TLB shootdown, balance hints ---------- */

void smp_ipi_init(void) {
    /* IDT gates registered from interrupt.c once stubs exist; here we only
     * clear software state so early boot is safe. */
    for (int i = 0; i < NCPU_MAX; i++) {
        need_resched[i] = 0;
        rq_depth[i] = 0;
        tlb_ack_gen[i] = 0;
    }
    ipi_resched_total = 0;
    ipi_tlb_total = 0;
    balance_steals = 0;
    tlb_shoot_page = 0;
    tlb_shoot_gen = 0;
}

static void send_ipi_vector(int logical_cpu, uint8_t vector) {
#if defined(__i386__)
    (void)logical_cpu;
    (void)vector;
    return;
#else
    if (!lapic_present())
        return;
    if (logical_cpu < 0 || logical_cpu >= NCPU_MAX)
        return;
    int apic = smp_logical[logical_cpu];
    if (apic < 0)
        return;
    /* Fixed delivery, physical destination, vector */
    lapic_send_ipi((uint8_t)apic, (uint32_t)vector);
#endif
}

void smp_send_resched(int cpu) {
    if (cpu < 0 || cpu >= smp_online)
        return;
    need_resched[cpu] = 1;
    if (cpu == smp_cpu_id())
        return;
    send_ipi_vector(cpu, IPI_VECTOR_RESCHED);
    ipi_resched_total++;
}

void smp_send_resched_all(void) {
    int self = smp_cpu_id();
    for (int i = 0; i < smp_online; i++) {
        if (i == self)
            continue;
        smp_send_resched(i);
    }
}

void smp_ipi_resched_handler(void) {
    int id = smp_cpu_id();
    if (id >= 0 && id < NCPU_MAX)
        need_resched[id] = 1;
    lapic_eoi();
}

/* Local TLB ops */
static void local_invlpg(uintptr_t page) {
    if (page == 0) {
        /* reload CR3 */
        uintptr_t cr3;
#if defined(__x86_64__)
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
#else
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
#endif
    } else {
        __asm__ volatile("invlpg (%0)" : : "r"(page) : "memory");
    }
}

void smp_tlb_shootdown(uintptr_t page) {
    local_invlpg(page);
#if defined(__i386__)
    return;
#else
    if (smp_online <= 1 || !lapic_present())
        return;
    tlb_shoot_page = page;
    tlb_shoot_gen++;
    uint32_t gen = tlb_shoot_gen;
    int self = smp_cpu_id();
    for (int i = 0; i < smp_online; i++) {
        if (i == self)
            continue;
        tlb_ack_gen[i] = 0;
        send_ipi_vector(i, IPI_VECTOR_TLB);
        ipi_tlb_total++;
    }
    /* Brief wait for acks (APs may still be parked — do not spin forever) */
    for (volatile int spin = 0; spin < 50000; spin++) {
        int done = 1;
        for (int i = 0; i < smp_online; i++) {
            if (i == self)
                continue;
            if (tlb_ack_gen[i] != gen)
                done = 0;
        }
        if (done)
            break;
        __asm__ volatile("pause");
    }
#endif
}

void smp_tlb_shootdown_all(void) {
    smp_tlb_shootdown(0);
}

void smp_ipi_tlb_handler(void) {
    uintptr_t page = tlb_shoot_page;
    local_invlpg(page);
    int id = smp_cpu_id();
    if (id >= 0 && id < NCPU_MAX)
        tlb_ack_gen[id] = tlb_shoot_gen;
    lapic_eoi();
}

uint64_t smp_ipi_resched_count(void) { return ipi_resched_total; }
uint64_t smp_ipi_tlb_count(void) { return ipi_tlb_total; }
uint64_t smp_balance_steals(void) { return balance_steals; }

int smp_need_resched(void) {
    int id = smp_cpu_id();
    if (id < 0 || id >= NCPU_MAX)
        return 0;
    return need_resched[id];
}

void smp_clear_need_resched(void) {
    int id = smp_cpu_id();
    if (id >= 0 && id < NCPU_MAX)
        need_resched[id] = 0;
}

void smp_rq_note_ready(int delta) {
    int id = smp_cpu_id();
    if (id < 0 || id >= NCPU_MAX)
        return;
    int v = rq_depth[id] + delta;
    if (v < 0) v = 0;
    rq_depth[id] = v;
}

int smp_rq_depth(int cpu) {
    if (cpu < 0 || cpu >= NCPU_MAX)
        return 0;
    return rq_depth[cpu];
}

void smp_note_steal(void) {
    balance_steals++;
}
