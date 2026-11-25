/* usb.c — USB xHCI, HID (keyboard/mouse), MSC (TigxOS 1.3.8) */
#include "usb.h"
#include "xhci.h"
#include "console.h"
#include "log.h"
#include "string.h"
#include "keyboard.h"
#include "mouse.h"
#include "block.h"
#include "memory.h"
#include "timer.h"

static struct usb_device g_devs[USB_MAX_DEVS];
static int g_ndev;
static int g_ctrl;
static int g_msc_blocks;
static uint8_t g_bulk_out[USB_MAX_DEVS];
static uint8_t g_bulk_in[USB_MAX_DEVS];
static uint16_t g_bulk_mps[USB_MAX_DEVS];
static int g_dci_out[USB_MAX_DEVS];
static int g_dci_in[USB_MAX_DEVS];


/* HID Usage ID → ASCII (US QWERTY). Index is boot-protocol keycode. */
static const char hid_plain[128] = {
    [0x04] = 'a', [0x05] = 'b', [0x06] = 'c', [0x07] = 'd', [0x08] = 'e', [0x09] = 'f',
    [0x0A] = 'g', [0x0B] = 'h', [0x0C] = 'i', [0x0D] = 'j', [0x0E] = 'k', [0x0F] = 'l',
    [0x10] = 'm', [0x11] = 'n', [0x12] = 'o', [0x13] = 'p', [0x14] = 'q', [0x15] = 'r',
    [0x16] = 's', [0x17] = 't', [0x18] = 'u', [0x19] = 'v', [0x1A] = 'w', [0x1B] = 'x',
    [0x1C] = 'y', [0x1D] = 'z', [0x1E] = '1', [0x1F] = '2', [0x20] = '3', [0x21] = '4',
    [0x22] = '5', [0x23] = '6', [0x24] = '7', [0x25] = '8', [0x26] = '9', [0x27] = '0',
    [0x28] = '\n', [0x29] = '\x1b', [0x2A] = '\b', [0x2B] = '\t', [0x2C] = ' ',
    [0x2D] = '-', [0x2E] = '=', [0x2F] = '[', [0x30] = ']', [0x31] = '\\',
    [0x33] = ';', [0x34] = '\'', [0x35] = '`', [0x36] = ',', [0x37] = '.', [0x38] = '/',
};
static const char hid_shift[128] = {
    [0x04] = 'A', [0x05] = 'B', [0x06] = 'C', [0x07] = 'D', [0x08] = 'E', [0x09] = 'F',
    [0x0A] = 'G', [0x0B] = 'H', [0x0C] = 'I', [0x0D] = 'J', [0x0E] = 'K', [0x0F] = 'L',
    [0x10] = 'M', [0x11] = 'N', [0x12] = 'O', [0x13] = 'P', [0x14] = 'Q', [0x15] = 'R',
    [0x16] = 'S', [0x17] = 'T', [0x18] = 'U', [0x19] = 'V', [0x1A] = 'W', [0x1B] = 'X',
    [0x1C] = 'Y', [0x1D] = 'Z', [0x1E] = '!', [0x1F] = '@', [0x20] = '#', [0x21] = '$',
    [0x22] = '%', [0x23] = '^', [0x24] = '&', [0x25] = '*', [0x26] = '(', [0x27] = ')',
    [0x28] = '\n', [0x29] = '\x1b', [0x2A] = '\b', [0x2B] = '\t', [0x2C] = ' ',
    [0x2D] = '_', [0x2E] = '+', [0x2F] = '{', [0x30] = '}', [0x31] = '|',
    [0x33] = ':', [0x34] = '"', [0x35] = '~', [0x36] = '<', [0x37] = '>', [0x38] = '?',
};

/* Last report per device + key-repeat state (1.3.8). */
static uint8_t hid_prev[USB_MAX_DEVS][8];
static uint8_t hid_rep_id[USB_MAX_DEVS];
static uint32_t hid_rep_tick[USB_MAX_DEVS];
#define HID_REPEAT_DELAY_TICKS  40   /* ~400 ms at 100 Hz before first repeat */
#define HID_REPEAT_RATE_TICKS   5    /* ~50 ms between repeats */

