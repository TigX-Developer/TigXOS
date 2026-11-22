/* log.c — dmesg ring buffer + leveled logging (TigxOS 1.2.5 final) */

#include "log.h"
#include "console.h"
#include "spinlock.h"
#include "string.h"
#include "timer.h"
#include <stdarg.h>
#include <stdint.h>

/* ---------- Ring buffer with absolute sequence ---------- */
static char          log_buf[LOG_BUF_SIZE];
static size_t        log_head;      /* next write index in the circular buffer */
static size_t        log_len;       /* bytes currently held (≤ LOG_BUF_SIZE) */
static size_t        log_abs_seq;   /* absolute byte counter of next write (never resets) */
static spinlock_t    log_lock = SPINLOCK_INIT;
static int           console_level = LOG_LEVEL_DEFAULT;
static int           log_ready = 0;
static int           log_recursion = 0;   /* guard against kprintf ↔ log reentry */

volatile int panicking = 0;

static const char *level_tag(int level) {
    switch (level) {
    case LOG_EMERG:  return "EMERG";
    case LOG_ALERT:  return "ALERT";
    case LOG_CRIT:   return "CRIT ";
    case LOG_ERR:    return "ERR  ";
    case LOG_WARN:   return "WARN ";
    case LOG_NOTICE: return "NOTE ";
    case LOG_INFO:   return "INFO ";
    case LOG_DEBUG:  return "DEBUG";
    default:         return "?????";
    }
}

void log_init(void) {
    log_head = 0;
    log_len  = 0;
    log_abs_seq = 0;
    log_ready = 1;
    klog(LOG_INFO, "log: ring buffer %u bytes ready\n", (unsigned)LOG_BUF_SIZE);
}

void log_set_console_level(int level) {
    if (level < LOG_EMERG) level = LOG_EMERG;
    if (level > LOG_DEBUG) level = LOG_DEBUG;
    console_level = level;
}

int log_get_console_level(void) {
    return console_level;
}

/* Caller must hold log_lock. */
static void ring_put_locked(const char *data, size_t n) {
    if (n == 0) return;
    if (n >= LOG_BUF_SIZE) {
        data += (n - LOG_BUF_SIZE);
        n = LOG_BUF_SIZE;
        log_head = 0;
        log_len  = 0;
    }
    size_t space = LOG_BUF_SIZE - log_len;
    if (n > space)
        log_len -= (n - space);

    for (size_t i = 0; i < n; i++) {
        log_buf[log_head] = data[i];
        log_head = (log_head + 1) % LOG_BUF_SIZE;
        if (log_len < LOG_BUF_SIZE)
            log_len++;
    }
    log_abs_seq += n;
}

/* Ring-only path used by kprintf (no console reprint, recursion-safe). */
void log_ring_raw(int level, const char *msg, size_t len) {
    (void)level;
    if (!msg || len == 0 || !log_ready || log_recursion)
        return;
    if (panicking)
        return; /* avoid lock work during panic dump */

    unsigned long flags = spin_lock_irqsave(&log_lock);
    if (log_ready)
        ring_put_locked(msg, len);
    spin_unlock_irqrestore(&log_lock, flags);
}

void log_append(int level, const char *msg) {
    if (!msg) return;
    if (log_recursion) return; /* prevent infinite kprintf ↔ log */

    char line[256];
    size_t pos = 0;

    uint32_t ticks = log_ready ? timer_get_ticks() : 0;
    char tbuf[16];
    int tlen = 0;
    if (ticks == 0) {
        tbuf[tlen++] = '0';
    } else {
        char rev[12];
        int r = 0;
        uint32_t v = ticks;
        while (v && r < 11) {
            rev[r++] = '0' + (v % 10);
            v /= 10;
        }
        while (r > 0)
            tbuf[tlen++] = rev[--r];
    }
    tbuf[tlen] = 0;

    line[pos++] = '[';
    for (int i = 0; tbuf[i] && pos < sizeof(line) - 1; i++)
        line[pos++] = tbuf[i];
    line[pos++] = ']';
    line[pos++] = ' ';
    const char *tag = level_tag(level);
    for (int i = 0; tag[i] && pos < sizeof(line) - 1; i++)
        line[pos++] = tag[i];
    line[pos++] = ' ';
    for (int i = 0; msg[i] && pos < sizeof(line) - 1; i++)
        line[pos++] = msg[i];
    if (pos > 0 && line[pos - 1] != '\n' && pos < sizeof(line) - 1)
        line[pos++] = '\n';
    line[pos] = 0;

    unsigned long flags = spin_lock_irqsave(&log_lock);
    if (log_ready)
        ring_put_locked(line, pos);
    spin_unlock_irqrestore(&log_lock, flags);

    if (level <= console_level && !panicking) {
        log_recursion++;
        kprintf("%s", line);
        log_recursion--;
    }
}

