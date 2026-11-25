#ifndef TIGX_HPET_H
#define TIGX_HPET_H
#include <stdint.h>
void hpet_init(void);
int  hpet_present(void);
uint64_t hpet_read_counter(void);
/* Busy-wait approx ns using HPET if present, else PIT-ish delay */
void hpet_udelay(uint32_t us);
#endif
