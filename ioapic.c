/* ioapic.c — program I/O APIC redirection (replace 8259 path when present) */
#include "ioapic.h"
#include "acpi.h"
#include "lapic.h"
#include "log.h"
#include "memory.h"
#include "console.h"

#define IOREGSEL 0x00
#define IOWIN    0x10
#define IOAPICID 0x00
#define IOAPICVER 0x01
#define IOREDTBL 0x10

static volatile uint32_t *g_io;
static int g_present;
static int g_max_redir;

static uint32_t ioread(uint32_t reg)
{
    g_io[IOREGSEL / 4] = reg;
    return g_io[IOWIN / 4];
}
static void iowrite(uint32_t reg, uint32_t val)
{
    g_io[IOREGSEL / 4] = reg;
    g_io[IOWIN / 4] = val;
}

void ioapic_set_irq(int irq, uint8_t vector, int masked)
{
    if (!g_present || irq < 0 || irq > g_max_redir)
        return;
    uint32_t lo_idx = IOREDTBL + irq * 2;
    uint32_t hi_idx = lo_idx + 1;
    /* dest = BSP LAPIC id 0, physical mode */
    iowrite(hi_idx, 0);
    uint32_t lo = vector;
    if (masked)
        lo |= (1u << 16);
    /* delivery fixed, physical, edge, active high */
    iowrite(lo_idx, lo);
}

void ioapic_init(void)
{
    g_present = 0;
    g_io = 0;
    const struct acpi_info *ai = acpi_get();
    if (!ai || !ai->present || ai->ioapic_count <= 0) {
        pr_info("ioapic: none in MADT — keeping PIC\n");
        return;
    }
    uint32_t addr = ai->ioapics[0].addr;
    if (addr == 0)
        addr = 0xFEC00000u;
#if defined(__x86_64__)
    if (vmm_map_mmio(addr, addr, 0x1000) != 0) {
        pr_warn("ioapic: map failed\n");
        return;
    }
#else
    /* i386: IOAPIC often at 0xFEC00000 — outside identity; skip */
    if (addr >= 0x40000000u) {
        pr_info("ioapic: skipped on i386 (MMIO high)\n");
        return;
    }
#endif
    g_io = (volatile uint32_t *)(uintptr_t)addr;
    uint32_t ver = ioread(IOAPICVER);
    g_max_redir = (int)((ver >> 16) & 0xFF);
    g_present = 1;
    pr_info("ioapic: @0x%x ver=%u maxredir=%d gsi_base=%u\n",
            addr, ver & 0xFF, g_max_redir, ai->ioapics[0].gsi_base);

#ifdef TIGX_IOAPIC_ROUTE
    /* EXPERIMENTAL, off by default. Needs (1) MADT interrupt-source overrides applied
     * (QEMU wires the PIT timer to GSI 2, not 0) and (2) a LAPIC EOI in the IRQ path
     * instead of the 8259 EOI. Without both the timer never fires and the kernel hangs. */
    for (int i = 0; i <= g_max_redir && i < 24; i++)
        ioapic_set_irq(i, (uint8_t)(32 + i), 1);
    for (int irq = 0; irq < 16 && irq <= g_max_redir; irq++)
        ioapic_set_irq(irq, (uint8_t)(32 + irq), 0);
    __asm__ volatile("movb $0xff, %%al; outb %%al, $0xa1; outb %%al, $0x21" ::: "al");
    pr_info("ioapic: ISA IRQs 0-15 routed to vectors 32-47; PIC masked\n");
#else
    /* Detected and readable, but the 8259 PIC stays the active interrupt controller. */
    pr_info("ioapic: found, routing not enabled (8259 PIC stays active)\n");
#endif
}

int ioapic_present(void) { return g_present; }
