// console.c - VGA text console + COM1 serial (IRQ-safe via spinlock)


#include "console.h"
#include "io.h"
#include "spinlock.h"
#include "log.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#define VGA_MEMORY  0xB8000
#define VGA_WIDTH   80
#define VGA_HEIGHT  25

#define VGA_CTRL_REG      0x3D4
#define VGA_DATA_REG      0x3D5
#define VGA_CURSOR_START  0x0A
#define VGA_CURSOR_END    0x0B
#define VGA_CURSOR_H      0x0E
#define VGA_CURSOR_L      0x0F

/* 16550 UART — COM1 */
#define COM1       0x3F8
#define COM1_DATA  (COM1 + 0)
#define COM1_IER   (COM1 + 1)
#define COM1_FCR   (COM1 + 2)
#define COM1_LCR   (COM1 + 3)
#define COM1_MCR   (COM1 + 4)
#define COM1_LSR   (COM1 + 5)

static volatile unsigned short *const vga = (volatile unsigned short *)VGA_MEMORY;
static int cursor_x = 0, cursor_y = 0;
static unsigned char color = 0x0F; /* bright white on black (body text) */
static int cursor_visible = 1;
static int serial_ready = 0;
void serial_putchar(char c);  /* defined later; used by putchar_raw */

/* Serializes VGA + kprintf against preemption / IRQ prints */
static spinlock_t console_lock = SPINLOCK_INIT;

/*
 * Panic-only: abandon whatever was holding console_lock.
 * Safe because panic never returns and IRQs stay off.
 */
void console_force_unlock(void) {
    console_lock.locked = 0;
    console_lock.irq_flags = 0;
}

static void update_hardware_cursor(void) {
    uint16_t pos = (uint16_t)(cursor_y * VGA_WIDTH + cursor_x);

    if (!cursor_visible) {
        outb(VGA_CTRL_REG, VGA_CURSOR_START);
        outb(VGA_DATA_REG, 0x20); /* bit 5 disables cursor */
        return;
    }

    outb(VGA_CTRL_REG, VGA_CURSOR_START);
    outb(VGA_DATA_REG, 0x0E);
    outb(VGA_CTRL_REG, VGA_CURSOR_END);
    outb(VGA_DATA_REG, 0x0F);

    outb(VGA_CTRL_REG, VGA_CURSOR_H);
    outb(VGA_DATA_REG, (uint8_t)((pos >> 8) & 0xFF));
    outb(VGA_CTRL_REG, VGA_CURSOR_L);
    outb(VGA_DATA_REG, (uint8_t)(pos & 0xFF));
}

static void scroll(void) {
    for (int y = 1; y < VGA_HEIGHT; y++)
        for (int x = 0; x < VGA_WIDTH; x++)
            vga[(y - 1) * VGA_WIDTH + x] = vga[y * VGA_WIDTH + x];
    for (int x = 0; x < VGA_WIDTH; x++)
        vga[(VGA_HEIGHT - 1) * VGA_WIDTH + x] =
            (unsigned short)((color << 8) | ' ');
    cursor_y = VGA_HEIGHT - 1;
}

static void wrap_cursor(void) {
    if (cursor_x >= VGA_WIDTH) {
        cursor_x = 0;
        cursor_y++;
    }
    if (cursor_y >= VGA_HEIGHT)
        scroll();
}

/* Unlocked primitives — caller holds console_lock */
static void putchar_raw(char c) {
    /* Mirror every character to COM1 so headless (-serial stdio) runs see output. */
    if (serial_ready)
        serial_putchar(c);

    if (c == '\b') {
        /* Non-destructive: move cursor left only (VT-style).
         * Erase is the shell's job (\b + space + \b, or redraw_from). */
        if (cursor_x > 0) {
            cursor_x--;
        } else if (cursor_y > 0) {
            cursor_y--;
            cursor_x = VGA_WIDTH - 1;
        } else {
            return;
        }
        update_hardware_cursor();
        return;
    }
    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
    } else if (c == '\r') {
        cursor_x = 0;
    } else if (c == '\t') {
        int next = (cursor_x + 4) & ~3;
        if (next >= VGA_WIDTH) {
            cursor_x = 0;
            cursor_y++;
        } else {
            cursor_x = next;
        }
    } else {
        vga[cursor_y * VGA_WIDTH + cursor_x] =
            (unsigned short)((color << 8) | (unsigned char)c);
        cursor_x++;
    }
    wrap_cursor();
    update_hardware_cursor();
}

