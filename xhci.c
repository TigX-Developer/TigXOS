/* xhci.c — xHCI + control transfers (TigxOS 1.3.8)
 *
 * QEMU: -device qemu-xhci [-device usb-kbd] [-device usb-mouse] [-device usb-storage,drive=...]
 */
#include "xhci.h"
#include "usb.h"
#include "console.h"
#include "log.h"
#include "memory.h"
#include "pci.h"
#include "string.h"
#include <stdint.h>

#define XHCI_CAPLENGTH   0x00
#define XHCI_HCIVERSION  0x02
#define XHCI_HCSPARAMS1  0x04
#define XHCI_DBOFF       0x14
#define XHCI_RTSOFF      0x18
#define XHCI_USBCMD      0x00
#define XHCI_USBSTS      0x04
#define XHCI_CRCR        0x18
#define XHCI_DCBAAP      0x30
#define XHCI_CONFIG      0x38
#define XHCI_PORTSC(n)   (0x400 + 0x10 * ((n) - 1))

#define USBCMD_RS        (1u << 0)
#define USBCMD_HCRST     (1u << 1)
#define USBCMD_INTE      (1u << 2)
#define USBSTS_HCH       (1u << 0)
#define USBSTS_CNR       (1u << 11)
#define USBSTS_EINT      (1u << 3)

#define PORTSC_CCS       (1u << 0)
#define PORTSC_PED       (1u << 1)
#define PORTSC_PR        (1u << 4)
#define PORTSC_PP        (1u << 9)
#define PORTSC_SPEED_SHIFT 10
#define PORTSC_SPEED_MASK  (0xFu << 10)
#define PORTSC_CSC       (1u << 17)
#define PORTSC_PRC       (1u << 21)

#define XHCI_MAX_PORTS   16
#define XHCI_RING_TRBS   32
#define XHCI_MAX_SLOTS   16

/* TRB types */
#define TRB_NORMAL       1
#define TRB_SETUP        2
#define TRB_DATA         3
#define TRB_STATUS       4
#define TRB_LINK         6
#define TRB_ENABLE_SLOT  9
#define TRB_DISABLE_SLOT 10
#define TRB_ADDRESS_DEV  11
#define TRB_CONFIG_EP    12
#define TRB_CMD_COMPLETE 33
#define TRB_TRANSFER     32

struct xhci {
    int present;
    volatile uint8_t *mmio, *op, *db, *rt;
    uint8_t max_slots, max_ports, bus, slot, func;
    uint64_t *dcbaa;
    uint32_t *cmd_ring;
    uint32_t *evt_ring;
    uint64_t *erst;
    int cmd_cycle, cmd_idx;
    int evt_cycle, evt_idx;
    /* per-slot device contexts + ep0 transfer rings (identity DMA) */
    uint8_t *dev_ctx[XHCI_MAX_SLOTS + 1];
    uint32_t *ep0_ring[XHCI_MAX_SLOTS + 1];
    int ep0_cycle[XHCI_MAX_SLOTS + 1];
    int ep0_idx[XHCI_MAX_SLOTS + 1];
    /* bulk EP rings: index by DCI 2..31 */
    uint32_t *ep_ring[XHCI_MAX_SLOTS + 1][32];
    int ep_cycle[XHCI_MAX_SLOTS + 1][32];
    int ep_idx[XHCI_MAX_SLOTS + 1][32];
    int bulk_ok[XHCI_MAX_SLOTS + 1];
};

static struct xhci g;

static inline uint32_t rr32(volatile uint8_t *b, uint32_t o) {
    return *(volatile uint32_t *)(b + o);
}
static inline void wr32(volatile uint8_t *b, uint32_t o, uint32_t v) {
    *(volatile uint32_t *)(b + o) = v;
}
static inline void wr64(volatile uint8_t *b, uint32_t o, uint64_t v) {
    *(volatile uint64_t *)(b + o) = v;
}

static void *dma_pages(size_t n) {
    void *p = pmm_alloc_frames(n ? n : 1);
    if (p) memset(p, 0, (n ? n : 1) * PAGE_SIZE);
    return p;
}

static void trb_set(uint32_t *trb, uint64_t ptr, uint32_t status, uint32_t control) {
    trb[0] = (uint32_t)ptr;
    trb[1] = (uint32_t)(ptr >> 32);
    trb[2] = status;
    trb[3] = control;
}