void usb_note_port(int port, int speed)
{
    if (g_ndev >= USB_MAX_DEVS)
        return;
    struct usb_device *d = &g_devs[g_ndev++];
    memset(d, 0, sizeof(*d));
    d->present = 1;
    d->port = port;
    d->speed = speed;
    d->class_code = USB_CLASS_UNKNOWN;
    strncpy(d->product, "usb-port", sizeof(d->product) - 1);
}


static void parse_bulk_eps(struct usb_device *d, int di, const uint8_t *cfg, int cfg_len)
{
    (void)d;
    g_bulk_out[di] = 0;
    g_bulk_in[di] = 0;
    g_bulk_mps[di] = 512;
    int off = 0;
    while (off + 2 <= cfg_len) {
        uint8_t len = cfg[off];
        uint8_t ty = cfg[off + 1];
        if (len < 2) break;
        if (ty == 0x05 && len >= 7) { /* endpoint */
            uint8_t addr = cfg[off + 2];
            uint8_t attr = cfg[off + 3] & 0x03;
            uint16_t mps = (uint16_t)cfg[off + 4] | ((uint16_t)cfg[off + 5] << 8);
            if (attr == 2) { /* bulk */
                if (addr & 0x80) {
                    g_bulk_in[di] = addr;
                    g_bulk_mps[di] = mps ? mps : 512;
                } else {
                    g_bulk_out[di] = addr;
                    if (mps) g_bulk_mps[di] = mps;
                }
            }
        }
        off += len;
    }
    if (g_bulk_out[di] && g_bulk_in[di]) {
        int on = g_bulk_out[di] & 0x0F;
        int in = g_bulk_in[di] & 0x0F;
        g_dci_out[di] = on * 2;
        g_dci_in[di] = in * 2 + 1;
        if (g_dci_out[di] < 2) g_dci_out[di] = 2;
        if (g_dci_in[di] < 3) g_dci_in[di] = 3;
    }
}

static int get_desc(int slot, uint16_t type_index, void *buf, uint16_t len)
{
    return xhci_control(slot, 0x80, 0x06, type_index, 0, len, buf);
}

static void classify_from_desc(struct usb_device *d, const uint8_t *dev,
                               const uint8_t *cfg, int cfg_len)
{
    if (dev && dev[0] >= 18) {
        d->id_vendor = (uint16_t)dev[8] | ((uint16_t)dev[9] << 8);
        d->id_product = (uint16_t)dev[10] | ((uint16_t)dev[11] << 8);
    }
    if (!cfg || cfg_len < 9)
        return;
    int off = 0;
    while (off + 2 <= cfg_len) {
        uint8_t len = cfg[off];
        uint8_t ty = cfg[off + 1];
        if (len < 2)
            break;
        if (ty == 0x04 && len >= 9) {
            uint8_t cls = cfg[off + 5];
            uint8_t sub = cfg[off + 6];
            uint8_t proto = cfg[off + 7];
            d->subclass = sub;
            d->protocol = proto;
            if (cls == 0x03) {
                d->class_code = USB_CLASS_HID;
                if (proto == 1)
                    strncpy(d->product, "hid-keyboard", sizeof(d->product) - 1);
                else if (proto == 2)
                    strncpy(d->product, "hid-mouse", sizeof(d->product) - 1);
                else
                    strncpy(d->product, "hid", sizeof(d->product) - 1);
            } else if (cls == 0x08) {
                d->class_code = USB_CLASS_MASS_STORAGE;
                strncpy(d->product, "msc", sizeof(d->product) - 1);
            } else if (cls == 0x09) {
                d->class_code = USB_CLASS_HUB;
                strncpy(d->product, "hub", sizeof(d->product) - 1);
            }
        }
        off += len;
    }
}

