/* pci.c — PCI enumeration + MSI/MSI-X (TigxOS 1.3.8) */

#include "pci.h"
#include "hal.h"
#include "console.h"
#include "string.h"
#include "io.h"
#include "log.h"
#include <stdint.h>

static struct pci_device g_devices[PCI_MAX_DEVICES];
static size_t            g_count;

/* ------------------------------------------------------------------ */
/* Config space                                                       */
/* ------------------------------------------------------------------ */

static uint32_t make_address(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    return (1u << 31)
         | ((uint32_t)bus  << 16)
         | ((uint32_t)slot << 11)
         | ((uint32_t)func <<  8)
         | (offset & 0xFC);
}

uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    outl(PCI_CONFIG_ADDRESS, make_address(bus, slot, func, offset));
    return inb(PCI_CONFIG_DATA + (offset & 3));
}

uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    outl(PCI_CONFIG_ADDRESS, make_address(bus, slot, func, offset));
    return inw(PCI_CONFIG_DATA + (offset & 2));
}

uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    outl(PCI_CONFIG_ADDRESS, make_address(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

void pci_write8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t val)
{
    outl(PCI_CONFIG_ADDRESS, make_address(bus, slot, func, offset));
    outb(PCI_CONFIG_DATA + (offset & 3), val);
}

void pci_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t val)
{
    outl(PCI_CONFIG_ADDRESS, make_address(bus, slot, func, offset));
    outw(PCI_CONFIG_DATA + (offset & 2), val);
}