static int wait_event(uint32_t expect_type, uint32_t *out_code, uint32_t *out_slot, int spins) {
    for (int i = 0; i < spins; i++) {
        uint32_t *trb = g.evt_ring + g.evt_idx * 4;
        uint32_t ctrl = trb[3];
        int c = ctrl & 1;
        if (c != g.evt_cycle) {
            __asm__ volatile("pause");
            continue;
        }
        uint32_t type = (ctrl >> 10) & 0x3F;
        uint32_t code = (trb[2] >> 24) & 0xFF;
        uint32_t slot = (ctrl >> 24) & 0xFF;
        /* advance ERDP */
        g.evt_idx++;
        if (g.evt_idx >= XHCI_RING_TRBS) {
            g.evt_idx = 0;
            g.evt_cycle ^= 1;
        }
        {
            volatile uint8_t *ir0 = g.rt + 0x20;
            uint64_t erdp = (uint64_t)(uintptr_t)(g.evt_ring + g.evt_idx * 4);
            wr64(ir0, 0x18, erdp | 1);
        }
        /* clear EINT */
        wr32(g.op, XHCI_USBSTS, USBSTS_EINT);

        if (type == expect_type || expect_type == 0) {
            if (out_code) *out_code = code;
            if (out_slot) *out_slot = slot;
            return (int)type;
        }
        /* skip other events */
    }
    return -1;
}

static int cmd_submit(uint32_t *trb_words) {
    uint32_t *ring = g.cmd_ring + g.cmd_idx * 4;
    ring[0] = trb_words[0];
    ring[1] = trb_words[1];
    ring[2] = trb_words[2];
    ring[3] = (trb_words[3] & ~1u) | (uint32_t)g.cmd_cycle;

    g.cmd_idx++;
    if (g.cmd_idx >= XHCI_RING_TRBS - 1) {
        /* link TRB already at end */
        g.cmd_idx = 0;
        g.cmd_cycle ^= 1;
    }
    /* doorbell 0 = command ring */
    wr32(g.db, 0, 0);

    uint32_t code = 0, slot = 0;
    int t = wait_event(TRB_CMD_COMPLETE, &code, &slot, 2000000);
    if (t < 0) return -1;
    if (code != 1) /* Success */
        return -((int)code);
    return (int)slot;
}

int xhci_enable_slot(void) {
    uint32_t trb[4] = {0, 0, 0, (TRB_ENABLE_SLOT << 10)};
    return cmd_submit(trb);
}

int xhci_address_device(int slot, int port, int speed) {
    if (slot <= 0 || slot > g.max_slots) return -1;
    /* Input context: 32-byte headers + slot ctx + ep0 ctx = ~0x420 */
    uint8_t *in = (uint8_t *)dma_pages(1);
    if (!in) return -1;
    memset(in, 0, PAGE_SIZE);
    /* Input Control: A0 + A1 (slot + ep0) */
    ((uint32_t *)in)[1] = 0x3;

    uint8_t *slot_ctx = in + 0x40;
    /* Slot context: route string 0, speed, ctx entries = 1 (ep0 only) */
    uint32_t sc0 = ((uint32_t)1 << 27) | ((uint32_t)speed << 20);
    ((uint32_t *)slot_ctx)[0] = sc0;
    ((uint32_t *)slot_ctx)[1] = ((uint32_t)port << 16); /* root hub port */

    uint8_t *ep0 = in + 0x80;
    /* EP0: control, max packet 8/64 depending on speed, CErr=3, Type=Control */
    uint16_t mps = (speed == 2) ? 8 : 64; /* low=8 else 64 for FS/HS default */
    if (speed >= 3) mps = 64;
    ((uint32_t *)ep0)[1] = (3u << 16) | (4u << 3) | (mps << 16 >> 16); /* rough */
    ((uint32_t *)ep0)[1] = (3u << 1) | (4u << 3); /* CErr=3, EPType=4 control */
    ((uint32_t *)ep0)[1] |= ((uint32_t)mps << 16);
    /* Dequeue pointer = ep0 ring | DCS */
    if (!g.ep0_ring[slot]) {
        g.ep0_ring[slot] = (uint32_t *)dma_pages(1);
        g.ep0_cycle[slot] = 1;
        g.ep0_idx[slot] = 0;
        if (!g.ep0_ring[slot]) return -1;
        /* link at end */
        uint32_t *link = g.ep0_ring[slot] + (XHCI_RING_TRBS - 1) * 4;
        trb_set(link, (uint64_t)(uintptr_t)g.ep0_ring[slot], 0,
                (TRB_LINK << 10) | (1u << 1) | 1);
    }
    uint64_t dq = (uint64_t)(uintptr_t)g.ep0_ring[slot] | 1;
    ((uint32_t *)ep0)[2] = (uint32_t)dq;
    ((uint32_t *)ep0)[3] = (uint32_t)(dq >> 32);

    /* Output device context */
    if (!g.dev_ctx[slot]) {
        g.dev_ctx[slot] = (uint8_t *)dma_pages(1);
        if (!g.dev_ctx[slot]) return -1;
        g.dcbaa[slot] = (uint64_t)(uintptr_t)g.dev_ctx[slot];
    }

    uint32_t trb[4];
    trb_set(trb, (uint64_t)(uintptr_t)in, 0, (TRB_ADDRESS_DEV << 10) | ((uint32_t)slot << 24));
    int r = cmd_submit(trb);
    return r < 0 ? r : 0;
}