static void try_enumerate(struct usb_device *d)
{
    if (!xhci_present())
        return;
    int xspeed = 1;
    if (d->speed == USB_SPEED_LOW)
        xspeed = 2;
    else if (d->speed == USB_SPEED_HIGH)
        xspeed = 3;
    else if (d->speed == USB_SPEED_SUPER)
        xspeed = 4;

    int slot = xhci_enable_slot();
    if (slot < 0) {
        pr_warn("usb: Enable Slot failed port %d\n", d->port);
        return;
    }
    d->slot_id = slot;
    if (xhci_address_device(slot, d->port, xspeed) < 0) {
        pr_warn("usb: Address Device failed slot %d\n", slot);
        return;
    }
    d->addr = (uint8_t)slot;

    uint8_t *dev = (uint8_t *)pmm_alloc_frames(1);
    uint8_t *cfg = (uint8_t *)pmm_alloc_frames(1);
    if (!dev || !cfg)
        return;
    memset(dev, 0, 64);
    memset(cfg, 0, 256);

    int n = get_desc(slot, 0x0100, dev, 18);
    if (n < 8) {
        pr_warn("usb: GET_DESCRIPTOR device failed slot %d\n", slot);
        return;
    }
    n = get_desc(slot, 0x0200, cfg, 9);
    uint16_t total = 9;
    if (n >= 9)
        total = (uint16_t)cfg[2] | ((uint16_t)cfg[3] << 8);
    if (total > 256)
        total = 256;
    if (total > 9)
        get_desc(slot, 0x0200, cfg, total);

    classify_from_desc(d, dev, cfg, (int)total);
    int di = (int)(d - g_devs);
    parse_bulk_eps(d, di, cfg, (int)total);
    pr_info("usb: slot %d port %d vid=%04x pid=%04x class=%d %s\n",
            slot, d->port, d->id_vendor, d->id_product, d->class_code, d->product);

    if (cfg[5])
        xhci_control(slot, 0x00, 0x09, cfg[5], 0, 0, NULL);

    if (d->class_code == USB_CLASS_HID && d->protocol == 1) {
        xhci_control(slot, 0x21, 0x0A, 0, 0, 0, NULL); /* SET_IDLE */
        xhci_control(slot, 0x21, 0x0B, 0, 0, 0, NULL); /* SET_PROTOCOL boot */
        pr_info("usb: HID boot keyboard configured\n");
    }
    if (d->class_code == USB_CLASS_MASS_STORAGE) {
        g_msc_blocks++;
        int di = (int)(d - g_devs);
        if (g_bulk_out[di] && g_bulk_in[di]) {
            if (xhci_config_bulk_eps(slot, g_bulk_out[di], g_bulk_in[di], g_bulk_mps[di]) == 0)
                pr_info("usb: MSC BOT bulk ready out=0x%x in=0x%x\n",
                        g_bulk_out[di], g_bulk_in[di]);
            else
                pr_warn("usb: MSC Configure Endpoint failed\n");
        } else {
            pr_warn("usb: MSC missing bulk endpoints\n");
        }
    }
}

void usb_init(void)
{
    memset(g_devs, 0, sizeof(g_devs));
    memset(hid_prev, 0, sizeof(hid_prev));
    g_ndev = 0;
    g_ctrl = 0;
    g_msc_blocks = 0;

    if (xhci_init() == 0 && xhci_present()) {
        g_ctrl = 1;
        int ports = xhci_enumerate_ports();
        pr_info("usb: xHCI up, %d connected port(s)\n", ports);
        for (int i = 0; i < g_ndev; i++)
            try_enumerate(&g_devs[i]);
        usb_msc_register_blocks();
    } else {
        pr_info("usb: no host controller\n");
    }
}

int usb_controller_present(void) { return g_ctrl; }
int usb_device_count(void) { return g_ndev; }

const struct usb_device *usb_device_at(int i)
{
    return (i >= 0 && i < g_ndev) ? &g_devs[i] : NULL;
}

