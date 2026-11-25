/* hal.h — Hardware Abstraction Layer (TigxOS 1.3.8)
 *
 * Thin face between the rest of the kernel and machine-specific details.
 * Drivers and policy stay out of the core kernel.
 *
 *  - Machine identity (arch, vendor, CPU count, APIC/MSI presence)
 *  - Simple device registry
 *  - MSI / MSI-X message construction (uses existing LAPIC)
 */
#ifndef TIGX_HAL_H
#define TIGX_HAL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Machine info                                                       */
/* ------------------------------------------------------------------ */

enum {
    HAL_ARCH_I386   = 1,
    HAL_ARCH_X86_64 = 2
};

struct hal_machine_info {
    int         arch;               /* HAL_ARCH_* */
    uint32_t    cpu_count;
    uint32_t    bsp_apic_id;
    int         has_apic;
    int         has_x2apic;
    int         has_msi;
    int         has_msi_x;
    const char *vendor;
    const char *model;
};

const struct hal_machine_info *hal_machine(void);

void hal_early_init(void);          /* after GDT/IDT, before scheduler */
void hal_late_init(void);           /* after ACPI + LAPIC + SMP */

/* ------------------------------------------------------------------ */
/* Device registry                                                    */
/* ------------------------------------------------------------------ */

enum {
    HAL_DEV_PCI    = 1,
    HAL_DEV_LEGACY = 2,
    HAL_DEV_VIRTIO = 3
};

struct hal_device {
    uint32_t    id;                 /* 0 = invalid */
    int         class;              /* HAL_DEV_* */
    uint16_t    vendor;
    uint16_t    device;
    uint8_t     bus;
    uint8_t     slot;
    uint8_t     func;
    void       *driver_data;
    const char *name;
};

#define HAL_MAX_DEVICES 128

uint32_t              hal_device_register(const struct hal_device *dev);
const struct hal_device *hal_device_get(uint32_t id);
int                   hal_device_foreach(int (*fn)(const struct hal_device *, void *),
                                         void *ctx);

/* ------------------------------------------------------------------ */
/* MSI / MSI-X helpers                                                */
/* ------------------------------------------------------------------ */

struct hal_msi_message {
    uint64_t address;
    uint32_t data;
    uint8_t  vector;
    uint8_t  cpu;
};

/* Allocate one system vector and fill *out. preferred_cpu == 0xff → BSP.
 * Returns 0 on success, -1 on failure. */
int  hal_msi_allocate(uint8_t preferred_cpu, struct hal_msi_message *out);
void hal_msi_free(uint8_t vector);

/* ------------------------------------------------------------------ */
/* Debug                                                              */
/* ------------------------------------------------------------------ */

void hal_dump_machine(void);
void hal_dump_devices(void);

#ifdef __cplusplus
}
#endif

#endif /* TIGX_HAL_H */
