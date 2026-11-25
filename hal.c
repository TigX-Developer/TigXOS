/* hal.c — Hardware Abstraction Layer (TigxOS 1.3.8) */

#include "hal.h"
#include "console.h"
#include "string.h"
#include "lapic.h"
#include "smp.h"
#include "acpi.h"
#include "log.h"
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* State                                                              */
/* ------------------------------------------------------------------ */

static struct hal_machine_info g_machine;
static struct hal_device       g_devices[HAL_MAX_DEVICES];
static size_t                  g_device_count;
static uint32_t                g_next_id = 1;

/* MSI vector pool (kept clear of exceptions + classic IRQs) */
#define HAL_MSI_BASE   0x40
#define HAL_MSI_COUNT  64
static uint64_t g_msi_bitmap;

static char g_vendor[16];
static char g_brand[64];

/* ------------------------------------------------------------------ */
/* CPUID                                                              */
/* ------------------------------------------------------------------ */

static void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(0));
}

static void detect_vendor(void)
{
    uint32_t a, b, c, d;
    cpuid(0, &a, &b, &c, &d);
    memcpy(g_vendor + 0, &b, 4);
    memcpy(g_vendor + 4, &d, 4);
    memcpy(g_vendor + 8, &c, 4);
    g_vendor[12] = 0;
}

static void detect_brand(void)
{
    uint32_t a, b, c, d;
    cpuid(0x80000000u, &a, &b, &c, &d);
    if (a < 0x80000004u) {
        g_brand[0] = 0;
        return;
    }
    for (uint32_t i = 0; i < 3; i++) {
        cpuid(0x80000002u + i, &a, &b, &c, &d);
        memcpy(g_brand + i * 16 +  0, &a, 4);
        memcpy(g_brand + i * 16 +  4, &b, 4);
        memcpy(g_brand + i * 16 +  8, &c, 4);
        memcpy(g_brand + i * 16 + 12, &d, 4);
    }
    g_brand[48] = 0;
    /* trim leading spaces */
    const char *p = g_brand;
    while (*p == ' ')
        p++;
    if (p != g_brand)
        memmove(g_brand, p, strlen(p) + 1);
}

/* ------------------------------------------------------------------ */
/* Public                                                             */
/* ------------------------------------------------------------------ */

const struct hal_machine_info *hal_machine(void)
{
    return &g_machine;
}

void hal_early_init(void)
{
    memset(&g_machine, 0, sizeof(g_machine));
    g_machine.arch = (sizeof(void *) == 8) ? HAL_ARCH_X86_64 : HAL_ARCH_I386;

    detect_vendor();
    detect_brand();
    g_machine.vendor = g_vendor;
    g_machine.model  = g_brand[0] ? g_brand : "(unknown)";

    uint32_t a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    g_machine.has_apic   = (d & (1u << 9)) != 0;
    g_machine.has_x2apic = (c & (1u << 21)) != 0;
    g_machine.has_msi    = g_machine.has_apic;
    g_machine.has_msi_x  = g_machine.has_apic;
    g_machine.cpu_count  = 1;
    g_machine.bsp_apic_id = 0;

    pr_info("hal: arch=%s vendor=%s\n",
            g_machine.arch == HAL_ARCH_X86_64 ? "x86_64" : "i386",
            g_machine.vendor);
    if (g_brand[0])
        pr_info("hal: cpu=%s\n", g_machine.model);
    pr_info("hal: apic=%d x2apic=%d msi=%d\n",
            g_machine.has_apic, g_machine.has_x2apic, g_machine.has_msi);
}

void hal_late_init(void)
{
    /* Prefer SMP / ACPI numbers when they are available */
    int n = smp_cpu_count();
    if (n < 1)
        n = 1;
    g_machine.cpu_count = (uint32_t)n;

    if (lapic_present())
        g_machine.bsp_apic_id = lapic_id();
    else {
        const struct acpi_info *ai = acpi_get();
        if (ai && ai->cpu_count > 0) {
            for (int i = 0; i < ai->cpu_count; i++) {
                if (ai->cpus[i].is_bsp) {
                    g_machine.bsp_apic_id = ai->cpus[i].apic_id;
                    break;
                }
            }
        }
    }

    pr_info("hal: %u logical CPU(s), BSP APIC ID %u\n",
            g_machine.cpu_count, g_machine.bsp_apic_id);
}

