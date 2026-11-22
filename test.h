/* test.h — Test framework for TigxOS (1.2.8-update3) */
#ifndef TEST_H
#define TEST_H

#include <stdint.h>
#include <stddef.h>

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

/* Soft check: records a note on failure but still counts as a check.
 * Use for environment-dependent assertions (e.g. multi-CPU only on -smp N). */
void test_check_soft(struct test_ctx *t, int cond, const char *what);

int  test_tick_delta_ok(uint32_t got, uint32_t want, uint32_t tol_pct);

int  test_total_failed(void);
int  test_total_checks(void);
void test_reset_totals(void);

/* Suites (tests.c) */
void test_suite_memory(void);
void test_suite_fs(void);
void test_suite_vfs(void);
void test_suite_timer(void);
void test_suite_sync(void);
void test_suite_process(void);
void test_suite_elf(void);
void test_suite_syscall(void);
void test_suite_waitq(void);
void test_suite_console(void);
void test_suite_string(void);
void test_suite_pipe(void);
void test_suite_irq(void);
void test_suite_rtc(void);
void test_suite_gdt(void);
void test_suite_aspace(void);
void test_suite_sched(void);
void test_suite_ramfs(void);
void test_suite_regression(void);
void test_suite_smp(void);      /* new: LAPIC / SMP foundation */
void test_suite_atomic(void);   /* new: atomic.h primitives */

#endif /* TEST_H */