/* Minimal formatter: %s %c %d %i %u %x %p %lu %llu %% */
static void log_format(char *out, size_t outsz, const char *fmt, va_list args) {
    char *dst = out;
    char *end = out + outsz - 1;
    const char *p = fmt ? fmt : "";

    while (*p && dst < end) {
        if (*p != '%') {
            *dst++ = *p++;
            continue;
        }
        p++;
        if (*p == '%') { *dst++ = '%'; p++; continue; }

        /* long / long long modifiers */
        int is_long = 0, is_ll = 0;
        if (*p == 'l') {
            p++;
            if (*p == 'l') { is_ll = 1; p++; }
            else is_long = 1;
        }

        if (*p == 's') {
            const char *s = va_arg(args, const char *);
            if (!s) s = "(null)";
            while (*s && dst < end) *dst++ = *s++;
            p++;
        } else if (*p == 'c') {
            char c = (char)va_arg(args, int);
            *dst++ = c;
            p++;
        } else if (*p == 'd' || *p == 'i') {
            long long v;
            if (is_ll) v = va_arg(args, long long);
            else if (is_long) v = va_arg(args, long);
            else v = va_arg(args, int);
            char tmp[24];
            int n = 0, neg = 0;
            if (v < 0) { neg = 1; v = -v; }
            if (v == 0) tmp[n++] = '0';
            else while (v && n < 23) { tmp[n++] = '0' + (v % 10); v /= 10; }
            if (neg && dst < end) *dst++ = '-';
            while (n > 0 && dst < end) *dst++ = tmp[--n];
            p++;
        } else if (*p == 'u') {
            unsigned long long v;
            if (is_ll) v = va_arg(args, unsigned long long);
            else if (is_long) v = va_arg(args, unsigned long);
            else v = va_arg(args, unsigned);
            char tmp[24];
            int n = 0;
            if (v == 0) tmp[n++] = '0';
            else while (v && n < 23) { tmp[n++] = '0' + (v % 10); v /= 10; }
            while (n > 0 && dst < end) *dst++ = tmp[--n];
            p++;
        } else if (*p == 'x' || *p == 'p') {
            unsigned long long v;
            if (*p == 'p') {
                v = (unsigned long long)(uintptr_t)va_arg(args, void *);
                if (dst + 2 < end) { *dst++ = '0'; *dst++ = 'x'; }
            } else if (is_ll) v = va_arg(args, unsigned long long);
            else if (is_long) v = va_arg(args, unsigned long);
            else v = va_arg(args, unsigned);
            char tmp[24];
            int n = 0;
            if (v == 0) tmp[n++] = '0';
            else while (v && n < 20) {
                unsigned d = (unsigned)(v & 0xf);
                tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
                v >>= 4;
            }
            while (n > 0 && dst < end) *dst++ = tmp[--n];
            p++;
        } else {
            *dst++ = '%';
            if (dst < end && *p) *dst++ = *p;
            if (*p) p++;
        }
    }
    *dst = 0;
}

void klog(int level, const char *fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    log_format(buf, sizeof(buf), fmt, args);
    va_end(args);
    log_append(level, buf);
}

size_t log_seq(void) {
    unsigned long flags = spin_lock_irqsave(&log_lock);
    size_t s = log_abs_seq;
    spin_unlock_irqrestore(&log_lock, flags);
    return s;
}

size_t log_size(void) {
    unsigned long flags = spin_lock_irqsave(&log_lock);
    size_t n = log_len;
    spin_unlock_irqrestore(&log_lock, flags);
    return n;
}

