/* test.c - Tiny test framework for TigxOS V1.0.58
 *
 * Uses CP437 single-byte box glyphs (ISO.F16).  Do NOT use UTF-8 here —
 * VGA text mode feeds each byte straight to the font, so multi-byte
 * UTF-8 sequences print as garbage (what you saw as weird characters).
 */
#include "test.h"
#include "console.h"
#include "timer.h"
#include "string.h"
#include <stdarg.h>

/* CP437 box-drawing (single byte each) */
#define BOX_TL  "\xDA"   /* ┌ */
#define BOX_TR  "\xBF"   /* ┐ */
#define BOX_BL  "\xC0"   /* └ */
#define BOX_BR  "\xD9"   /* ┘ */
#define BOX_H   "\xC4"   /* ─ */
#define BOX_V   "\xB3"   /* │ */

static int g_total_failed = 0;
static int g_total_checks = 0;

void test_reset_totals(void) {
    g_total_failed = 0;
    g_total_checks = 0;
}

int test_total_failed(void) { return g_total_failed; }
int test_total_checks(void) { return g_total_checks; }

void test_begin(struct test_ctx *t, const char *name) {
    t->name       = name ? name : "?";
    t->checks     = 0;
    t->failed     = 0;
    t->start_tick = timer_get_ticks();

    kprintf_color(TIG_HEADER, "\n" BOX_TL BOX_H " %s ", t->name);
    int n = (int)strlen(t->name);
    for (int i = n; i < 58; i++)
        kprintf_color(TIG_DIM, BOX_H);
    kprintf_color(TIG_HEADER, BOX_TR "\n");
}


void test_check(struct test_ctx *t, int cond, const char *what) {
    t->checks++;
    if (!cond)
        t->failed++;

    kprintf_color(TIG_DIM, BOX_V " ");
    if (cond)
        kprintf_color(TIG_OK, "[OK] ");
    else
        kprintf_color(TIG_FAIL, "[X]  ");
    kprintf_color(TIG_INFO, "%s\n", what ? what : "(null)");
}

void test_note(struct test_ctx *t, const char *fmt, ...) {
    (void)t;
    kprintf_color(TIG_DIM, BOX_V "   ");
    va_list ap;
    va_start(ap, fmt);
    vkprintf_color(TIG_METRIC, fmt, ap);
    va_end(ap);
}

void test_end(struct test_ctx *t) {
    t->end_tick = timer_get_ticks();
    uint32_t dt = t->end_tick - t->start_tick;

    g_total_checks += t->checks;
    g_total_failed += t->failed;

    kprintf_color(TIG_HEADER, BOX_BL BOX_H " ");
    if (t->failed == 0) {
        kprintf_color(TIG_OK, "PASS");
        kprintf_color(TIG_DIM, "  %d/%d checks  %u ticks\n\n",
                      t->checks, t->checks, (unsigned)dt);
    } else {
        kprintf_color(TIG_FAIL, "FAIL");
        kprintf_color(TIG_DIM, "  %d/%d checks  %u ticks\n\n",
                      t->checks - t->failed, t->checks, (unsigned)dt);
    }
}


int test_tick_delta_ok(uint32_t got, uint32_t want, uint32_t tol_pct) {
    uint32_t tol = want * tol_pct / 100;
    if (tol < 1)
        tol = 1;
    uint32_t lo = (want > tol) ? want - tol : 0;
    uint32_t hi = want + tol;
    return got >= lo && got <= hi;
}
