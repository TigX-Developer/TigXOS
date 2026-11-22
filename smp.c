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


/* AP C entry — reserved for 1.2.9 when APs get a real stack + long mode. */
static void __attribute__((unused)) ap_main(void) {
    uint32_t aid = lapic_id();
    int id = 0;
    for (int i = 0; i < NCPU_MAX; i++) {
        if (smp_logical[i] == (int)aid) {
            id = i;
            break;
        }
    }
    ap_ready[id] = 1;
    pr_info("smp: CPU %d (APIC %u) online and parked\n", id, aid);
    for (;;)
        __asm__ volatile("hlt");
}

/*
 * Minimal real-mode trampoline at 0x8000.
 * Layout:
 *   [0x00]  16-bit entry (SIPI lands here, CS=0x800)
 *   [0x78]  GDTR
 *   [0x80]  GDT (null + code32 + data32)
 *   [0x100] 32-bit stub: set segments, lock-inc mailbox, hlt loop
 */
static void install_trampoline(void) {
    uint8_t *t = (uint8_t *)(uintptr_t)TRAMPOLINE_PHYS;
    memset(t, 0, TRAMPOLINE_SIZE);

    /* cli */
    t[0x00] = 0xFA;
    /* xor ax,ax ; mov ds,ax */
    t[0x01] = 0x31; t[0x02] = 0xC0;
    t[0x03] = 0x8E; t[0x04] = 0xD8;
    /* lgdt [0x8078] */
    t[0x05] = 0x0F; t[0x06] = 0x01; t[0x07] = 0x16;
    t[0x08] = 0x78; t[0x09] = 0x80;
    /* mov eax,cr0 ; or al,1 ; mov cr0,eax */
    t[0x0A] = 0x0F; t[0x0B] = 0x20; t[0x0C] = 0xC0;
    t[0x0D] = 0x0C; t[0x0E] = 0x01;
    t[0x0F] = 0x0F; t[0x10] = 0x22; t[0x11] = 0xC0;
    /* jmp far 0x08:0x8100 */
    t[0x12] = 0x66; t[0x13] = 0xEA;
    t[0x14] = 0x00; t[0x15] = 0x81; t[0x16] = 0x00; t[0x17] = 0x00;
    t[0x18] = 0x08; t[0x19] = 0x00;

    /* GDT at 0x8080 */
    uint8_t *gdt = t + 0x80;
    memset(gdt, 0, 8);
    /* code32: base 0, limit 4 GB, type 0x9A, gran 0xCF */
    gdt[8]  = 0xFF; gdt[9]  = 0xFF; gdt[10] = 0x00; gdt[11] = 0x00;
    gdt[12] = 0x00; gdt[13] = 0x9A; gdt[14] = 0xCF; gdt[15] = 0x00;
    /* data32: type 0x92 */
    gdt[16] = 0xFF; gdt[17] = 0xFF; gdt[18] = 0x00; gdt[19] = 0x00;
    gdt[20] = 0x00; gdt[21] = 0x92; gdt[22] = 0xCF; gdt[23] = 0x00;

    /* GDTR at 0x8078 */
    uint8_t *gdtr = t + 0x78;
    gdtr[0] = 23; gdtr[1] = 0;
    uint32_t gdt_phys = TRAMPOLINE_PHYS + 0x80;
    gdtr[2] = (uint8_t)(gdt_phys);
    gdtr[3] = (uint8_t)(gdt_phys >> 8);
    gdtr[4] = (uint8_t)(gdt_phys >> 16);
    gdtr[5] = (uint8_t)(gdt_phys >> 24);

    /* 32-bit stub at 0x8100 */
    uint8_t *p = t + 0x100;
    int i = 0;
    p[i++] = 0x66; p[i++] = 0xB8; p[i++] = 0x10; p[i++] = 0x00; /* mov ax,0x10 */
    p[i++] = 0x8E; p[i++] = 0xD8; /* mov ds,ax */
    p[i++] = 0x8E; p[i++] = 0xC0; /* mov es,ax */
    p[i++] = 0x8E; p[i++] = 0xD0; /* mov ss,ax */
    /* lock incl dword [0x8FF0] */
    p[i++] = 0xF0; p[i++] = 0xFF; p[i++] = 0x05;
    p[i++] = 0xF0; p[i++] = 0x8F; p[i++] = 0x00; p[i++] = 0x00;
    /* sti ; hlt ; jmp $ */
    p[i++] = 0xFB;
    p[i++] = 0xF4;
    p[i++] = 0xEB; p[i++] = 0xFD;

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

    volatile uint32_t *mailbox = (volatile uint32_t *)(uintptr_t)MAILBOX_PHYS;
    *mailbox = 0;
    __asm__ volatile("mfence" ::: "memory");

    install_trampoline();

    int started = 1; /* BSP */
    int attempted = 0;
    for (int i = 0; i < n; i++) {
        if (ai->cpus[i].is_bsp || !ai->cpus[i].enabled)
            continue;
        uint8_t apic = ai->cpus[i].apic_id;
        attempted++;
        pr_info("smp: starting APIC %u ...\n", apic);

        uint32_t before = *mailbox;
        lapic_start_ap(apic, TRAMPOLINE_PHYS >> 12);

        /* Wait up to ~50 ms of busy loops for the mailbox bump */
        int ok = 0;
        for (int t = 0; t < 500000; t++) {
            if (*mailbox > before) {
                ok = 1;
                break;
            }
            __asm__ volatile("pause");
        }
        if (ok) {
            started++;
            smp_ap_ok++;
            pr_info("smp: APIC %u answered (mailbox=%u)\n", apic, *mailbox);
        } else {
            pr_warn("smp: APIC %u did not respond (mailbox still %u)\n",
                    apic, *mailbox);
        }
    }

    smp_online = started;
    pr_info("smp: %d CPU(s) online of %d attempted (BSP scheduling only until 1.2.9)\n",
            smp_online, attempted + 1);

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
