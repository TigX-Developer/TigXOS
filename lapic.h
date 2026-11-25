/* lapic.h — Local APIC (TigxOS 1.3.8-update3)
 *
 * BSP-only foundation. Application processors park after SIPI until 1.2.9.
 * On i386 the LAPIC MMIO window sits above the 1 GB identity map, so the
 * driver is a no-op there and the kernel stays UP.
 */
#ifndef TIGX_LAPIC_H
#define TIGX_LAPIC_H

#include <stdint.h>

#define LAPIC_DEFAULT_ADDR  0xFEE00000UL

/* Register offsets (Intel SDM Vol. 3) */
#define LAPIC_ID            0x020
#define LAPIC_VER           0x030
#define LAPIC_TPR           0x080
#define LAPIC_EOI           0x0B0
#define LAPIC_SVR           0x0F0
#define LAPIC_ICR_LOW       0x300
#define LAPIC_ICR_HIGH      0x310
#define LAPIC_LVT_TIMER     0x320
#define LAPIC_LVT_LINT0     0x350
#define LAPIC_LVT_LINT1     0x360
#define LAPIC_LVT_ERROR     0x370
#define LAPIC_TIMER_INIT    0x380
#define LAPIC_TIMER_CUR     0x390
#define LAPIC_TIMER_DIV     0x3E0

#define LAPIC_SVR_ENABLE    (1u << 8)
#define LAPIC_ICR_INIT      0x00000500u
#define LAPIC_ICR_STARTUP   0x00000600u
#define LAPIC_ICR_ASSERT    0x00004000u
#define LAPIC_ICR_LEVEL     0x00008000u
#define LAPIC_ICR_DELIVS    (1u << 12)
#define LAPIC_LVT_MASKED    (1u << 16)

void     lapic_init(void);          /* BSP only; safe to call on i386 (no-op) */
void     lapic_eoi(void);
uint32_t lapic_id(void);
void     lapic_send_ipi(uint8_t apic_id, uint32_t flags);
void     lapic_start_ap(uint8_t apic_id, uint32_t trampoline_page);
int      lapic_present(void);
uintptr_t lapic_base(void);
uint32_t lapic_version(void);       /* 0 if not present */
void     lapic_ap_init(void);       /* enable LAPIC on an AP */
void     lapic_timer_enable(uint32_t initial_count, uint8_t vector);
void     lapic_timer_disable(void);

#endif /* TIGX_LAPIC_H */