/* VGA text mode is CP437, not UTF-8. Decode UTF-8 and map the few symbols we
 * actually use; anything else becomes '?'. */
static unsigned char cp437_from_unicode(unsigned cp) {
    switch (cp) {
    case 0x2013: case 0x2014: return '-';
    case 0x2018: case 0x2019: return '\'';
    case 0x201C: case 0x201D: return '"';
    case 0x2022: return 0x07;                   /* bullet */
    case 0x2192: return 0x1A;                   /* -> */
    case 0x2190: return 0x1B;                   /* <- */
    case 0x2713: return 0xFB;                   /* check */
    case 0x2500: return 0xC4; case 0x2502: return 0xB3;
    case 0x2550: return 0xCD; case 0x2551: return 0xBA;
    case 0x2554: return 0xC9; case 0x2557: return 0xBB;
    case 0x255A: return 0xC8; case 0x255D: return 0xBC;
    default: return '?';
    }
}

/* Line accumulator so every kprintf line also lands in the dmesg ring.
 * Flushed on newline or when full. Recursion-safe via log_ring_raw. */
static char   kprintf_line[192];
static size_t kprintf_line_len;

static void kprintf_line_flush(void) {
    if (kprintf_line_len == 0)
        return;
    log_ring_raw(LOG_INFO, kprintf_line, kprintf_line_len);
    kprintf_line_len = 0;
}

static void putchar_unlocked(char ch) {
    static unsigned cp, need;
    unsigned char c = (unsigned char)ch;

    /* Accumulate printable ASCII for the ring (ignore UTF-8 continuation) */
    if (c == '\n' || c == '\r') {
        if (kprintf_line_len < sizeof(kprintf_line) - 1)
            kprintf_line[kprintf_line_len++] = (char)c;
        kprintf_line_flush();
    } else if (c >= 0x20 && c < 0x80) {
        if (kprintf_line_len < sizeof(kprintf_line) - 1)
            kprintf_line[kprintf_line_len++] = (char)c;
        else {
            kprintf_line_flush();
            kprintf_line[kprintf_line_len++] = (char)c;
        }
    }

    if (need) {
        if ((c & 0xC0) == 0x80) {
            cp = (cp << 6) | (c & 0x3F);
            if (--need == 0)
                putchar_raw((char)cp437_from_unicode(cp));
            return;
        }
        need = 0;                               /* broken sequence: fall through */
        putchar_raw('?');
    }
    if (c < 0x80) { putchar_raw((char)c); return; }
    if ((c & 0xE0) == 0xC0)      { cp = c & 0x1F; need = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; need = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; need = 3; }
    else putchar_raw((char)c);                  /* already CP437 */
}

static void print_number_unlocked(unsigned long long num, int base, int upper,
                                  int width) {
    char buf[32];
    int i = 0;
    if (num == 0) {
        while (width > 1) {
            putchar_unlocked('0');
            width--;
        }
        putchar_unlocked('0');
        return;
    }
    while (num) {
        int d = (int)(num % (unsigned)base);
        buf[i++] = (d < 10) ? (char)('0' + d)
                            : (char)((upper ? 'A' : 'a') + d - 10);
        num /= (unsigned)base;
    }
    while (i < width) {
        putchar_unlocked('0');
        width--;
    }
    while (--i >= 0)
        putchar_unlocked(buf[i]);
}

void console_clear(void) {
    spin_lock(&console_lock);
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        vga[i] = (unsigned short)((color << 8) | ' ');
    cursor_x = cursor_y = 0;
    update_hardware_cursor();
    spin_unlock(&console_lock);
}

void console_setcolor(unsigned char c) {
    spin_lock(&console_lock);
    color = c;
    spin_unlock(&console_lock);
}

void console_putchar(char c) {
    spin_lock(&console_lock);
    putchar_unlocked(c);
    spin_unlock(&console_lock);
}

void console_backspace(void) {
    spin_lock(&console_lock);
    putchar_unlocked('\b');
    spin_unlock(&console_lock);
}

void console_get_cursor(int *x, int *y) {
    spin_lock(&console_lock);
    if (x) *x = cursor_x;
    if (y) *y = cursor_y;
    spin_unlock(&console_lock);
}

void console_set_cursor(int x, int y) {
    spin_lock(&console_lock);
    if (x >= 0 && x < VGA_WIDTH) cursor_x = x;
    if (y >= 0 && y < VGA_HEIGHT) cursor_y = y;
    update_hardware_cursor();
    spin_unlock(&console_lock);
}

void console_show_cursor(void) {
    spin_lock(&console_lock);
    cursor_visible = 1;
    update_hardware_cursor();
    spin_unlock(&console_lock);
}

