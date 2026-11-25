/* ioapic.h — I/O APIC programming (TigxOS: closes 1.2.8 gap) */
#ifndef TIGX_IOAPIC_H
#define TIGX_IOAPIC_H
#include <stdint.h>
void ioapic_init(void);          /* after acpi + lapic; programs IRQ0-15 → LAPIC */
int  ioapic_present(void);
void ioapic_set_irq(int irq, uint8_t vector, int masked);
#endif