void usb_dump(void)
{
    kprintf("usb: ctrl=%s devs=%d msc=%d\n",
            g_ctrl ? "xHCI" : "none", g_ndev, g_msc_blocks);
    if (g_ctrl)
        xhci_dump();
    for (int i = 0; i < g_ndev; i++) {
        const struct usb_device *d = &g_devs[i];
        kprintf("  [%d] port=%d slot=%d %04x:%04x %s\n",
                i, d->port, d->slot_id, d->id_vendor, d->id_product, d->product);
    }
}



void usb_hid_poll(void)
{
    extern uint32_t timer_get_ticks(void);
    uint32_t now = timer_get_ticks();

    for (int i = 0; i < g_ndev; i++) {
        struct usb_device *d = &g_devs[i];
        if (d->class_code != USB_CLASS_HID || d->slot_id <= 0)
            continue;

        if (d->protocol == 1) {
            /* boot keyboard — edge + typematic repeat (1.3.8) */
            uint8_t report[8];
            memset(report, 0, sizeof(report));
            if (xhci_control(d->slot_id, 0xA1, 0x01, 0x0100, 0, 8, report) < 0)
                continue;
            int shift = (report[0] & 0x22) != 0; /* L/R Shift */
            int caps = 0; /* Caps handled via shift map when Shift held */
            const char *map = shift ? hid_shift : hid_plain;

            /* Fresh key-downs */
            for (int k = 2; k < 8; k++) {
                uint8_t id = report[k];
                if (!id || id >= 128)
                    continue;
                int held = 0;
                for (int p = 2; p < 8; p++) {
                    if (hid_prev[i][p] == id) { held = 1; break; }
                }
                if (held)
                    continue;
                char c = map[id];
                if (c)
                    keyboard_inject_char(c);
                hid_rep_id[i] = id;
                hid_rep_tick[i] = now;
            }

            /* Key still held → typematic */
            if (hid_rep_id[i]) {
                int still = 0;
                for (int k = 2; k < 8; k++) {
                    if (report[k] == hid_rep_id[i]) { still = 1; break; }
                }
                if (!still) {
                    hid_rep_id[i] = 0;
                } else {
                    uint32_t elapsed = now - hid_rep_tick[i];
                    if (elapsed >= HID_REPEAT_DELAY_TICKS) {
                        uint32_t since = elapsed - HID_REPEAT_DELAY_TICKS;
                        if (since % HID_REPEAT_RATE_TICKS == 0) {
                            char c = map[hid_rep_id[i]];
                            if (c)
                                keyboard_inject_char(c);
                        }
                    }
                }
            }

            memcpy(hid_prev[i], report, 8);
            (void)caps;
        } else if (d->protocol == 2) {
            /* boot mouse */
            uint8_t report[4];
            memset(report, 0, sizeof(report));
            if (xhci_control(d->slot_id, 0xA1, 0x01, 0x0100, 0, 3, report) < 0)
                continue;
            int8_t dx = (int8_t)report[1];
            int8_t dy = (int8_t)report[2];
            if (dx || dy || (report[0] & 7))
                mouse_inject(dx, dy, report[0] & 7);
        }
    }
}


static int msc_blk_read(struct block_dev *dev, uint64_t lba, uint32_t count, void *buf)
{
    int di = (int)(uintptr_t)dev->priv;
    return usb_msc_read(di, lba, count, buf);
}
static int msc_blk_write(struct block_dev *dev, uint64_t lba, uint32_t count, const void *buf)
{
    int di = (int)(uintptr_t)dev->priv;
    return usb_msc_write(di, lba, count, buf);
}

