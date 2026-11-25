#ifndef TIGX_XHCI_H
#define TIGX_XHCI_H

#include <stdint.h>
#include "pci.h"

#define PCI_CLASS_SERIAL     0x0C
#define PCI_SUBCLASS_USB     0x03
#define PCI_PROG_XHCI        0x30

int  xhci_init(void);
int  xhci_present(void);
int  xhci_port_count(void);
int  xhci_enumerate_ports(void);
void xhci_dump(void);

/* Control transfer on default control endpoint of addressed device.
 * Returns bytes transferred or <0 on error. slot 1..max, ep0. */
int  xhci_control(int slot, uint8_t bmRequestType, uint8_t bRequest,
                  uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                  void *data);
int  xhci_enable_slot(void);          /* returns slot id or <0 */
int  xhci_address_device(int slot, int port, int speed);
/* Bulk transfer on endpoint DCI (2=OUT1, 3=IN1 typical). */
int  xhci_config_bulk_eps(int slot, uint8_t ep_out, uint8_t ep_in, uint16_t mps);
int  xhci_bulk(int slot, int dci, int dir_in, void *data, uint32_t len);

#endif