void console_hide_cursor(void) {
    spin_lock(&console_lock);
    cursor_visible = 0;
    update_hardware_cursor();
    spin_unlock(&console_lock);
}

void kprint(const char *s) {
    if (!s) s = "(null)";
    spin_lock(&console_lock);
    while (*s)
        putchar_unlocked(*s++);
    spin_unlock(&console_lock);
}

/* --- Shared format walker used by both vkprintf and vkprintf_color --- */
static void format_walk(va_list args, const char *fmt) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            putchar_unlocked(*fmt);
            continue;
        }
        fmt++;
        if (!*fmt)
            break;

        int width = 0;
        int left = 0;
        if (*fmt == '-') {
            left = 1;
            fmt++;
        }
        if (*fmt == '0')
            fmt++;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        int long_cnt = 0;
        while (*fmt == 'l') {
            long_cnt++;
            fmt++;
        }

        switch (*fmt) {
        case 's': {
            const char *s = va_arg(args, const char *);
            if (!s) s = "(null)";
            int len = 0;
            while (s[len])
                len++;
            if (!left)
                for (int k = len; k < width; k++)
                    putchar_unlocked(' ');
            while (*s)
                putchar_unlocked(*s++);
            if (left)
                for (int k = len; k < width; k++)
                    putchar_unlocked(' ');
            break;
        }
        case 'c':
            putchar_unlocked((char)va_arg(args, int));
            break;
        case 'd': {
            int v = va_arg(args, int);
            if (v < 0) {
                putchar_unlocked('-');
                print_number_unlocked((unsigned int)(-(v + 1)) + 1u, 10, 0, 0);
            } else {
                print_number_unlocked((unsigned)v, 10, 0, 0);
            }
            break;
        }
        case 'u': {
            unsigned long long v;
            if (long_cnt >= 2)      v = va_arg(args, unsigned long long);
            else if (long_cnt == 1) v = va_arg(args, unsigned long);
            else                    v = va_arg(args, unsigned int);
            print_number_unlocked(v, 10, 0, width);
            break;
        }
        case 'x': {
            unsigned long long v;
            if (long_cnt >= 2)      v = va_arg(args, unsigned long long);
            else if (long_cnt == 1) v = va_arg(args, unsigned long);
            else                    v = va_arg(args, unsigned int);
            print_number_unlocked(v, 16, 0, width);
            break;
        }
        case 'X': {
            unsigned long long v;
            if (long_cnt >= 2)      v = va_arg(args, unsigned long long);
            else if (long_cnt == 1) v = va_arg(args, unsigned long);
            else                    v = va_arg(args, unsigned int);
            print_number_unlocked(v, 16, 1, width);
            break;
        }
        case 'p': {
            unsigned long long pv = (unsigned long long)(uintptr_t)va_arg(args, void *);
            putchar_unlocked('0');
            putchar_unlocked('x');
            print_number_unlocked(pv, 16, 0, (int)(sizeof(void *) * 2));
            break;
        }
        case '%':
            putchar_unlocked('%');
            break;
        default:
            putchar_unlocked('%');
            putchar_unlocked(*fmt);
            break;
        }
    }
}

void vkprintf(const char *fmt, va_list args) {
    spin_lock(&console_lock);
    format_walk(args, fmt);
    spin_unlock(&console_lock);
}

void vkprintf_color(unsigned char col, const char *fmt, va_list args) {
    spin_lock(&console_lock);
    unsigned char old = color;
    color = col;
    format_walk(args, fmt);
    color = old;
    spin_unlock(&console_lock);
}

void kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vkprintf(fmt, args);
    va_end(args);
}

void kprintf_color(unsigned char col, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vkprintf_color(col, fmt, args);
    va_end(args);
}

/* Colored single-string output */
void kprint_color(const char *s, unsigned char col) {
    if (!s) s = "(null)";
    spin_lock(&console_lock);
    unsigned char old = color;
    color = col;
    while (*s)
        putchar_unlocked(*s++);
    color = old;
    spin_unlock(&console_lock);
}

/*
 * Write a string with inline color codes.
 * Format: ~X where X is a single hex digit (0-F) sets color.
 * Example: "~ETig~Fx~EOS~F" prints Tig in yellow, x in white.
 */
