#ifndef TIGX_AHCI_H
#define TIGX_AHCI_H
#include <stdint.h>
void ahci_init(void);
int  ahci_present(void);
int  ahci_port_count(void);
/* Block-layer style: read/write 512-byte sectors on first active port. */
int  ahci_read(uint64_t lba, uint32_t count, void *buf);
int  ahci_write(uint64_t lba, uint32_t count, const void *buf);
#endif
