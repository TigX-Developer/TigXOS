// test.h - Tiny test framework for TigxOS V1.0.58
#ifndef TEST_H
#define TEST_H

#include <stdint.h>
#include <stddef.h>

/*
 * Lightweight test framework.
 *
 * Usage:
 *   struct test_ctx t;
 *   test_begin(&t, "suite_name");
 *   test_check(&t, cond, "description");
 *   test_note(&t, "extra info: %d\n", x);
 *   test_end(&t);
 *
 * Output uses CP437 box glyphs (ISO.F16) — never UTF-8 and the
 * semantic palette from console.h.
 */

struct test_ctx {
    const char *name;
    int         checks;
    int         failed;
    uint32_t    start_tick;
    uint32_t    end_tick;
};

void test_begin(struct test_ctx *t, const char *name);
void test_check(struct test_ctx *t, int cond, const char *what);
void test_note(struct test_ctx *t, const char *fmt, ...);
void test_end(struct test_ctx *t);

/* Tick comparison helper: |got - want| <= want * tol_pct / 100, floor 1. */
int  test_tick_delta_ok(uint32_t got, uint32_t want, uint32_t tol_pct);

/* Global counters (reset by run_all_tests). Used for QEMU exit code. */
int  test_total_failed(void);
int  test_total_checks(void);
void test_reset_totals(void);


/* Registered test suites (defined in tests.c). */
void test_suite_memory(void);
void test_suite_fs(void);
void test_suite_timer(void);
void test_suite_sync(void);
void test_suite_process(void);
void test_suite_elf(void);
void test_suite_syscall(void);
void test_suite_waitq(void);
void test_suite_console(void);
void test_suite_regression(void);

#endif /* TEST_H */