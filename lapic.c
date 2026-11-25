/* lapic.c — Local APIC for BSP + IPI (TigxOS 1.3.8-update3)
 *
 * Robustness rules:
 *  - i386: never touch 0xFEE00000 (outside 1 GB identity map) — return early.
 *  - x86_64: map MMIO uncached; if map fails, try direct access only when the
 *    address is already reachable, otherwise stay UP.
 *  - Keep LINT0 in ExtINT so the 8259 PIC continues to deliver timer/keyboard.
 *  - All ICR waits are bounded; never spin forever.
 */

#include "lapic.h"
#include "acpi.h"
#include "console.h"
#include "io.h"
#include "log.h"
#include "memory.h"
#include "string.h"
#include <stdint.h>

static volatile uint32_t *lapic_regs;
static int               lapic_ok;
static uintptr_t         lapic_phys = LAPIC_DEFAULT_ADDR;
static uint32_t          lapic_ver_cached;

static inline uint32_t lapic_read(uint32_t reg) {
    return lapic_regs[reg / 4];
}

static inline void lapic_write(uint32_t reg, uint32_t val) {
    lapic_regs[reg / 4] = val;
    /* Serialize the write with a read of a register that always exists. */
    (void)lapic_regs[LAPIC_ID / 4];
}

int lapic_present(void) {
    return lapic_ok;
}

uintptr_t lapic_base(void) {
    return lapic_phys;
}

uint32_t lapic_version(void) {
    return lapic_ok ? lapic_ver_cached : 0;
}

uint32_t lapic_id(void) {
    if (!lapic_ok)
        return 0;
    return (lapic_read(LAPIC_ID) >> 24) & 0xFF;
}

void lapic_eoi(void) {
    if (lapic_ok)
        lapic_write(LAPIC_EOI, 0);
}

/* Bounded wait for ICR delivery bit to clear. */
static void lapic_wait_icr(void) {
    for (int i = 0; i < 100000; i++) {
        if (!(lapic_read(LAPIC_ICR_LOW) & LAPIC_ICR_DELIVS))
            return;
        __asm__ volatile("pause");
    }
    pr_warn("lapic: ICR delivery timeout\n");
}

void lapic_send_ipi(uint8_t apic_id, uint32_t flags) {
    if (!lapic_ok)
        return;
    lapic_write(LAPIC_ICR_HIGH, ((uint32_t)apic_id) << 24);
    lapic_write(LAPIC_ICR_LOW, flags);
    lapic_wait_icr();
}

/* INIT → delay → SIPI → delay → SIPI (Intel SDM recommended sequence). */
void lapic_start_ap(uint8_t apic_id, uint32_t trampoline_page) {
    if (!lapic_ok)
        return;

    /* INIT assert */
    lapic_send_ipi(apic_id, LAPIC_ICR_INIT | LAPIC_ICR_ASSERT | LAPIC_ICR_LEVEL);
    /* INIT deassert */
    lapic_send_ipi(apic_id, LAPIC_ICR_INIT | LAPIC_ICR_LEVEL);

    /* ~10 ms busy-wait (no calibrated timer yet on APs) */
    for (volatile int i = 0; i < 200000; i++)
        __asm__ volatile("pause");

    uint32_t vec = trampoline_page & 0xFF;
    lapic_send_ipi(apic_id, LAPIC_ICR_STARTUP | vec);
    for (volatile int i = 0; i < 100000; i++)
        __asm__ volatile("pause");
    lapic_send_ipi(apic_id, LAPIC_ICR_STARTUP | vec);
}

void lapic_init(void) {
    const struct acpi_info *ai = acpi_get();
    if (ai && ai->lapic_addr) {
        /* MADT local APIC address must be in MMIO space (~4GB).  Low
         * values mean a corrupt parse — fall back to the default. */
        if (ai->lapic_addr >= 0xFEC00000UL)
            lapic_phys = ai->lapic_addr;
        else
            pr_warn("lapic: ignoring bogus ACPI LAPIC addr 0x%x\n",
                    (unsigned)ai->lapic_addr);
    }

#if defined(__i386__)
    /*
     * i386 identity map covers 0–1 GB only. LAPIC lives at ~4 GB.
     * Any access page-faults and kills the boot (the original make-test hang).
     * Stay UP; SMP on i386 is deferred until a high-memory map exists.
     */
    (void)lapic_phys;
    pr_warn("lapic: skipped on i386 (above 1 GB identity window) — SMP deferred\n");
    lapic_ok = 0;
    return;
#endif

    if (vmm_map_mmio(lapic_phys, lapic_phys, PAGE_SIZE) != 0) {
        pr_warn("lapic: MMIO map failed for 0x%x — trying direct\n",
                (unsigned)lapic_phys);
        /* Fall through: on x86_64 the address may already be reachable. */
    }

    lapic_regs = (volatile uint32_t *)lapic_phys;

    /* Probe version first — a bus error / bad map returns nonsense. */
    uint32_t ver = lapic_read(LAPIC_VER);
    if ((ver & 0xFF) == 0xFF || (ver & 0xFF) == 0x00) {
        pr_warn("lapic: version probe failed (ver=0x%x) — staying UP\n", ver);
        lapic_regs = 0;
        lapic_ok = 0;
        return;
    }
    lapic_ver_cached = ver & 0xFF;

    /* Enable APIC, keep spurious vector 0xFF */
    uint32_t svr = lapic_read(LAPIC_SVR);
    svr |= LAPIC_SVR_ENABLE;
    svr = (svr & ~0xFFu) | 0xFF;
    lapic_write(LAPIC_SVR, svr);

    /*
     * LINT0 = ExtINT so the 8259 PIC virtual-wire path still delivers
     * timer + keyboard. Masking LINT0 after enabling the APIC was the
     * original keyboard/timer death on QEMU.
     */
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT0, 0x700);            /* ExtINT, unmasked */
    lapic_write(LAPIC_LVT_LINT1, 0x400);            /* NMI */
    lapic_write(LAPIC_LVT_ERROR, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_TPR, 0);

    lapic_ok = 1;
    pr_info("lapic: base=0x%x id=%u ver=0x%x\n",
            (unsigned)lapic_phys, lapic_id(), lapic_ver_cached);
}

/* ---- 1.2.9: AP init + periodic LAPIC timer ---- */

void lapic_ap_init(void) {
#if defined(__i386__)
    return;
#else
    if (!lapic_ok || !lapic_regs)
        return;
    /* Soft-enable APIC; keep LINT0 masked on AP (no ExtINT from 8259). */
    uint32_t svr = lapic_read(LAPIC_SVR);
    svr |= LAPIC_SVR_ENABLE;
    lapic_write(LAPIC_SVR, svr);
    lapic_write(LAPIC_LVT_LINT0, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT1, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_ERROR, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_TPR, 0);
    lapic_write(LAPIC_EOI, 0);
#endif
}

void lapic_timer_enable(uint32_t initial_count, uint8_t vector) {
#if defined(__i386__)
    (void)initial_count;
    (void)vector;
    return;
#else
    if (!lapic_ok || !lapic_regs)
        return;
    /* Divide by 16 */
    lapic_write(LAPIC_TIMER_DIV, 0x3);
    /* Periodic mode, unmasked, vector */
    lapic_write(LAPIC_LVT_TIMER, (1u << 17) | (vector & 0xFF));
    lapic_write(LAPIC_TIMER_INIT, initial_count ? initial_count : 100000);
#endif
}

void lapic_timer_disable(void) {
#if defined(__i386__)
    return;
#else
    if (!lapic_ok || !lapic_regs)
        return;
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
#endif
}