void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val)
{
    outl(PCI_CONFIG_ADDRESS, make_address(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, val);
}

/* ------------------------------------------------------------------ */
/* BAR decoding                                                       */
/* ------------------------------------------------------------------ */

static void decode_bar(struct pci_device *dev, int idx)
{
    uint8_t off = (uint8_t)(PCI_BAR0 + idx * 4);
    uint32_t raw = pci_dev_read32(dev, off);

    if (raw == 0 || raw == 0xFFFFFFFFu) {
        dev->bars[idx].address = 0;
        dev->bars[idx].size    = 0;
        return;
    }

    int is_io = (raw & 1) != 0;
    dev->bars[idx].is_io = is_io;

    if (is_io) {
        uint32_t base = raw & ~0x3u;
        pci_dev_write32(dev, off, 0xFFFFFFFFu);
        uint32_t mask = pci_dev_read32(dev, off);
        pci_dev_write32(dev, off, raw);
        uint32_t size = ~(mask & ~0x3u) + 1;
        dev->bars[idx].address      = base;
        dev->bars[idx].size         = size;
        dev->bars[idx].is_64        = 0;
        dev->bars[idx].prefetchable = 0;
    } else {
        int is_64 = ((raw >> 1) & 3) == 2;
        int pref  = (raw & (1u << 3)) != 0;
        uint64_t base = raw & ~0xFull;

        if (is_64 && idx < 5) {
            uint32_t hi = pci_dev_read32(dev, off + 4);
            base |= ((uint64_t)hi << 32);
        }

        pci_dev_write32(dev, off, 0xFFFFFFFFu);
        uint32_t mask_lo = pci_dev_read32(dev, off);
        pci_dev_write32(dev, off, raw);

        uint64_t mask = mask_lo & ~0xFull;
        if (is_64 && idx < 5) {
            uint32_t orig_hi = pci_dev_read32(dev, off + 4);
            pci_dev_write32(dev, off + 4, 0xFFFFFFFFu);
            uint32_t mask_hi = pci_dev_read32(dev, off + 4);
            pci_dev_write32(dev, off + 4, orig_hi);
            mask |= ((uint64_t)mask_hi << 32);
        }

        dev->bars[idx].address      = base;
        dev->bars[idx].size         = ~mask + 1;
        dev->bars[idx].is_64        = is_64;
        dev->bars[idx].prefetchable = pref;

        if (is_64 && idx < 5)
            dev->bars[idx + 1].address = 0; /* consumed */
    }
}

/* ------------------------------------------------------------------ */
/* Capability list                                                    */
/* ------------------------------------------------------------------ */

static void parse_caps(struct pci_device *dev)
{
    uint16_t status = pci_dev_read16(dev, PCI_STATUS);
    if (!(status & PCI_STATUS_CAP_LIST))
        return;

    uint16_t ptr = pci_dev_read8(dev, PCI_CAP_PTR) & 0xFC;
    int guard = 0;

    while (ptr && ptr < 0x100 && guard++ < 48) {
        uint8_t id   = pci_dev_read8(dev, ptr + 0);
        uint8_t next = pci_dev_read8(dev, ptr + 1) & 0xFC;

        if (id == PCI_CAP_MSI) {
            dev->cap_msi = ptr;
        } else if (id == PCI_CAP_MSIX) {
            dev->cap_msix = ptr;
            uint16_t msg_ctrl = pci_dev_read16(dev, ptr + 2);
            dev->msix_table_size = (msg_ctrl & 0x7FF) + 1;

            uint32_t table = pci_dev_read32(dev, ptr + 4);
            dev->msix_table_bir    = table & 7;
            dev->msix_table_offset = table & ~7u;

            uint32_t pba = pci_dev_read32(dev, ptr + 8);
            dev->msix_pba_bir    = pba & 7;
            dev->msix_pba_offset = pba & ~7u;
        } else if (id == PCI_CAP_PCIE) {
            dev->cap_pcie = ptr;
        }
        ptr = next;
    }
}

/* ------------------------------------------------------------------ */
/* Probe one function                                                 */
/* ------------------------------------------------------------------ */

static void probe_function(uint8_t bus, uint8_t slot, uint8_t func)
{
    if (g_count >= PCI_MAX_DEVICES)
        return;

    uint16_t vendor = pci_read16(bus, slot, func, PCI_VENDOR_ID);
    if (vendor == 0xFFFF || vendor == 0x0000)
        return;

    struct pci_device *dev = &g_devices[g_count];
    memset(dev, 0, sizeof(*dev));

    dev->bus        = bus;
    dev->slot       = slot;
    dev->func       = func;
    dev->vendor_id  = vendor;
    dev->device_id  = pci_read16(bus, slot, func, PCI_DEVICE_ID);
    dev->revision   = pci_read8 (bus, slot, func, PCI_REVISION_ID);
    dev->prog_if    = pci_read8 (bus, slot, func, PCI_PROG_IF);
    dev->subclass   = pci_read8 (bus, slot, func, PCI_SUBCLASS);
    dev->class_code = pci_read8 (bus, slot, func, PCI_CLASS);
    dev->header_type= pci_read8 (bus, slot, func, PCI_HEADER_TYPE);
    dev->interrupt_line = pci_read8(bus, slot, func, PCI_INTERRUPT_LINE);
    dev->interrupt_pin  = pci_read8(bus, slot, func, PCI_INTERRUPT_PIN);

    if ((dev->header_type & 0x7F) == 0) {
        for (int i = 0; i < PCI_MAX_BARS; i++) {
            if (i > 0 && dev->bars[i - 1].is_64)
                continue;
            decode_bar(dev, i);
        }
    }

    parse_caps(dev);

    struct hal_device hdev;
    memset(&hdev, 0, sizeof(hdev));
    hdev.class       = HAL_DEV_PCI;
    hdev.vendor      = dev->vendor_id;
    hdev.device      = dev->device_id;
    hdev.bus         = bus;
    hdev.slot        = slot;
    hdev.func        = func;
    hdev.driver_data = dev;
    dev->hal_id = hal_device_register(&hdev);

    g_count++;
}

static void scan_bus(uint8_t bus)
{
    for (uint8_t slot = 0; slot < 32; slot++) {
        uint16_t vendor = pci_read16(bus, slot, 0, PCI_VENDOR_ID);
        if (vendor == 0xFFFF)
            continue;

        probe_function(bus, slot, 0);

        uint8_t ht = pci_read8(bus, slot, 0, PCI_HEADER_TYPE);
        if (ht & 0x80) {
            for (uint8_t f = 1; f < 8; f++)
                probe_function(bus, slot, f);
        }
    }
}

void pci_init(void)
{
    g_count = 0;
    memset(g_devices, 0, sizeof(g_devices));

    for (uint8_t b = 0; b < 8; b++)
        scan_bus(b);

    pr_info("pci: enumerated %zu function(s)\n", g_count);
}

/* ------------------------------------------------------------------ */
/* Lookup                                                             */
/* ------------------------------------------------------------------ */

size_t pci_device_count(void)
{
    return g_count;
}

const struct pci_device *pci_device_at(size_t index)
{
    if (index >= g_count)
        return NULL;
    return &g_devices[index];
}

const struct pci_device *pci_find(uint16_t vendor, uint16_t device)
{
    return pci_find_next(NULL, vendor, device);
}

const struct pci_device *pci_find_next(const struct pci_device *prev,
                                       uint16_t vendor, uint16_t device)
{
    size_t start = 0;
    if (prev) {
        start = (size_t)(prev - g_devices) + 1;
        if (start > g_count)
            return NULL;
    }
    for (size_t i = start; i < g_count; i++) {
        const struct pci_device *d = &g_devices[i];
        if ((vendor == 0xFFFF || d->vendor_id == vendor) &&
            (device == 0xFFFF || d->device_id == device))
            return d;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Enable                                                             */
/* ------------------------------------------------------------------ */

void pci_enable_device(struct pci_device *dev)
{
    if (!dev)
        return;

    uint16_t cmd = pci_dev_read16(dev, PCI_COMMAND);
    cmd |= PCI_CMD_BUS_MASTER;

    for (int i = 0; i < PCI_MAX_BARS; i++) {
        if (dev->bars[i].size == 0)
            continue;
        if (dev->bars[i].is_io)
            cmd |= PCI_CMD_IO;
        else
            cmd |= PCI_CMD_MEM;
    }

    cmd &= (uint16_t)~PCI_CMD_INTX_DISABLE;
    pci_dev_write16(dev, PCI_COMMAND, cmd);
    dev->enabled = 1;
}

/* ------------------------------------------------------------------ */
/* MSI                                                                */
/* ------------------------------------------------------------------ */

int pci_enable_msi(struct pci_device *dev, uint8_t preferred_cpu,
                   struct hal_msi_message *out_msg)
{
    if (!dev || !dev->cap_msi || !out_msg)
        return -1;

    struct hal_msi_message msg;
    if (hal_msi_allocate(preferred_cpu, &msg) != 0)
        return -1;

    uint8_t  cap = dev->cap_msi;
    uint16_t ctrl = pci_dev_read16(dev, cap + 2);

    /* request a single message */
    ctrl &= (uint16_t)~0x0070;
    ctrl |= 1u;                     /* MSI Enable */

    int is64 = (ctrl & (1u << 7)) != 0;
    if (is64) {
        pci_dev_write32(dev, cap + 4, (uint32_t)(msg.address & 0xFFFFFFFFu));
        pci_dev_write32(dev, cap + 8, (uint32_t)(msg.address >> 32));
        pci_dev_write16(dev, cap + 12, (uint16_t)msg.data);
    } else {
        pci_dev_write32(dev, cap + 4, (uint32_t)(msg.address & 0xFFFFFFFFu));
        pci_dev_write16(dev, cap + 8, (uint16_t)msg.data);
    }
    pci_dev_write16(dev, cap + 2, ctrl);

    uint16_t cmd = pci_dev_read16(dev, PCI_COMMAND);
    cmd |= PCI_CMD_INTX_DISABLE;
    pci_dev_write16(dev, PCI_COMMAND, cmd);

    *out_msg = msg;
    return 0;
}

/* ------------------------------------------------------------------ */
/* MSI-X                                                              */
/* ------------------------------------------------------------------ */

int pci_enable_msix(struct pci_device *dev, uint8_t preferred_cpu,
                    struct hal_msi_message *out_msg)
{
    if (!dev || !dev->cap_msix || !out_msg)
        return -1;
    if (dev->msix_table_bir >= PCI_MAX_BARS ||
        dev->bars[dev->msix_table_bir].address == 0)
        return -1;

    struct hal_msi_message msg;
    if (hal_msi_allocate(preferred_cpu, &msg) != 0)
        return -1;

    /* Table is expected to be identity-mapped or already mapped by the
     * driver. We write entry 0 only; multi-vector is left to the driver. */
    volatile uint32_t *table =
        (volatile uint32_t *)(uintptr_t)(dev->bars[dev->msix_table_bir].address
                                         + dev->msix_table_offset);

    table[0] = (uint32_t)(msg.address & 0xFFFFFFFFu);
    table[1] = (uint32_t)(msg.address >> 32);
    table[2] = msg.data;
    table[3] = 0;                   /* unmasked */

    uint16_t ctrl = pci_dev_read16(dev, dev->cap_msix + 2);
    ctrl |= (1u << 15);             /* MSI-X Enable */
    ctrl &= (uint16_t)~(1u << 14);  /* Function Mask clear */
    pci_dev_write16(dev, dev->cap_msix + 2, ctrl);

    uint16_t cmd = pci_dev_read16(dev, PCI_COMMAND);
    cmd |= PCI_CMD_INTX_DISABLE;
    pci_dev_write16(dev, PCI_COMMAND, cmd);

    *out_msg = msg;
    return 0;
}

void pci_disable_msi(struct pci_device *dev)
{
    if (!dev)
        return;

    if (dev->cap_msi) {
        uint16_t ctrl = pci_dev_read16(dev, dev->cap_msi + 2);
        ctrl &= (uint16_t)~1u;
        pci_dev_write16(dev, dev->cap_msi + 2, ctrl);
    }
    if (dev->cap_msix) {
        uint16_t ctrl = pci_dev_read16(dev, dev->cap_msix + 2);
        ctrl &= (uint16_t)~(1u << 15);
        pci_dev_write16(dev, dev->cap_msix + 2, ctrl);
    }

    uint16_t cmd = pci_dev_read16(dev, PCI_COMMAND);
    cmd &= (uint16_t)~PCI_CMD_INTX_DISABLE;
    pci_dev_write16(dev, PCI_COMMAND, cmd);
}

/* ------------------------------------------------------------------ */
/* Debug                                                              */
/* ------------------------------------------------------------------ */

static const char *class_name(uint8_t c)
{
    switch (c) {
    case 0x00: return "Unclassified";
    case 0x01: return "Mass Storage";
    case 0x02: return "Network";
    case 0x03: return "Display";
    case 0x04: return "Multimedia";
    case 0x05: return "Memory";
    case 0x06: return "Bridge";
    case 0x07: return "Simple Comm";
    case 0x08: return "Base System";
    case 0x09: return "Input";
    case 0x0C: return "Serial Bus";
    case 0x0D: return "Wireless";
    default:   return "Other";
    }
}

void pci_dump_device(const struct pci_device *dev)
{
    if (!dev)
        return;
    kprintf("  %02x:%02x.%x  %04x:%04x  class %02x.%02x.%02x (%s)  rev %02x\n",
            dev->bus, dev->slot, dev->func,
            dev->vendor_id, dev->device_id,
            dev->class_code, dev->subclass, dev->prog_if,
            class_name(dev->class_code),
            dev->revision);
    for (int i = 0; i < PCI_MAX_BARS; i++) {
        if (dev->bars[i].size == 0)
            continue;
        kprintf("    BAR%d  %s  %016llx  size %llu%s%s\n",
                i,
                dev->bars[i].is_io ? "I/O " : "MEM ",
                (unsigned long long)dev->bars[i].address,
                (unsigned long long)dev->bars[i].size,
                dev->bars[i].is_64 ? " 64-bit" : "",
                dev->bars[i].prefetchable ? " pref" : "");
    }
    if (dev->cap_msi)
        kprintf("    MSI   @ 0x%02x\n", dev->cap_msi);
    if (dev->cap_msix)
        kprintf("    MSI-X @ 0x%02x  table size %u  BIR %u\n",
                dev->cap_msix, dev->msix_table_size, dev->msix_table_bir);
    if (dev->interrupt_pin)
        kprintf("    INTx pin %c  line %u\n",
                'A' + dev->interrupt_pin - 1, dev->interrupt_line);
}

void pci_dump_all(void)
{
    kprintf("=== PCI devices (%zu) ===\n", g_count);
    for (size_t i = 0; i < g_count; i++)
        pci_dump_device(&g_devices[i]);
}
