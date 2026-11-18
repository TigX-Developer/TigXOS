// font.h - Custom VGA text-mode font (self-contained, no deps)



#ifndef FONT_H
#define FONT_H

#include <stdint.h>

/*
 * TigxOS custom 8×16 font for VGA text mode (80×25).
 *
 * Usage:
 *   font_init();          // load Tigx font into VGA plane 2
 *   font_load(ptr);       // load any 256×16 glyph table
 *   font_restore_bios();  // best-effort restore (re-inits text mode look)
 *
 * Glyph format: font[ch][row], row 0 = top, bit 7 = leftmost pixel.
 * VGA stores 32 bytes per character slot; we write 16 and zero-pad.
 */

#define FONT_HEIGHT 16
#define FONT_GLYPHS 256

/* Built-in geometric Tigx font */
extern const uint8_t tig_font_8x16[FONT_GLYPHS][FONT_HEIGHT];

/* Load the built-in Tigx font into the VGA character generator. */
void font_init(void);

/* Load an arbitrary 8×16 font (256 glyphs × 16 rows). */
void font_load(const uint8_t font[FONT_GLYPHS][FONT_HEIGHT]);

/* Replace a single glyph (0–255) while the custom font is active. */
void font_set_glyph(unsigned char ch, const uint8_t rows[FONT_HEIGHT]);

#endif