int xhci_control(int slot, uint8_t bmRequestType, uint8_t bRequest,
                 uint16_t wValue, uint16_t wIndex, uint16_t wLength, void *data) {
    if (slot <= 0 || !g.ep0_ring[slot]) return -1;

    uint8_t setup[8];
    setup[0] = bmRequestType;
    setup[1] = bRequest;
    setup[2] = (uint8_t)wValue;
    setup[3] = (uint8_t)(wValue >> 8);
    setup[4] = (uint8_t)wIndex;
    setup[5] = (uint8_t)(wIndex >> 8);
    setup[6] = (uint8_t)wLength;
    setup[7] = (uint8_t)(wLength >> 8);

    int idx = g.ep0_idx[slot];
    int cyc = g.ep0_cycle[slot];
    uint32_t *ring = g.ep0_ring[slot];

    /* Setup stage TRB */
    uint32_t *s = ring + idx * 4;
    memcpy(s, setup, 8);
    s[2] = 8; /* transfer length */
    s[3] = (TRB_SETUP << 10) | (3u << 16) | (1u << 6) | (uint32_t)cyc; /* IDT, TRT=3 if IN data */
    if (wLength == 0)
        s[3] = (TRB_SETUP << 10) | (0u << 16) | (1u << 6) | (uint32_t)cyc;
    else if (bmRequestType & 0x80)
        s[3] = (TRB_SETUP << 10) | (3u << 16) | (1u << 6) | (uint32_t)cyc; /* IN */
    else
        s[3] = (TRB_SETUP << 10) | (2u << 16) | (1u << 6) | (uint32_t)cyc; /* OUT */

    idx = (idx + 1) % (XHCI_RING_TRBS - 1);

    if (wLength && data) {
        uint32_t *d = ring + idx * 4;
        trb_set(d, (uint64_t)(uintptr_t)data, wLength,
                (TRB_DATA << 10) | ((bmRequestType & 0x80) ? (1u << 16) : 0) | (uint32_t)cyc);
        idx = (idx + 1) % (XHCI_RING_TRBS - 1);
    }

    /* Status stage */
    uint32_t *st = ring + idx * 4;
    uint32_t dir = (wLength == 0 || !(bmRequestType & 0x80)) ? (1u << 16) : 0; /* IN status if no data or OUT data */
    if (wLength && (bmRequestType & 0x80))
        dir = 0; /* OUT status after IN data */
    else
        dir = (1u << 16);
    trb_set(st, 0, 0, (TRB_STATUS << 10) | dir | (1u << 5) | (uint32_t)cyc); /* IOC */

    g.ep0_idx[slot] = (idx + 1) % (XHCI_RING_TRBS - 1);
    if (g.ep0_idx[slot] == 0)
        g.ep0_cycle[slot] ^= 1;

    /* doorbell: endpoint 1 = control (dci 1) */
    wr32(g.db, slot * 4, 1);

    uint32_t code = 0;
    int t = wait_event(TRB_TRANSFER, &code, NULL, 2000000);
    if (t < 0) return -1;
    if (code != 1 && code != 13 /* short packet */) return -((int)code);
    return (int)wLength;
}

