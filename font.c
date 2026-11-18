// font.c - Load custom 8×16 fonts into VGA text-mode plane 2


#include "font.h"
#include "io.h"
#include "console.h"

/*
 * VGA font RAM lives in plane 2. To write it from CPU:
 *   1. Unlock CRTC (clear protect bit on CRTC index 0x11)
 *   2. Sequencer: odd/even off, map mask = plane 2 only
 *   3. Graphics: write mode, sequential addressing, plane 2 read
 *   4. Write 32 bytes/char at 0xA0000 (we use 16 + pad)
 *   5. Restore sequencer / graphics to text-mode defaults
 *
 * Works on real VGA and QEMU/SeaBIOS stdvga. No external deps.
 */

#define VGA_SEQ_INDEX   0x3C4
#define VGA_SEQ_DATA    0x3C5
#define VGA_GC_INDEX    0x3CE
#define VGA_GC_DATA     0x3CF
#define VGA_CRTC_INDEX  0x3D4
#define VGA_CRTC_DATA   0x3D5

#define FONT_PLANE_BASE 0xA0000u
#define BYTES_PER_CHAR  32

static inline void seq_write(uint8_t idx, uint8_t val) {
    outb(VGA_SEQ_INDEX, idx);
    outb(VGA_SEQ_DATA, val);
}

static inline uint8_t seq_read(uint8_t idx) {
    outb(VGA_SEQ_INDEX, idx);
    return inb(VGA_SEQ_DATA);
}

static inline void gc_write(uint8_t idx, uint8_t val) {
    outb(VGA_GC_INDEX, idx);
    outb(VGA_GC_DATA, val);
}

static inline uint8_t gc_read(uint8_t idx) {
    outb(VGA_GC_INDEX, idx);
    return inb(VGA_GC_DATA);
}

static inline void crtc_write(uint8_t idx, uint8_t val) {
    outb(VGA_CRTC_INDEX, idx);
    outb(VGA_CRTC_DATA, val);
}

static inline uint8_t crtc_read(uint8_t idx) {
    outb(VGA_CRTC_INDEX, idx);
    return inb(VGA_CRTC_DATA);
}

static void vga_write_font_plane(const uint8_t font[FONT_GLYPHS][FONT_HEIGHT]) {
    /* Save registers we touch */
    uint8_t seq2 = seq_read(0x02);
    uint8_t seq4 = seq_read(0x04);
    uint8_t gc4  = gc_read(0x04);
    uint8_t gc5  = gc_read(0x05);
    uint8_t gc6  = gc_read(0x06);
    uint8_t cr11 = crtc_read(0x11);

    /* Unlock CRTC registers 0–7 */
    crtc_write(0x11, (uint8_t)(cr11 & 0x7F));

    /* Sequencer: enable plane 2 writes, disable odd/even */
    seq_write(0x02, 0x04);              /* map mask = plane 2 */
    seq_write(0x04, (uint8_t)((seq4 & ~0x04) | 0x04)); /* sequential / odd-even off → set bit 2? */
    /* Actually: SEQ4 bit2 = odd/even disable. We want sequential addressing. */
    seq_write(0x04, 0x06);              /* plane 0–3 enable + odd/even off (common recipe) */

    /* Graphics controller: read plane 2, write mode 0, sequential */
    gc_write(0x04, 0x02);               /* read map select = plane 2 */
    gc_write(0x05, 0x00);               /* write mode 0, read mode 0, odd/even off */
    gc_write(0x06, 0x00);               /* memory map A0000-AFFFF, non-interleaved */

    volatile uint8_t *dst = (volatile uint8_t *)FONT_PLANE_BASE;
    for (int ch = 0; ch < FONT_GLYPHS; ch++) {
        for (int row = 0; row < FONT_HEIGHT; row++)
            dst[ch * BYTES_PER_CHAR + row] = font[ch][row];
        for (int row = FONT_HEIGHT; row < BYTES_PER_CHAR; row++)
            dst[ch * BYTES_PER_CHAR + row] = 0;
    }

    /* Restore */
    seq_write(0x02, seq2);
    seq_write(0x04, seq4);
    gc_write(0x04, gc4);
    gc_write(0x05, gc5);
    gc_write(0x06, gc6);
    crtc_write(0x11, cr11);
}

void font_load(const uint8_t font[FONT_GLYPHS][FONT_HEIGHT]) {
    if (!font)
        return;
    vga_write_font_plane(font);
}

void font_init(void) {
    font_load(tig_font_8x16);
    kprintf("Font: ISO.F16 8x16 VGA font loaded\n");
}

void font_set_glyph(unsigned char ch, const uint8_t rows[FONT_HEIGHT]) {
    if (!rows)
        return;

    uint8_t seq2 = seq_read(0x02);
    uint8_t seq4 = seq_read(0x04);
    uint8_t gc4  = gc_read(0x04);
    uint8_t gc5  = gc_read(0x05);
    uint8_t gc6  = gc_read(0x06);
    uint8_t cr11 = crtc_read(0x11);

    crtc_write(0x11, (uint8_t)(cr11 & 0x7F));
    seq_write(0x02, 0x04);
    seq_write(0x04, 0x06);
    gc_write(0x04, 0x02);
    gc_write(0x05, 0x00);
    gc_write(0x06, 0x00);

    volatile uint8_t *dst = (volatile uint8_t *)FONT_PLANE_BASE;
    for (int row = 0; row < FONT_HEIGHT; row++)
        dst[ch * BYTES_PER_CHAR + row] = rows[row];
    for (int row = FONT_HEIGHT; row < BYTES_PER_CHAR; row++)
        dst[ch * BYTES_PER_CHAR + row] = 0;

    seq_write(0x02, seq2);
    seq_write(0x04, seq4);
    gc_write(0x04, gc4);
    gc_write(0x05, gc5);
    gc_write(0x06, gc6);
    crtc_write(0x11, cr11);
}