void usb_msc_register_blocks(void)
{
    for (int i = 0; i < g_ndev; i++) {
        if (g_devs[i].class_code != USB_CLASS_MASS_STORAGE) continue;
        if (!g_bulk_out[i] || !g_dci_out[i]) continue;
        struct block_dev bd;
        memset(&bd, 0, sizeof(bd));
        bd.type = BLOCK_DEV_AHCI; /* treat as removable */
        bd.present = 1;
        bd.sectors = 0x100000;
        bd.read = msc_blk_read;
        bd.write = msc_blk_write;
        bd.priv = (void *)(uintptr_t)i;
        strncpy(bd.name, "usb0", sizeof(bd.name) - 1);
        if (i > 0) {
            bd.name[3] = (char)('0' + (i % 10));
        }
        int id = block_register(&bd);
        if (id >= 0)
            pr_info("usb: registered block %s id=%d\n", bd.name, id);
    }
}

int usb_msc_block_count(void) { return g_msc_blocks; }


/* BOT SCSI READ(10)/WRITE(10) — best-effort via control+bulk when EP rings exist.
 * Full bulk EP configuration is still incomplete on some QEMU builds; returns -1
 * when the path is not ready so callers fall back to ATA. */
/* BOT CBW/CSW structures */
struct usb_cbw {
    uint32_t sig;
    uint32_t tag;
    uint32_t data_len;
    uint8_t  flags;
    uint8_t  lun;
    uint8_t  cblen;
    uint8_t  cb[16];
} __attribute__((packed));

struct usb_csw {
    uint32_t sig;
    uint32_t tag;
    uint32_t residue;
    uint8_t  status;
} __attribute__((packed));

static int msc_bot_xfer(int di, int slot, int dir_in, uint64_t lba, uint32_t count, void *buf)
{
    struct usb_cbw cbw;
    struct usb_csw csw;
    memset(&cbw, 0, sizeof(cbw));
    memset(&csw, 0, sizeof(csw));
    cbw.sig = 0x43425355; /* USBC */
    cbw.tag = 1;
    cbw.data_len = count * 512;
    cbw.flags = dir_in ? 0x80 : 0;
    cbw.lun = 0;
    cbw.cblen = 10;
    cbw.cb[0] = dir_in ? 0x28 : 0x2A; /* READ(10)/WRITE(10) */
    cbw.cb[2] = (uint8_t)(lba >> 24);
    cbw.cb[3] = (uint8_t)(lba >> 16);
    cbw.cb[4] = (uint8_t)(lba >> 8);
    cbw.cb[5] = (uint8_t)lba;
    cbw.cb[7] = (uint8_t)(count >> 8);
    cbw.cb[8] = (uint8_t)count;

    int dci_out = g_dci_out[di] ? g_dci_out[di] : 2;
    int dci_in  = g_dci_in[di]  ? g_dci_in[di]  : 3;
    if (xhci_bulk(slot, dci_out, 0, &cbw, 31) < 0)
        return -1;
    if (count && buf) {
        int dci = dir_in ? dci_in : dci_out;
        if (xhci_bulk(slot, dci, dir_in, buf, count * 512) < 0)
            return -1;
    }
    if (xhci_bulk(slot, dci_in, 1, &csw, 13) < 0)
        return -1;
    if (csw.sig != 0x53425355 || csw.status != 0)
        return -1;
    return 0;
}

int usb_msc_read(int dev_index, uint64_t lba, uint32_t count, void *buf)
{
    if (dev_index < 0 || dev_index >= g_ndev || !buf || !count)
        return -1;
    struct usb_device *d = &g_devs[dev_index];
    if (d->class_code != USB_CLASS_MASS_STORAGE || d->slot_id <= 0)
        return -1;
    return msc_bot_xfer(dev_index, d->slot_id, 1, lba, count, buf);
}
int usb_msc_write(int dev_index, uint64_t lba, uint32_t count, const void *buf)
{
    if (dev_index < 0 || dev_index >= g_ndev || !buf || !count)
        return -1;
    struct usb_device *d = &g_devs[dev_index];
    if (d->class_code != USB_CLASS_MASS_STORAGE || d->slot_id <= 0)
        return -1;
    return msc_bot_xfer(dev_index, d->slot_id, 0, lba, count, (void *)buf);
}