static int map_bar(struct pci_device *dev) {
    if (!dev->bars[0].address || dev->bars[0].is_io) return -1;
    uintptr_t phys = (uintptr_t)dev->bars[0].address;
    size_t len = (size_t)dev->bars[0].size;
    if (len < 0x1000) len = 0x1000;
#if defined(__x86_64__)
    (void)vmm_map_mmio(phys, phys, len);
#endif
    g.mmio = (volatile uint8_t *)phys;
    return 0;
}

int xhci_init(void) {
    memset(&g, 0, sizeof(g));
    const struct pci_device *found = NULL;
    size_t n = pci_device_count();
    for (size_t i = 0; i < n; i++) {
        const struct pci_device *d = pci_device_at(i);
        if (d && d->class_code == PCI_CLASS_SERIAL && d->subclass == PCI_SUBCLASS_USB &&
            d->prog_if == PCI_PROG_XHCI) {
            found = d;
            break;
        }
    }
    if (!found) {
        pr_info("xhci: no controller (QEMU: -device qemu-xhci)\n");
        return 0;
    }
    struct pci_device mut = *found;
    pci_enable_device(&mut);
    g.bus = mut.bus; g.slot = mut.slot; g.func = mut.func;
    if (map_bar(&mut) != 0) return -1;

    uint8_t caplen = g.mmio[XHCI_CAPLENGTH];
    uint32_t hcs1 = rr32(g.mmio, XHCI_HCSPARAMS1);
    g.op = g.mmio + caplen;
    g.db = g.mmio + (rr32(g.mmio, XHCI_DBOFF) & ~3u);
    g.rt = g.mmio + (rr32(g.mmio, XHCI_RTSOFF) & ~0x1Fu);
    g.max_slots = (uint8_t)(hcs1 & 0xFF);
    g.max_ports = (uint8_t)((hcs1 >> 24) & 0xFF);
    if (g.max_slots > XHCI_MAX_SLOTS) g.max_slots = XHCI_MAX_SLOTS;
    if (g.max_ports > XHCI_MAX_PORTS) g.max_ports = XHCI_MAX_PORTS;

    pr_info("xhci: %02x:%02x.%x slots=%u ports=%u\n",
            g.bus, g.slot, g.func, g.max_slots, g.max_ports);

    if (!(rr32(g.op, XHCI_USBSTS) & USBSTS_HCH)) {
        wr32(g.op, XHCI_USBCMD, 0);
        for (int i = 0; i < 100000; i++)
            if (rr32(g.op, XHCI_USBSTS) & USBSTS_HCH) break;
    }
    wr32(g.op, XHCI_USBCMD, USBCMD_HCRST);
    for (int i = 0; i < 1000000; i++)
        if (!(rr32(g.op, XHCI_USBCMD) & USBCMD_HCRST) &&
            !(rr32(g.op, XHCI_USBSTS) & USBSTS_CNR)) break;

    wr32(g.op, XHCI_CONFIG, g.max_slots);
    g.dcbaa = (uint64_t *)dma_pages(1);
    if (!g.dcbaa) return -1;
    wr64(g.op, XHCI_DCBAAP, (uint64_t)(uintptr_t)g.dcbaa);

    g.cmd_ring = (uint32_t *)dma_pages(1);
    g.cmd_cycle = 1;
    g.cmd_idx = 0;
    trb_set(g.cmd_ring + (XHCI_RING_TRBS - 1) * 4,
            (uint64_t)(uintptr_t)g.cmd_ring, 0, (TRB_LINK << 10) | (1u << 1) | 1);
    wr64(g.op, XHCI_CRCR, (uint64_t)(uintptr_t)g.cmd_ring | 1);

    g.evt_ring = (uint32_t *)dma_pages(1);
    g.erst = (uint64_t *)dma_pages(1);
    g.evt_cycle = 1;
    g.evt_idx = 0;
    {
        uint32_t *e = (uint32_t *)g.erst;
        uint64_t er = (uint64_t)(uintptr_t)g.evt_ring;
        e[0] = (uint32_t)er; e[1] = (uint32_t)(er >> 32); e[2] = XHCI_RING_TRBS; e[3] = 0;
        volatile uint8_t *ir0 = g.rt + 0x20;
        wr32(ir0, 0x08, 1);
        wr64(ir0, 0x10, (uint64_t)(uintptr_t)g.erst);
        wr64(ir0, 0x18, er | 1);
        wr32(ir0, 0x00, 1);
    }

    wr32(g.op, XHCI_USBCMD, USBCMD_RS | USBCMD_INTE);
    for (int i = 0; i < 100000; i++)
        if (!(rr32(g.op, XHCI_USBSTS) & USBSTS_HCH)) break;

    g.present = 1;
    pr_info("xhci: running (control transfers ready)\n");
    return 0;
}

