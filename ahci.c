/* ahci.c — basic AHCI HBA + port0 read/write (TigxOS 1.3.8 closes 1.3.3 gap) */
#include "ahci.h"
#include "interrupt.h"
#include "pci.h"
#include "log.h"
#include "memory.h"
#include "string.h"
#include "block.h"
#include "console.h"

#define PCI_CLASS_STORAGE 0x01
#define PCI_SUBCLASS_SATA 0x06
#define PCI_PROG_AHCI     0x01

#define HBA_GHC 0x04
#define HBA_PI  0x0C
#define HBA_PORT0 0x100
#define PORT_CLB  0x00
#define PORT_FB   0x08
#define PORT_IS   0x10
#define PORT_CMD  0x18
#define PORT_TFD  0x20
#define PORT_SIG  0x24
#define PORT_SSTS 0x28
#define PORT_SCTL 0x2C
#define PORT_SERR 0x30
#define PORT_CI   0x38

#define CMD_ST  (1u<<0)
#define CMD_FRE (1u<<4)
#define CMD_FR  (1u<<14)
#define CMD_CR  (1u<<15)

static volatile uint8_t *g_abar;
static int g_present;
static int g_port = -1;
static uint8_t *g_cl;   /* command list 1K */
static uint8_t *g_fb;   /* FIS receive 256 */
static uint8_t *g_ct;   /* command table */

static inline uint32_t rr(volatile uint8_t *b, uint32_t o) {
    return *(volatile uint32_t *)(b + o);
}
static inline void wr(volatile uint8_t *b, uint32_t o, uint32_t v) {
    *(volatile uint32_t *)(b + o) = v;
}

static void port_stop(volatile uint8_t *p) {
    uint32_t cmd = rr(p, PORT_CMD);
    cmd &= ~(CMD_ST | CMD_FRE);
    wr(p, PORT_CMD, cmd);
    for (int i = 0; i < 1000000; i++) {
        cmd = rr(p, PORT_CMD);
        if (!(cmd & (CMD_FR | CMD_CR))) break;
    }
}

static void port_start(volatile uint8_t *p) {
    uint32_t cmd = rr(p, PORT_CMD);
    cmd |= CMD_FRE;
    wr(p, PORT_CMD, cmd);
    cmd |= CMD_ST;
    wr(p, PORT_CMD, cmd);
}

static int port_cmd_rw(int write, uint64_t lba, uint32_t count, void *buf) {
    if (!g_present || g_port < 0 || count == 0 || count > 8) return -1;
    volatile uint8_t *p = g_abar + HBA_PORT0 + g_port * 0x80;
    wr(p, PORT_IS, 0xFFFFFFFF);
    /* Command header */
    memset(g_cl, 0, 1024);
    memset(g_ct, 0, 256);
    uint32_t *ch = (uint32_t *)g_cl;
    /* PRDTL=1, write flag, CFL=5 (FIS length) */
    ch[0] = (1u << 16) | (write ? (1u << 6) : 0) | 5;
    ch[1] = 0;
    ch[2] = (uint32_t)(uintptr_t)g_ct;
    ch[3] = (uint32_t)((uint64_t)(uintptr_t)g_ct >> 32);

    /* CFIS H2D */
    uint8_t *cfis = g_ct;
    memset(cfis, 0, 64);
    cfis[0] = 0x27; /* H2D */
    cfis[1] = 1 << 7; /* command */
    cfis[2] = write ? 0x35 : 0x25; /* DMA Ext write/read */
    cfis[4] = (uint8_t)lba;
    cfis[5] = (uint8_t)(lba >> 8);
    cfis[6] = (uint8_t)(lba >> 16);
    cfis[7] = 0x40; /* LBA mode */
    cfis[8] = (uint8_t)(lba >> 24);
    cfis[9] = (uint8_t)(lba >> 32);
    cfis[10] = (uint8_t)(lba >> 40);
    cfis[12] = (uint8_t)count;
    cfis[13] = (uint8_t)(count >> 8);

    /* PRDT */
    uint32_t *prd = (uint32_t *)(g_ct + 0x80);
    prd[0] = (uint32_t)(uintptr_t)buf;
    prd[1] = (uint32_t)((uint64_t)(uintptr_t)buf >> 32);
    prd[2] = 0;
    prd[3] = (count * 512 - 1) | (1u << 31);

    wr(p, PORT_CI, 1);
    for (int i = 0; i < 5000000; i++) {
        if ((rr(p, PORT_CI) & 1) == 0) {
            if (rr(p, PORT_IS) & (1u << 30)) return -1; /* TFES */
            return 0;
        }
    }
    return -1;
}