void console_write_colored(const char *s) {
    if (!s) return;
    spin_lock(&console_lock);
    unsigned char old = color;

    while (*s) {
        if (*s == '~' && s[1] &&
            ((s[1] >= '0' && s[1] <= '9') ||
             (s[1] >= 'A' && s[1] <= 'F') ||
             (s[1] >= 'a' && s[1] <= 'f'))) {
            char c = s[1];
            if (c >= 'a' && c <= 'f') c = c - 'a' + 'A';
            if (c >= '0' && c <= '9')
                color = (unsigned char)(c - '0');
            else
                color = (unsigned char)(c - 'A' + 10);
            s += 2;
            continue;
        }
        putchar_unlocked(*s++);
    }

    color = old;
    spin_unlock(&console_lock);
}

/* Draw n copies of `ch` in `col`, then newline. */
void console_rule(unsigned char col, char ch, int n) {
    spin_lock(&console_lock);
    unsigned char old = color;
    color = col;
    for (int i = 0; i < n; i++)
        putchar_unlocked(ch);
    putchar_unlocked('\n');
    color = old;
    spin_unlock(&console_lock);
}

/* ---------- COM1 serial (for panic mirror / headless debug) ---------- */

void serial_init(void) {
    outb(COM1_IER, 0x00);
    outb(COM1_LCR, 0x80);
    outb(COM1_DATA, 0x03); /* 38400 baud */
    outb(COM1_IER, 0x00);
    outb(COM1_LCR, 0x03);
    outb(COM1_FCR, 0xC7);
    outb(COM1_MCR, 0x0B);
    serial_ready = 1;
}

void serial_putchar(char c) {
    if (!serial_ready)
        return;
    if (c == '\n')
        serial_putchar('\r');
    while (!(inb(COM1_LSR) & 0x20))
        ;
    outb(COM1_DATA, (uint8_t)c);
}

void serial_print(const char *s) {
    if (!s)
        s = "(null)";
    while (*s)
        serial_putchar(*s++);
}

static void serial_print_number(unsigned long long num, int base, int upper,
                                int width) {
    char buf[32];
    int i = 0;
    if (num == 0) {
        while (width > 1) {
            serial_putchar('0');
            width--;
        }
        serial_putchar('0');
        return;
    }
    while (num) {
        int d = (int)(num % (unsigned)base);
        buf[i++] = (d < 10) ? (char)('0' + d)
                            : (char)((upper ? 'A' : 'a') + d - 10);
        num /= (unsigned)base;
    }
    while (i < width) {
        serial_putchar('0');
        width--;
    }
    while (--i >= 0)
        serial_putchar(buf[i]);
}

void serial_printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            serial_putchar(*fmt);
            continue;
        }
        fmt++;
        if (!*fmt)
            break;

        int width = 0;
        if (*fmt == '0')
            fmt++;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        int long_cnt = 0;
        while (*fmt == 'l') { long_cnt++; fmt++; }

        switch (*fmt) {
        case 's': {
            const char *s = va_arg(args, const char *);
            serial_print(s);
            break;
        }
        case 'c':
            serial_putchar((char)va_arg(args, int));
            break;
        case 'd': {
            int v = va_arg(args, int);
            if (v < 0) {
                serial_putchar('-');
                serial_print_number((unsigned int)(-(v + 1)) + 1u, 10, 0, 0);
            } else {
                serial_print_number((unsigned)v, 10, 0, 0);
            }
            break;
        }
        case 'u': {
            unsigned long long v;
            if (long_cnt >= 2)      v = va_arg(args, unsigned long long);
            else if (long_cnt == 1) v = va_arg(args, unsigned long);
            else                    v = va_arg(args, unsigned int);
            serial_print_number(v, 10, 0, width);
            break;
        }
        case 'x': {
            unsigned long long v;
            if (long_cnt >= 2)      v = va_arg(args, unsigned long long);
            else if (long_cnt == 1) v = va_arg(args, unsigned long);
            else                    v = va_arg(args, unsigned int);
            serial_print_number(v, 16, 0, width);
            break;
        }
        case 'X': {
            unsigned long long v;
            if (long_cnt >= 2)      v = va_arg(args, unsigned long long);
            else if (long_cnt == 1) v = va_arg(args, unsigned long);
            else                    v = va_arg(args, unsigned int);
            serial_print_number(v, 16, 1, width);
            break;
        }
        case 'p': {
            unsigned long long pv = (unsigned long long)(uintptr_t)va_arg(args, void *);
            serial_print("0x");
            serial_print_number(pv, 16, 0, (int)(sizeof(void *) * 2));
            break;
        }
        case '%':
            serial_putchar('%');
            break;
        default:
            serial_putchar('%');
            serial_putchar(*fmt);
            break;
        }
    }
    va_end(args);
}