int xhci_present(void) { return g.present; }
int xhci_port_count(void) { return g.present ? g.max_ports : 0; }

int xhci_enumerate_ports(void) {
    if (!g.present) return 0;
    int n = 0;
    for (int p = 1; p <= g.max_ports; p++) {
        uint32_t sc = rr32(g.op, XHCI_PORTSC(p));
        uint32_t ch = sc & (PORTSC_CSC | PORTSC_PRC);
        if (ch) wr32(g.op, XHCI_PORTSC(p), (sc & PORTSC_PP) | ch);
        sc = rr32(g.op, XHCI_PORTSC(p));
        if (!(sc & PORTSC_CCS)) continue;
        n++;
        int speed = (sc & PORTSC_SPEED_MASK) >> PORTSC_SPEED_SHIFT;
        int us = USB_SPEED_UNKNOWN;
        if (speed == 1) us = USB_SPEED_FULL;
        else if (speed == 2) us = USB_SPEED_LOW;
        else if (speed == 3) us = USB_SPEED_HIGH;
        else if (speed >= 4) us = USB_SPEED_SUPER;
        wr32(g.op, XHCI_PORTSC(p), PORTSC_PP | PORTSC_PR);
        for (int i = 0; i < 500000; i++) {
            sc = rr32(g.op, XHCI_PORTSC(p));
            if (!(sc & PORTSC_PR)) break;
        }
        sc = rr32(g.op, XHCI_PORTSC(p));
        wr32(g.op, XHCI_PORTSC(p), (sc & PORTSC_PP) | PORTSC_PRC);
        usb_note_port(p, us);
        pr_info("xhci: port %d connected speed=%d\n", p, us);
    }
    return n;
}

void xhci_dump(void) {
    if (!g.present) { kprintf("xhci: absent\n"); return; }
    kprintf("xhci: ports=%u slots=%u\n", g.max_ports, g.max_slots);
}


/* Allocate a transfer ring for DCI (1=EP0, 2=EP1 OUT, 3=EP1 IN, ...) */
static int ensure_ep_ring(int slot, int dci)
{
    if (slot <= 0 || dci < 1 || dci >= 32) return -1;
    if (dci == 1) {
        if (!g.ep0_ring[slot]) return -1;
        return 0;
    }
    if (g.ep_ring[slot][dci]) return 0;
    g.ep_ring[slot][dci] = (uint32_t *)dma_pages(1);
    if (!g.ep_ring[slot][dci]) return -1;
    g.ep_cycle[slot][dci] = 1;
    g.ep_idx[slot][dci] = 0;
    uint32_t *link = g.ep_ring[slot][dci] + (XHCI_RING_TRBS - 1) * 4;
    trb_set(link, (uint64_t)(uintptr_t)g.ep_ring[slot][dci], 0,
            (TRB_LINK << 10) | (1u << 1) | 1);
    return 0;
}

/*
 * Configure bulk OUT + bulk IN endpoints after SET_CONFIGURATION.
 * ep_out/ep_in are USB endpoint addresses (e.g. 0x01 and 0x81).
 * DCI = (ep_num * 2) + (IN ? 1 : 0)  for ep_num>=1; EP0 is DCI1.
 */
