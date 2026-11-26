/* pci.h — PCI enumeration + MSI/MSI-X (TigxOS 1.3.8)
 *
 * Classic config-space access via 0xCF8/0xCFC.
 * Scans buses, decodes BARs, walks the capability list, and can enable
 * single-message MSI or the first MSI-X table entry.
 */
#ifndef TIGX_PCI_H
#define TIGX_PCI_H

#include <stdint.h>
#include <stddef.h>
#include "hal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PCI_CONFIG_ADDRESS  0xCF8
#define PCI_CONFIG_DATA     0xCFC

#define PCI_VENDOR_ID       0x00
#define PCI_DEVICE_ID       0x02
#define PCI_COMMAND         0x04
#define PCI_STATUS          0x06
#define PCI_REVISION_ID     0x08
#define PCI_PROG_IF         0x09
#define PCI_SUBCLASS        0x0A
#define PCI_CLASS           0x0B
#define PCI_HEADER_TYPE     0x0E
#define PCI_BAR0            0x10
#define PCI_CAP_PTR         0x34
#define PCI_INTERRUPT_LINE  0x3C
#define PCI_INTERRUPT_PIN   0x3D

#define PCI_CMD_IO          (1u << 0)
#define PCI_CMD_MEM         (1u << 1)
#define PCI_CMD_BUS_MASTER  (1u << 2)
#define PCI_CMD_INTX_DISABLE (1u << 10)

#define PCI_STATUS_CAP_LIST (1u << 4)

#define PCI_CAP_MSI         0x05
#define PCI_CAP_MSIX        0x11
#define PCI_CAP_PCIE        0x10

#define PCI_MAX_DEVICES     64
#define PCI_MAX_BARS        6

struct pci_bar {
    uint64_t address;
    uint64_t size;
    int      is_io;
    int      is_64;
    int      prefetchable;
};

struct pci_device {
    uint8_t  bus, slot, func;
    uint16_t vendor_id, device_id;
    uint8_t  revision, class_code, subclass, prog_if;
    uint8_t  header_type;
    struct pci_bar bars[PCI_MAX_BARS];
    uint8_t  interrupt_line, interrupt_pin;

    /* Capability offsets (0 = absent) */
    uint8_t  cap_msi;
    uint8_t  cap_msix;
    uint8_t  cap_pcie;

    /* MSI-X details (valid when cap_msix != 0) */
    uint16_t msix_table_size;
    uint8_t  msix_table_bir;
    uint32_t msix_table_offset;
    uint8_t  msix_pba_bir;
    uint32_t msix_pba_offset;

    int      enabled;
    uint32_t hal_id;
};

void               pci_init(void);
size_t             pci_device_count(void);
const struct pci_device *pci_device_at(size_t index);
const struct pci_device *pci_find(uint16_t vendor, uint16_t device);
const struct pci_device *pci_find_next(const struct pci_device *prev,
                                       uint16_t vendor, uint16_t device);

uint8_t  pci_read8 (uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_write8 (uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t  val);
void     pci_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t val);
void     pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val);

static inline uint8_t  pci_dev_read8 (const struct pci_device *d, uint8_t off) {
    return pci_read8(d->bus, d->slot, d->func, off);
}
static inline uint16_t pci_dev_read16(const struct pci_device *d, uint8_t off) {
    return pci_read16(d->bus, d->slot, d->func, off);
}
static inline uint32_t pci_dev_read32(const struct pci_device *d, uint8_t off) {
    return pci_read32(d->bus, d->slot, d->func, off);
}
static inline void pci_dev_write8 (const struct pci_device *d, uint8_t off, uint8_t v) {
    pci_write8(d->bus, d->slot, d->func, off, v);
}
static inline void pci_dev_write16(const struct pci_device *d, uint8_t off, uint16_t v) {
    pci_write16(d->bus, d->slot, d->func, off, v);
}
static inline void pci_dev_write32(const struct pci_device *d, uint8_t off, uint32_t v) {
    pci_write32(d->bus, d->slot, d->func, off, v);
}

void pci_enable_device(struct pci_device *dev);

/* Single-message MSI / first MSI-X entry. Returns 0 on success. */
int  pci_enable_msi (struct pci_device *dev, uint8_t preferred_cpu,
                     struct hal_msi_message *out_msg);
int  pci_enable_msix(struct pci_device *dev, uint8_t preferred_cpu,
                     struct hal_msi_message *out_msg);
void pci_disable_msi(struct pci_device *dev);

void pci_dump_device(const struct pci_device *dev);
void pci_dump_all(void);

#ifdef __cplusplus
}
#endif

#endif /* TIGX_PCI_H */

/* Route device interrupts to a specific CPU via MSI (preferred) or MSI-X.
 * Registers IDT handler for the allocated vector. Returns vector or -1. */
struct interrupt_frame;
int pci_route_msi(struct pci_device *dev, uint8_t preferred_cpu,
                  void (*handler)(struct interrupt_frame *));