size_t log_read(char *buf, size_t len, size_t off) {
    if (!buf || len == 0) return 0;

    unsigned long flags = spin_lock_irqsave(&log_lock);
    if (log_len == 0) {
        spin_unlock_irqrestore(&log_lock, flags);
        return 0;
    }

    /* Absolute window: [log_abs_seq - log_len , log_abs_seq) */
    size_t oldest = log_abs_seq - log_len;
    if (off < oldest)
        off = oldest;          /* clamp to oldest available */
    if (off >= log_abs_seq) {
        spin_unlock_irqrestore(&log_lock, flags);
        return 0;
    }

    size_t avail = log_abs_seq - off;
    if (len > avail) len = avail;

    /* Position of `off` inside the circular buffer */
    size_t start = (log_head + LOG_BUF_SIZE - log_len + (off - oldest)) % LOG_BUF_SIZE;
    for (size_t i = 0; i < len; i++)
        buf[i] = log_buf[(start + i) % LOG_BUF_SIZE];

    spin_unlock_irqrestore(&log_lock, flags);
    return len;
}

void log_dump_recent(unsigned max_lines) {
    if (max_lines == 0) max_lines = 24;

    /* Never block in panic path */
    if (!spin_trylock(&log_lock)) {
        kprintf("(log lock busy — skipping dump)\n");
        return;
    }

    if (log_len == 0) {
        spin_unlock(&log_lock);
        kprintf("(log empty)\n");
        return;
    }

    /* Copy into a static buffer (never on the stack) */
    static char tmp[LOG_BUF_SIZE];
    size_t n = log_len;
    size_t start = (log_head + LOG_BUF_SIZE - log_len) % LOG_BUF_SIZE;
    for (size_t i = 0; i < n; i++)
        tmp[i] = log_buf[(start + i) % LOG_BUF_SIZE];
    tmp[n] = 0;
    spin_unlock(&log_lock);

    /* Walk from the end counting newlines */
    unsigned lines = 0;
    size_t i = n;
    while (i > 0 && lines < max_lines) {
        i--;
        if (tmp[i] == '\n') lines++;
    }
    if (i > 0 && tmp[i] == '\n') i++;

    kprintf("--- recent log (%u lines) ---\n", lines);
    kprintf("%s", tmp + i);
    if (n > 0 && tmp[n - 1] != '\n')
        kprintf("\n");
    kprintf("--- end log ---\n");
}

/* Validated frame-pointer walk. Only follows pointers that look like
 * kernel stack addresses and are properly aligned. */
void log_stack_trace(void *frame_ptr, unsigned max_frames) {
    if (max_frames == 0 || max_frames > 12)
        max_frames = 12;

    kprintf("Stack trace (validated, max %u frames):\n", max_frames);

    /* Kernel stacks live in identity-mapped low memory. Conservative bounds. */
    const uintptr_t KSTACK_LO = 0x1000;
    const uintptr_t KSTACK_HI = 0x04000000; /* 64 MB */

#if defined(__x86_64__)
    uintptr_t *fp = (uintptr_t *)frame_ptr;
    for (unsigned i = 0; i < max_frames && fp; i++) {
        uintptr_t fpa = (uintptr_t)fp;
        if ((fpa & 0x7) != 0) break;                 /* 8-byte aligned */
        if (fpa < KSTACK_LO || fpa >= KSTACK_HI) break;
        uintptr_t ret = fp[1];
        if (ret < KSTACK_LO || ret >= 0xFFFFFFFFUL) break;
        kprintf("  #%u  %p\n", i, (void *)ret);
        uintptr_t *next = (uintptr_t *)fp[0];
        if (next <= fp) break;
        fp = next;
    }
#elif defined(__i386__)
    uint32_t *fp = (uint32_t *)frame_ptr;
    for (unsigned i = 0; i < max_frames && fp; i++) {
        uint32_t fpa = (uint32_t)(uintptr_t)fp;
        if ((fpa & 0x3) != 0) break;
        if (fpa < KSTACK_LO || fpa >= KSTACK_HI) break;
        uint32_t ret = fp[1];
        if (ret < KSTACK_LO) break;
        kprintf("  #%u  0x%x\n", i, ret);
        uint32_t *next = (uint32_t *)fp[0];
        if (next <= fp) break;
        fp = next;
    }
#else
    (void)frame_ptr;
    kprintf("  (no architecture support)\n");
#endif
}