/* ------------------------------------------------------------------ */
/* Device registry                                                    */
/* ------------------------------------------------------------------ */

uint32_t hal_device_register(const struct hal_device *dev)
{
    if (!dev || g_device_count >= HAL_MAX_DEVICES)
        return 0;
    struct hal_device *slot = &g_devices[g_device_count++];
    *slot = *dev;
    slot->id = g_next_id++;
    return slot->id;
}

const struct hal_device *hal_device_get(uint32_t id)
{
    if (id == 0)
        return NULL;
    for (size_t i = 0; i < g_device_count; i++)
        if (g_devices[i].id == id)
            return &g_devices[i];
    return NULL;
}

int hal_device_foreach(int (*fn)(const struct hal_device *, void *), void *ctx)
{
    if (!fn)
        return 0;
    for (size_t i = 0; i < g_device_count; i++)
        if (!fn(&g_devices[i], ctx))
            return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* MSI vector allocation                                              */
/* ------------------------------------------------------------------ */

int hal_msi_allocate(uint8_t preferred_cpu, struct hal_msi_message *out)
{
    if (!out || !g_machine.has_msi)
        return -1;

    int bit = -1;
    for (int i = 0; i < HAL_MSI_COUNT; i++) {
        if ((g_msi_bitmap & (1ull << i)) == 0) {
            bit = i;
            break;
        }
    }
    if (bit < 0)
        return -1;

    g_msi_bitmap |= (1ull << bit);
    uint8_t vector = (uint8_t)(HAL_MSI_BASE + bit);

    uint8_t cpu = (preferred_cpu == 0xff) ? 0 : preferred_cpu;
    if (cpu >= g_machine.cpu_count)
        cpu = 0;

    uint32_t apic_id = (cpu == 0) ? g_machine.bsp_apic_id : cpu;
    uint64_t addr = 0xFEE00000ull | ((uint64_t)apic_id << 12);
    uint32_t data = vector;     /* edge-triggered, fixed delivery */

    out->address = addr;
    out->data    = data;
    out->vector  = vector;
    out->cpu     = cpu;
    return 0;
}

void hal_msi_free(uint8_t vector)
{
    if (vector < HAL_MSI_BASE || vector >= HAL_MSI_BASE + HAL_MSI_COUNT)
        return;
    int bit = vector - HAL_MSI_BASE;
    g_msi_bitmap &= ~(1ull << bit);
}

/* ------------------------------------------------------------------ */
/* Debug                                                              */
/* ------------------------------------------------------------------ */

void hal_dump_machine(void)
{
    const struct hal_machine_info *m = &g_machine;
    kprintf("=== HAL machine ===\n");
    kprintf("  arch          : %s\n", m->arch == HAL_ARCH_X86_64 ? "x86_64" : "i386");
    kprintf("  vendor        : %s\n", m->vendor);
    kprintf("  model         : %s\n", m->model);
    kprintf("  logical CPUs  : %u\n", m->cpu_count);
    kprintf("  BSP APIC ID   : %u\n", m->bsp_apic_id);
    kprintf("  APIC / x2APIC : %d / %d\n", m->has_apic, m->has_x2apic);
    kprintf("  MSI / MSI-X   : %d / %d\n", m->has_msi, m->has_msi_x);
}

void hal_dump_devices(void)
{
    kprintf("=== HAL devices (%zu) ===\n", g_device_count);
    for (size_t i = 0; i < g_device_count; i++) {
        const struct hal_device *d = &g_devices[i];
        kprintf("  [%u] %s  %04x:%04x  %02x:%02x.%x  \"%s\"\n",
                d->id,
                d->class == HAL_DEV_PCI ? "PCI" :
                d->class == HAL_DEV_VIRTIO ? "VIRTIO" : "LEGACY",
                d->vendor, d->device,
                d->bus, d->slot, d->func,
                d->name ? d->name : "");
    }
}
