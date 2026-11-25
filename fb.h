/* fb.h — Linear framebuffer (TigxOS 1.3.8)
 *
 * Discovery:
 *   - Boot VBE handoff at ARCH_BOOT_FB_INFO (magic FB_MAGIC) if boot.asm
 *     already set a graphics mode.
 *   - Bochs/QEMU BGA (VBE DISPI) is probed but NOT enabled at boot, so the
 *     VGA text console stays visible. Call fb_enable_bga() when you actually
 *     want graphics (compositor / tests).
 *
 * Drawing is software-only; GPU offload is 1.4.x.
 */
#ifndef TIGX_FB_H
#define TIGX_FB_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FB_MAGIC  0x54494746u   /* 'TIGF' */

/* Boot handoff layout at ARCH_BOOT_FB_INFO (see arch/arch.h). */
struct boot_fb_info {
    uint32_t magic;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;             /* bytes per scanline */
    uint32_t bpp;               /* bits per pixel (24 or 32) */
    uint32_t phys_lo;
    uint32_t phys_hi;
    uint32_t mode;              /* VBE mode number (with LFB bit if set) */
} __attribute__((packed));

struct fb_info {
    int          active;
    uint32_t     width;
    uint32_t     height;
    uint32_t     pitch;
    uint32_t     bpp;
    uint32_t     bytes_per_pixel;
    uintptr_t    phys;
    volatile uint8_t *virt;     /* mapped (or identity) address */
    uint32_t     mode;
    int          bga_present;   /* 1 if Bochs/QEMU DISPI was detected */
};

const struct fb_info *fb_get(void);

/* Called once after memory is up. Does NOT switch the display away from
 * text mode unless a valid boot VBE handoff is already present. */
void fb_init(void);

/* Explicitly enable Bochs/QEMU BGA LFB. Switches the display to graphics
 * mode — text console becomes invisible. Returns 0 on success. */
int  fb_enable_bga(uint16_t width, uint16_t height, uint16_t bpp);

/* Basic drawing — no-ops when !active */
void fb_clear(uint32_t argb);
void fb_put_pixel(uint32_t x, uint32_t y, uint32_t argb);
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t argb);
void fb_hline(uint32_t x, uint32_t y, uint32_t w, uint32_t argb);
void fb_vline(uint32_t x, uint32_t y, uint32_t h, uint32_t argb);

static inline uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

void fb_dump(void);

#ifdef __cplusplus
}
#endif

#endif /* TIGX_FB_H */
