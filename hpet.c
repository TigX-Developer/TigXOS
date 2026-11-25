/* hpet.c — High Precision Event Timer (TigxOS 1.3.8)
 *
 * Base address prefers the ACPI HPET table; falls back to 0xFED00000.
 * i386 skips HPET (MMIO above the identity window).
 */
#include "hpet.h"
#include "acpi.h"
#include "console.h"
#include "log.h"
#include "memory.h"
#include <stdint.h>

static volatile uint32_t *g_hpet;
static int g_present;
static uint32_t g_period_fs;

void hpet_init(void)
{
    g_present = 0;
    g_hpet = 0;
    g_period_fs = 0;

#if !defined(__x86_64__)
    pr_info("hpet: skipped on i386 (MMIO above identity window)\n");
    return;
#else
    uintptr_t base = 0xFED00000UL;
    const struct acpi_info *ai = acpi_get();
    if (ai && ai->hpet_addr)
        base = (uintptr_t)ai->hpet_addr;

    if (vmm_map_mmio(base, base, 0x1000) != 0) {
        pr_info("hpet: map failed at 0x%lx\n", (unsigned long)base);
        return;
    }

    volatile uint32_t *h32 = (volatile uint32_t *)base;
    uint32_t cap_lo = h32[0];
    uint32_t cap_hi = h32[1];
    if (cap_lo == 0 || cap_lo == 0xFFFFFFFFu) {
        pr_info("hpet: not present at 0x%lx\n", (unsigned long)base);
        return;
    }

    g_period_fs = cap_hi;
    if (g_period_fs == 0 || g_period_fs == 0xFFFFFFFFu) {
        pr_warn("hpet: bad period\n");
        return;
    }

    /* CONFIG at 0x10: clear LEG_RT_CNF, set ENABLE_CNF */
    uint32_t cnf = h32[0x10 / 4];
    cnf &= ~(1u << 1);
    cnf |= 1u;
    h32[0x10 / 4] = cnf;

    g_hpet = h32;
    g_present = 1;
    pr_info("hpet: base=0x%lx period=%u fs (acpi=%s)\n",
            (unsigned long)base, g_period_fs,
            (ai && ai->hpet_addr) ? "yes" : "fallback");
#endif
}

int hpet_present(void)
{
    return g_present;
}

uint64_t hpet_read_counter(void)
{
    if (!g_present || !g_hpet)
        return 0;
    return *(volatile uint64_t *)(g_hpet + (0xF0 / 4));
}

void hpet_udelay(uint32_t us)
{
    if (!g_present || g_period_fs == 0) {
        for (volatile uint32_t i = 0; i < us * 100; i++)
            __asm__ volatile("pause");
        return;
    }
    uint64_t need = ((uint64_t)us * 1000000000ULL) / g_period_fs;
    if (need == 0)
        need = 1;
    uint64_t start = hpet_read_counter();
    while (hpet_read_counter() - start < need)
        __asm__ volatile("pause");
}
