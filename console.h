// console.h - VGA text console + serial (COM1)


#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdarg.h>

/* VGA text mode colors (foreground 0-15, background 0-7) */
#define VGA_BLACK        0
#define VGA_BLUE         1
#define VGA_GREEN        2
#define VGA_CYAN         3
#define VGA_RED          4
#define VGA_MAGENTA      5
#define VGA_BROWN        6   /* "orange" */
#define VGA_LGRAY        7
#define VGA_DGRAY        8
#define VGA_LBLUE        9
#define VGA_LGREEN      10
#define VGA_LCYAN       11
#define VGA_LRED        12
#define VGA_LMAGENTA    13
#define VGA_YELLOW      14  /* brighter "orange" */
#define VGA_WHITE       15

/* Build attribute byte: fg | (bg << 4) */
#define VGA_ATTR(fg, bg) ((unsigned char)((fg) | ((bg) << 4)))

/* Common presets */
#define VGA_ORANGE_ON_BLACK   VGA_ATTR(VGA_YELLOW, VGA_BLACK)
#define VGA_WHITE_ON_BLACK    VGA_ATTR(VGA_WHITE, VGA_BLACK)
#define VGA_ORANGE_ON_BLUE    VGA_ATTR(VGA_YELLOW, VGA_BLUE)

/* --- TigxOS 1.1.12 semantic palette --- */
#define TIG_BG           VGA_BLACK
#define TIG_FG           VGA_LGRAY

#define TIG_HEADER       VGA_LCYAN
#define TIG_SUBHEADER    VGA_CYAN
#define TIG_BRAND_A      VGA_YELLOW    /* "Tig" and "OS" */
#define TIG_BRAND_B      VGA_WHITE     /* "x" */
#define TIG_OK           VGA_LGREEN
#define TIG_FAIL         VGA_LRED
#define TIG_WARN         VGA_YELLOW
#define TIG_INFO         VGA_WHITE
#define TIG_DIM          VGA_DGRAY
#define TIG_METRIC       VGA_LMAGENTA
#define TIG_ACCENT       VGA_LBLUE
#define TIG_PROMPT       VGA_YELLOW

#define TIG_ATTR_OK      VGA_ATTR(TIG_OK,     TIG_BG)
#define TIG_ATTR_FAIL    VGA_ATTR(TIG_FAIL,   TIG_BG)
#define TIG_ATTR_HEADER  VGA_ATTR(TIG_HEADER, TIG_BG)
#define TIG_ATTR_DIM     VGA_ATTR(TIG_DIM,    TIG_BG)

void console_clear(void);
void console_setcolor(unsigned char c);
void console_putchar(char c);
void console_backspace(void);
void console_get_cursor(int *x, int *y);
void console_set_cursor(int x, int y);
void console_show_cursor(void);
void console_hide_cursor(void);

/*
 * Panic escape hatch: forcibly release console_lock.
 * Call only from panic_render() after cli.  A fault mid-print can leave the
 * lock held by a context that will never run again; without this, every
 * subsequent kprintf / console_* call spins forever with IRQs off.
 */
void console_force_unlock(void);

void kprint(const char *s);
void kprintf(const char *fmt, ...);
void vkprintf(const char *fmt, va_list args);

/* Colored output helpers */
void kprint_color(const char *s, unsigned char color);
void kprintf_color(unsigned char color, const char *fmt, ...);
void vkprintf_color(unsigned char color, const char *fmt, va_list args);
void console_write_colored(const char *s); /* parses inline color codes ~X */

/* Draw `n` copies of character `ch` in color `col` followed by newline. */
void console_rule(unsigned char col, char ch, int n);

/* COM1 (0x3F8) serial — used by panic path and optional early debug */
void serial_init(void);
void serial_putchar(char c);
void serial_print(const char *s);
void serial_printf(const char *fmt, ...);

#endif
