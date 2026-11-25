/* fb.c — Linear framebuffer (TigxOS 1.3.8)
 *
 * Two discovery paths:
 *   1. Boot VBE handoff at ARCH_BOOT_FB_INFO (magic FB_MAGIC) — only used
 *      when boot.asm already put the hardware into a graphics mode.
 *   2. Bochs/QEMU VBE DISPI (BGA) via ports 0x01CE/0x01CF — probed at init
 *      but left disabled so the VGA text console stays on screen. Call
 *      fb_enable_bga() when you want graphics (1.4.x compositor, tests).
 *
 * High physical LFB addresses are mapped with vmm_map_mmio (uncached).
 * i386 cannot map >1 GB → BGA stays inactive there (GUI is x86_64-only).
 *
 * Drawing is pure software. GPU acceleration is 1.4.x.
 */

#include "fb.h"
#include "arch/arch.h"
#include "console.h"
#include "io.h"
#include "log.h"
#include "memory.h"
#include "string.h"
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Bochs Graphics Adapter (BGA / VBE DISPI)                            */
/* ------------------------------------------------------------------ */

#define VBE_DISPI_IOPORT_INDEX  0x01CE
#define VBE_DISPI_IOPORT_DATA   0x01CF

#define VBE_DISPI_INDEX_ID      0x0
#define VBE_DISPI_INDEX_XRES    0x1
#define VBE_DISPI_INDEX_YRES    0x2
#define VBE_DISPI_INDEX_BPP     0x3
#define VBE_DISPI_INDEX_ENABLE  0x4

#define VBE_DISPI_DISABLED      0x00
#define VBE_DISPI_ENABLED       0x01
#define VBE_DISPI_LFB_ENABLED   0x40

#define VBE_DISPI_ID0           0xB0C0
#define VBE_DISPI_ID5           0xB0C5

/* QEMU/Bochs LFB physical base (above 1 GB identity map on many setups). */
#define BGA_LFB_PHYS            0xE0000000UL
#define BGA_LFB_VIRT            0x0E000000UL   /* 224 MB virtual window */

static struct fb_info g_fb;

static void bga_write(uint16_t index, uint16_t value)
{
    outw(VBE_DISPI_IOPORT_INDEX, index);
    outw(VBE_DISPI_IOPORT_DATA, value);
}

static uint16_t bga_read(uint16_t index)
{
    outw(VBE_DISPI_IOPORT_INDEX, index);
    return inw(VBE_DISPI_IOPORT_DATA);
}

static int bga_present(void)
{
    uint16_t id = bga_read(VBE_DISPI_INDEX_ID);
    return (id >= VBE_DISPI_ID0 && id <= VBE_DISPI_ID5);
}

