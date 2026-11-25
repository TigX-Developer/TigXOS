/* usb.h — USB subsystem (TigxOS 1.3.8: control + HID/MSC) */
#ifndef TIGX_USB_H
#define TIGX_USB_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define USB_MAX_DEVS 16

enum usb_speed {
    USB_SPEED_UNKNOWN = 0,
    USB_SPEED_FULL,
    USB_SPEED_LOW,
    USB_SPEED_HIGH,
    USB_SPEED_SUPER,
};

enum usb_class {
    USB_CLASS_UNKNOWN = 0,
    USB_CLASS_HID,
    USB_CLASS_MASS_STORAGE,
    USB_CLASS_HUB,
    USB_CLASS_OTHER,
};

struct usb_device {
    int      present;
    int      slot_id;
    int      port;
    int      speed;
    int      class_code;
    int      subclass;
    int      protocol;      /* HID boot: 1=kbd 2=mouse; MSC: 0x50=BOT */
    uint16_t id_vendor;
    uint16_t id_product;
    uint8_t  addr;          /* USB address 1..127 */
    char     product[32];
};

void usb_init(void);
int  usb_controller_present(void);
int  usb_device_count(void);
const struct usb_device *usb_device_at(int index);
void usb_dump(void);

void usb_hid_poll(void);
int  usb_msc_block_count(void);
void usb_msc_register_blocks(void);
int  usb_msc_read(int dev_index, uint64_t lba, uint32_t count, void *buf);
int  usb_msc_write(int dev_index, uint64_t lba, uint32_t count, const void *buf);

/* Used by xhci port scan */
void usb_note_port(int port, int speed);

#ifdef __cplusplus
}
#endif

#endif