int xhci_config_bulk_eps(int slot, uint8_t ep_out, uint8_t ep_in, uint16_t mps)
{
    if (!g.present || slot <= 0 || slot > g.max_slots) return -1;
    int out_num = ep_out & 0x0F;
    int in_num  = ep_in & 0x0F;
    int dci_out = out_num * 2;          /* OUT */
    int dci_in  = in_num * 2 + 1;       /* IN */
    if (dci_out < 2) dci_out = 2;
    if (dci_in < 3) dci_in = 3;
    if (mps == 0) mps = 512;

    if (ensure_ep_ring(slot, dci_out) < 0 || ensure_ep_ring(slot, dci_in) < 0)
        return -1;

    uint8_t *in = (uint8_t *)dma_pages(1);
    if (!in) return -1;
    memset(in, 0, PAGE_SIZE);
    /* Add flags: slot + both bulk EPs (bits dci) */
    uint32_t add = (1u << 0) | (1u << dci_out) | (1u << dci_in);
    ((uint32_t *)in)[1] = add;

    /* Slot context: Context Entries = max DCI */
    int ctx_entries = dci_in > dci_out ? dci_in : dci_out;
    uint8_t *slot_ctx = in + 0x40;
    if (g.dev_ctx[slot]) {
        /* copy existing slot ctx from output and bump entries */
        for (int i = 0; i < 16; i++)
            ((uint32_t *)slot_ctx)[i] = ((uint32_t *)g.dev_ctx[slot])[i];
    }
    uint32_t sc0 = ((uint32_t *)slot_ctx)[0];
    sc0 = (sc0 & ~(0x1Fu << 27)) | ((uint32_t)ctx_entries << 27);
    ((uint32_t *)slot_ctx)[0] = sc0;

    /* EP context helper */
    for (int pass = 0; pass < 2; pass++) {
        int dci = pass == 0 ? dci_out : dci_in;
        int bulk_type = pass == 0 ? 2 : 6; /* Bulk OUT=2, Bulk IN=6 */
        uint8_t *ep = in + 0x40 + (uint32_t)dci * 0x20;
        ((uint32_t *)ep)[0] = 0;
        ((uint32_t *)ep)[1] = (3u << 1) | ((uint32_t)bulk_type << 3) | ((uint32_t)mps << 16);
        uint64_t dq = (uint64_t)(uintptr_t)g.ep_ring[slot][dci] | 1;
        ((uint32_t *)ep)[2] = (uint32_t)dq;
        ((uint32_t *)ep)[3] = (uint32_t)(dq >> 32);
        ((uint32_t *)ep)[4] = 0;
    }

    uint32_t trb[4];
    trb_set(trb, (uint64_t)(uintptr_t)in, 0,
            (TRB_CONFIG_EP << 10) | ((uint32_t)slot << 24));
    int r = cmd_submit(trb);
    if (r < 0) {
        pr_warn("xhci: Configure Endpoint failed slot %d code\n", slot);
        return -1;
    }
    g.bulk_ok[slot] = 1;
    pr_info("xhci: slot %d bulk DCI out=%d in=%d mps=%u\n", slot, dci_out, dci_in, mps);
    return 0;
}

int xhci_bulk(int slot, int dci, int dir_in, void *data, uint32_t len)
{
    (void)dir_in;
    if (!g.present || slot <= 0 || !g.bulk_ok[slot])
        return -1;
    if (dci < 2 || dci >= 32 || !g.ep_ring[slot][dci])
        return -1;
    if (!data && len)
        return -1;

    int idx = g.ep_idx[slot][dci];
    int cyc = g.ep_cycle[slot][dci];
    uint32_t *ring = g.ep_ring[slot][dci];
    uint32_t *trb = ring + idx * 4;
    trb_set(trb, (uint64_t)(uintptr_t)data, len,
            (TRB_NORMAL << 10) | (1u << 5) /* IOC */ | (uint32_t)cyc);

    idx = (idx + 1) % (XHCI_RING_TRBS - 1);
    if (idx == 0)
        g.ep_cycle[slot][dci] ^= 1;
    g.ep_idx[slot][dci] = idx;

    /* Doorbell: target = DCI */
    wr32(g.db, slot * 4, (uint32_t)dci);

    uint32_t code = 0;
    int t = wait_event(TRB_TRANSFER, &code, NULL, 5000000);
    if (t < 0) return -1;
    if (code != 1 && code != 13) return -((int)code);
    return (int)len;
}