/* Actually switch the display into BGA LFB mode. */
static int enable_bga_mode(uint16_t width, uint16_t height, uint16_t bpp)
{
    if (!bga_present())
        return 0;

    bga_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    bga_write(VBE_DISPI_INDEX_XRES, width);
    bga_write(VBE_DISPI_INDEX_YRES, height);
    bga_write(VBE_DISPI_INDEX_BPP, bpp);
    bga_write(VBE_DISPI_INDEX_ENABLE,
              VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

    if (bga_read(VBE_DISPI_INDEX_XRES) != width ||
        bga_read(VBE_DISPI_INDEX_YRES) != height ||
        bga_read(VBE_DISPI_INDEX_BPP) != bpp)
        return 0;

    size_t fb_bytes = (size_t)width * height * (bpp / 8);
    fb_bytes = PAGE_ALIGN_UP(fb_bytes);

    if (vmm_map_mmio(BGA_LFB_PHYS, BGA_LFB_VIRT, fb_bytes) != 0)
        return 0;

    g_fb.width           = width;
    g_fb.height          = height;
    g_fb.bpp             = bpp;
    g_fb.bytes_per_pixel = bpp / 8;
    g_fb.pitch           = (uint32_t)width * g_fb.bytes_per_pixel;
    g_fb.phys            = BGA_LFB_PHYS;
    g_fb.virt            = (volatile uint8_t *)BGA_LFB_VIRT;
    g_fb.mode            = 0;
    g_fb.active          = 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Boot VBE handoff                                                   */
/* ------------------------------------------------------------------ */

static int try_boot_handoff(void)
{
    const struct boot_fb_info *boot =
        (const struct boot_fb_info *)ARCH_BOOT_FB_INFO;

    if (boot->magic != FB_MAGIC)
        return 0;
    if (boot->width < 320 || boot->height < 200 ||
        boot->pitch == 0 || (boot->bpp != 24 && boot->bpp != 32))
        return 0;

    uint64_t phys = (uint64_t)boot->phys_lo | ((uint64_t)boot->phys_hi << 32);
    if (phys == 0)
        return 0;

    size_t fb_bytes = (size_t)boot->pitch * boot->height;
    fb_bytes = PAGE_ALIGN_UP(fb_bytes);

    uintptr_t virt;
    if (phys + fb_bytes <= 0x40000000ULL) {
        virt = (uintptr_t)phys;     /* already in identity map */
    } else {
        virt = BGA_LFB_VIRT;
        if (vmm_map_mmio((uintptr_t)phys, virt, fb_bytes) != 0)
            return 0;
    }

    g_fb.width           = boot->width;
    g_fb.height          = boot->height;
    g_fb.pitch           = boot->pitch;
    g_fb.bpp             = boot->bpp;
    g_fb.bytes_per_pixel = boot->bpp / 8;
    g_fb.phys            = (uintptr_t)phys;
    g_fb.virt            = (volatile uint8_t *)virt;
    g_fb.mode            = boot->mode;
    g_fb.active          = 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Init / explicit enable                                             */
/* ------------------------------------------------------------------ */

const struct fb_info *fb_get(void)
{
    return &g_fb;
}

void fb_init(void)
{
    memset(&g_fb, 0, sizeof(g_fb));

    /* Probe BGA so later code knows it exists, but do NOT enable it.
     * Enabling LFB kills VGA text mode and the shell disappears. */
    g_fb.bga_present = bga_present();

    if (try_boot_handoff()) {
        pr_info("fb: VBE handoff %ux%u %ubpp pitch=%u LFB=%p mode=0x%x\n",
                g_fb.width, g_fb.height, g_fb.bpp, g_fb.pitch,
                (void *)g_fb.virt, g_fb.mode);
        return;
    }

    if (g_fb.bga_present)
        pr_info("fb: BGA present (call fb_enable_bga to use graphics)\n");
    else
        pr_info("fb: no framebuffer (text mode)\n");
}

int fb_enable_bga(uint16_t width, uint16_t height, uint16_t bpp)
{
    if (g_fb.active)
        return 0;   /* already have an FB */

    if (width == 0)
        width = 1024;
    if (height == 0)
        height = 768;
    if (bpp != 24 && bpp != 32)
        bpp = 32;

    if (!enable_bga_mode(width, height, bpp) &&
        !enable_bga_mode(800, 600, 32) &&
        !enable_bga_mode(640, 480, 32)) {
        pr_info("fb: BGA enable failed\n");
        return -1;
    }

    pr_info("fb: BGA enabled %ux%u %ubpp LFB phys=%p virt=%p\n",
            g_fb.width, g_fb.height, g_fb.bpp,
            (void *)g_fb.phys, (void *)g_fb.virt);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Drawing                                                            */
/* ------------------------------------------------------------------ */

static inline void put_pixel_raw(uint32_t x, uint32_t y, uint32_t argb)
{
    volatile uint8_t *p = g_fb.virt + y * g_fb.pitch + x * g_fb.bytes_per_pixel;
    /* Memory order is B,G,R[,A] (little-endian 32bpp as used by BGA/QEMU). */
    if (g_fb.bpp == 32) {
        p[0] = (uint8_t)(argb);           /* B */
        p[1] = (uint8_t)(argb >> 8);      /* G */
        p[2] = (uint8_t)(argb >> 16);     /* R */
        p[3] = (uint8_t)(argb >> 24);     /* A */
    } else {
        p[0] = (uint8_t)(argb);
        p[1] = (uint8_t)(argb >> 8);
        p[2] = (uint8_t)(argb >> 16);
    }
}

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t argb)
{
    if (!g_fb.active || x >= g_fb.width || y >= g_fb.height)
        return;
    put_pixel_raw(x, y, argb);
}

void fb_clear(uint32_t argb)
{
    if (!g_fb.active)
        return;

    if (g_fb.bpp == 32 && g_fb.pitch == g_fb.width * 4) {
        volatile uint32_t *p = (volatile uint32_t *)g_fb.virt;
        uint32_t n = g_fb.width * g_fb.height;
        /* Same channel order as put_pixel_raw */
        uint32_t pixel = (argb & 0xFFu) |
                         (((argb >> 8) & 0xFFu) << 8) |
                         (((argb >> 16) & 0xFFu) << 16) |
                         (((argb >> 24) & 0xFFu) << 24);
        for (uint32_t i = 0; i < n; i++)
            p[i] = pixel;
        return;
    }

    for (uint32_t y = 0; y < g_fb.height; y++)
        for (uint32_t x = 0; x < g_fb.width; x++)
            put_pixel_raw(x, y, argb);
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t argb)
{
    if (!g_fb.active)
        return;
    if (x >= g_fb.width || y >= g_fb.height)
        return;
    if (x + w > g_fb.width)
        w = g_fb.width - x;
    if (y + h > g_fb.height)
        h = g_fb.height - y;
    for (uint32_t row = 0; row < h; row++)
        for (uint32_t col = 0; col < w; col++)
            put_pixel_raw(x + col, y + row, argb);
}

void fb_hline(uint32_t x, uint32_t y, uint32_t w, uint32_t argb)
{
    fb_fill_rect(x, y, w, 1, argb);
}

void fb_vline(uint32_t x, uint32_t y, uint32_t h, uint32_t argb)
{
    fb_fill_rect(x, y, 1, h, argb);
}

void fb_dump(void)
{
    if (!g_fb.active) {
        kprintf("fb: inactive (bga_present=%d)\n", g_fb.bga_present);
        return;
    }
    kprintf("=== framebuffer ===\n");
    kprintf("  %ux%u  %u bpp  pitch %u\n",
            g_fb.width, g_fb.height, g_fb.bpp, g_fb.pitch);
    kprintf("  phys %p  virt %p  mode 0x%x\n",
            (void *)g_fb.phys, (void *)g_fb.virt, g_fb.mode);
}
