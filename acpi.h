/* acpi.h — ACPI table discovery, power control, MADT (TigxOS 1.3.8) */
#ifndef TIGX_ACPI_H
#define TIGX_ACPI_H

#include <stdint.h>
#include <stddef.h>

#define ACPI_MAX_CPUS      16
#define ACPI_MAX_IOAPICS   4
#define ACPI_MAX_OVERRIDES 16

struct acpi_cpu {
    uint8_t  apic_id;
    uint8_t  enabled;       /* 1 = usable */
    uint8_t  is_bsp;        /* 1 = bootstrap processor */
};

struct acpi_ioapic {
    uint8_t  id;
    uint32_t addr;          /* physical MMIO base */
    uint32_t gsi_base;
};

struct acpi_irq_override {
    uint8_t  bus;           /* 0 = ISA */
    uint8_t  source;        /* IRQ */
    uint32_t gsi;
    uint16_t flags;
};

struct acpi_info {
    int      present;
    int      cpu_count;
    int      ioapic_count;
    int      override_count;
    uint32_t lapic_addr;    /* default 0xFEE00000 */
    uint64_t hpet_addr;     /* 0 if unknown; from HPET table or fallback */
    uint16_t pm1a_cnt;      /* PM1a control port */
    uint16_t pm1b_cnt;
    uint8_t  slp_typa;      /* S5 sleep type from _S5_ */
    uint8_t  slp_typb;
    uint8_t  has_s5;
    uint8_t  reset_reg_space; /* 1 = SystemIO */
    uint8_t  reset_value;
    uint16_t reset_reg;     /* port if SystemIO */
    struct acpi_cpu          cpus[ACPI_MAX_CPUS];
    struct acpi_ioapic       ioapics[ACPI_MAX_IOAPICS];
    struct acpi_irq_override overrides[ACPI_MAX_OVERRIDES];
};

void acpi_init(void);
const struct acpi_info *acpi_get(void);

/* Power control (fall back to QEMU/legacy if ACPI incomplete) */
void acpi_poweroff(void) __attribute__((noreturn));
void acpi_reboot(void)   __attribute__((noreturn));

#endif