int ahci_read(uint64_t lba, uint32_t count, void *buf) {
    return port_cmd_rw(0, lba, count, buf);
}
int ahci_write(uint64_t lba, uint32_t count, const void *buf) {
    return port_cmd_rw(1, lba, count, (void *)buf);
}

static int ahci_blk_read(struct block_dev *dev, uint64_t lba, uint32_t count, void *buf)
{
    (void)dev; return ahci_read(lba, count, buf);
}
static int ahci_blk_write(struct block_dev *dev, uint64_t lba, uint32_t count, const void *buf)
{
    (void)dev; return ahci_write(lba, count, buf);
}

static void ahci_msi_isr(struct interrupt_frame *f)
{
    (void)f;
}

void ahci_init(void) {
    g_present = 0;
    g_port = -1;
    const struct pci_device *found = NULL;
    size_t n = pci_device_count();
    for (size_t i = 0; i < n; i++) {
        const struct pci_device *d = pci_device_at(i);
        if (d && d->class_code == PCI_CLASS_STORAGE && d->subclass == PCI_SUBCLASS_SATA &&
            d->prog_if == PCI_PROG_AHCI) {
            found = d;
            break;
        }
    }
    if (!found) {
        pr_info("ahci: no HBA on PCI (ATA remains primary)\n");
        return;
    }
    struct pci_device mut = *found;
    pci_enable_device(&mut);
    if (!mut.bars[5].address) {
        pr_warn("ahci: no ABAR\n");
        return;
    }
    uintptr_t abar = (uintptr_t)mut.bars[5].address;
#if defined(__x86_64__)
    (void)vmm_map_mmio(abar, abar, 0x1100);
#endif
    g_abar = (volatile uint8_t *)abar;
    /* GHC.AE */
    wr(g_abar, HBA_GHC, rr(g_abar, HBA_GHC) | (1u << 31));
    uint32_t pi = rr(g_abar, HBA_PI);
    for (int i = 0; i < 32; i++) {
        if (!(pi & (1u << i))) continue;
        volatile uint8_t *p = g_abar + HBA_PORT0 + i * 0x80;
        uint32_t ssts = rr(p, PORT_SSTS);
        uint32_t det = ssts & 0xF;
        uint32_t ipm = (ssts >> 8) & 0xF;
        if (det != 3 || ipm != 1) continue; /* present + active */
        uint32_t sig = rr(p, PORT_SIG);
        if (sig != 0x00000101) continue; /* ATA */
        g_port = i;
        break;
    }
    if (g_port < 0) {
        pr_info("ahci: HBA found but no active ATA port\n");
        g_present = 1; /* HBA present */
        return;
    }
    g_cl = pmm_alloc_frames(1);
    g_fb = pmm_alloc_frames(1);
    g_ct = pmm_alloc_frames(1);
    if (!g_cl || !g_fb || !g_ct) return;
    memset(g_cl, 0, 4096);
    memset(g_fb, 0, 4096);
    volatile uint8_t *p = g_abar + HBA_PORT0 + g_port * 0x80;
    port_stop(p);
    wr(p, PORT_CLB, (uint32_t)(uintptr_t)g_cl);
    wr(p, PORT_CLB + 4, (uint32_t)((uint64_t)(uintptr_t)g_cl >> 32));
    wr(p, PORT_FB, (uint32_t)(uintptr_t)g_fb);
    wr(p, PORT_FB + 4, (uint32_t)((uint64_t)(uintptr_t)g_fb >> 32));
    port_start(p);
    g_present = 1;
    pr_info("ahci: port %d online (DMA read/write ready)\n", g_port);

    /* Per-core MSI: route HBA IRQs to CPU 0 when the device supports MSI. */
    {
        if (pci_route_msi(&mut, 0, ahci_msi_isr) >= 0)
            pr_info("ahci: MSI routed to CPU 0\n");
        else
            pr_info("ahci: MSI unavailable, staying on INTx\n");
    }

    /* Register as block device and prefer as root disk. */
    {
        struct block_dev bd;
        memset(&bd, 0, sizeof(bd));
        strncpy(bd.name, "ahci0", sizeof(bd.name) - 1);
        bd.type = BLOCK_DEV_AHCI;
        bd.present = 1;
        bd.sectors = 0x100000;
        bd.read = ahci_blk_read;
        bd.write = ahci_blk_write;
        int id = block_register(&bd);
        if (id >= 0) {
            block_set_default(id);
            pr_info("ahci: set as default block device id=%d (root path)\n", id);
        }
    }
}

int ahci_present(void) { return g_present; }
int ahci_port_count(void) { return g_port >= 0 ? 1 : 0; }
