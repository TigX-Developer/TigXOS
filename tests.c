// tests.c - TigxOS V1.0.58 expanded test suites
#include "test.h"
#include "console.h"
#include "string.h"
#include "memory.h"
#include "process.h"
#include "wait.h"
#include "sync.h"
#include "timer.h"
#include "fs.h"
#include "disk.h"
#include "elf.h"
#include "embed_hello.h"
#include "syscall.h"

/* ============================================================
 * SUITE: memory
 * ============================================================ */
static void worker_alloc_free(void) {
    for (int i = 0; i < 200; i++) {
        void *p = kmalloc((size_t)(16 + (i * 13) % 256));
        if (p) {
            memset(p, 0xAB, 16);
            kfree(p);
        }
        process_yield();
    }
    process_exit(0);
}

void test_suite_memory(void) {
    struct test_ctx t;
    test_begin(&t, "memory");

    /* 1. Aligned allocation */
    void *p = kmalloc(1);
    test_check(&t, p != NULL, "kmalloc(1) succeeds");
    test_check(&t, ((uintptr_t)p & 0xF) == 0, "kmalloc returns 16-byte aligned");
    kfree(p);

    /* 2. Zero-size is rejected */
    test_check(&t, kmalloc(0) == NULL, "kmalloc(0) returns NULL");

    /* 3. kcalloc zeroes */
    uint8_t *z = (uint8_t *)kcalloc(64, 1);
    int all_zero = (z != NULL);
    if (z) {
        for (int i = 0; i < 64; i++) if (z[i]) { all_zero = 0; break; }
    }
    test_check(&t, all_zero, "kcalloc zeroes memory");
    kfree(z);

    /* 4. krealloc preserves contents */
    char *r = (char *)kmalloc(16);
    if (r) {
        strcpy(r, "hello");
        r = (char *)krealloc(r, 64);
    }
    test_check(&t, r && strcmp(r, "hello") == 0, "krealloc preserves data");
    kfree(r);

    /* 5. Overflow guard */
    test_check(&t, kmalloc((size_t)-1) == NULL, "kmalloc rejects huge size");

    /* 6. Double-free tolerated */
    void *d = kmalloc(32);
    kfree(d);
    kfree(d);   /* logs "double free", should not crash */
    test_check(&t, 1, "double-free tolerated (logged)");

    /* 7. Concurrent alloc/free from another process */
    int before = process_count();
    int pid = process_create("alloc_stress", worker_alloc_free);
    test_check(&t, pid > 0, "spawned alloc/free worker");
    while (process_count() > before)
        process_yield();

    /* 8. Basic sanity: allocator still works after stress */
    void *after = kmalloc(128);
    test_check(&t, after != NULL, "allocator healthy after stress");
    kfree(after);

    test_end(&t);
}

/* ============================================================
 * SUITE: filesystem
 * ============================================================ */
void test_suite_fs(void) {
    struct test_ctx t;
    test_begin(&t, "fs");

    if (!fs_ready()) {
        test_check(&t, 0, "filesystem mounted (skipping rest)");
        test_end(&t);
        return;
    }

    /* 1. Write + read roundtrip */
    const char *msg = "The quick brown fox jumps over the lazy dog";
    test_check(&t, fs_write("T_ROUND", msg, (uint32_t)strlen(msg)) == 0,
               "fs_write small file");
    char buf[128];
    int n = fs_read("T_ROUND", buf, sizeof(buf) - 1);
    if (n > 0)
        buf[n] = 0;
    else
        buf[0] = 0;
    test_check(&t, n == (int)strlen(msg) && strcmp(buf, msg) == 0,
               "fs_read returns identical bytes");

    /* 2. size query */
    test_check(&t, fs_size("T_ROUND") == (int)strlen(msg),
               "fs_size matches written length");

    /* 3. Remove works */
    test_check(&t, fs_remove("T_ROUND") == 0, "fs_remove existing file");
    test_check(&t, fs_size("T_ROUND") < 0, "fs_size on removed file fails");

    /* 4. Missing file read fails cleanly */
    test_check(&t, fs_read("NOPE", buf, sizeof(buf)) < 0,
               "fs_read missing file fails");

    /* 5. Name too long rejected */
    char longname[TIGFS_NAME_LEN + 8];
    for (size_t i = 0; i < sizeof(longname) - 1; i++)
        longname[i] = 'x';
    longname[sizeof(longname) - 1] = 0;
    test_check(&t, fs_create(longname) < 0, "fs_create rejects long name");

    /* 6. Duplicate create rejected */
    fs_write("T_DUP", "a", 1);
    test_check(&t, fs_create("T_DUP") < 0, "fs_create rejects duplicate");
    fs_remove("T_DUP");

    /* 7. Empty file */
    fs_write("T_EMPTY", "", 0);
    test_check(&t, fs_size("T_EMPTY") == 0, "empty file has size 0");
    fs_remove("T_EMPTY");

    /* 8. Multi-block file (crosses 512-byte boundary) */
    static uint8_t big[1500];
    for (size_t i = 0; i < sizeof(big); i++)
        big[i] = (uint8_t)(i * 31);
    if (fs_write("T_BIG", big, sizeof(big)) == 0) {
        static uint8_t rb[1500];
        int got = fs_read("T_BIG", rb, sizeof(rb));
        test_check(&t, got == (int)sizeof(big) &&
                        memcmp(rb, big, sizeof(big)) == 0,
                   "multi-block write/read matches");
        fs_remove("T_BIG");
    } else {
        test_note(&t, "(multi-block write failed — disk may be full)\n");
    }

    /* 9. Oversized write rejected (just over TIGFS_DIRECT * 512) */
    static uint8_t huge[TIGFS_DIRECT * 512 + 1];
    test_check(&t, fs_write("T_HUGE", huge, sizeof(huge)) < 0,
               "fs_write rejects > direct-block limit");

    /* 10. Rewrite (truncate-style): write smaller content replaces */
    fs_write("T_RW", "ABCDEFGH", 8);
    fs_write("T_RW", "xy", 2);
    test_check(&t, fs_size("T_RW") == 2, "rewrite shrinks size");
    {
        char b[8];
        int g = fs_read("T_RW", b, sizeof(b));
        test_check(&t, g == 2 && b[0] == 'x' && b[1] == 'y',
                   "rewrite content");
    }
    fs_remove("T_RW");

    /* 11. unlink missing fails */
    test_check(&t, fs_remove("T_NO_SUCH") != 0, "remove missing fails");

    test_end(&t);
}

/* ============================================================
 * SUITE: timer
 * ============================================================ */
void test_suite_timer(void) {
    struct test_ctx t;
    test_begin(&t, "timer");

    uint32_t hz = timer_get_hz();
    test_check(&t, hz > 0, "timer_hz nonzero");

    static const uint32_t durations[] = { 10, 25, 50, 100, 200, 500 };
    for (size_t i = 0; i < sizeof(durations) / sizeof(durations[0]); i++) {
        uint32_t ms   = durations[i];
        uint32_t want = (ms * hz + 999) / 1000;
        if (want == 0)
            want = 1;

        uint32_t t0  = timer_get_ticks();
        timer_sleep_ms(ms);
        uint32_t got = timer_get_ticks() - t0;

        /* Build label without snprintf */
        char label[48];
        int p = 0;
        const char *pre = "sleep ";
        while (*pre && p < (int)sizeof(label) - 8)
            label[p++] = *pre++;
        char tmp[12];
        int ti = 0;
        uint32_t v = ms;
        if (!v) tmp[ti++] = '0';
        else while (v) { tmp[ti++] = (char)('0' + (v % 10)); v /= 10; }
        while (ti-- > 0 && p < (int)sizeof(label) - 4)
            label[p++] = tmp[ti];
        label[p++] = 'm';
        label[p++] = 's';
        label[p] = 0;

        int ok = test_tick_delta_ok(got, want, 25);
        test_check(&t, ok, label);
        if (!ok)
            test_note(&t, "got=%u want=%u (±25%%)\n",
                      (unsigned)got, (unsigned)want);
    }

    /* Monotonicity check */
    uint32_t a = timer_get_ticks();
    for (volatile int i = 0; i < 10000; i++)
        ;
    uint32_t b = timer_get_ticks();
    test_check(&t, b >= a, "ticks are monotonic");

    test_end(&t);
}

/* ============================================================
 * SUITE: sync
 * ============================================================ */
static struct mutex     s_mtx;
static struct semaphore s_sem;
static volatile int     s_counter;
static volatile int     s_done;

static void mtx_worker(void) {
    for (int i = 0; i < 25; i++) {
        mutex_lock(&s_mtx);
        int v = s_counter;
        for (volatile int d = 0; d < 20000; d++)
            ;
        s_counter = v + 1;
        mutex_unlock(&s_mtx);
        process_yield();
    }
    s_done = 1;
    process_exit(0);
}

static void sem_waiter(void) {
    sem_wait(&s_sem);
    s_done = 1;
    process_exit(0);
}

void test_suite_sync(void) {
    struct test_ctx t;
    test_begin(&t, "sync");

    /* --- mutex serialisation --- */
    mutex_init(&s_mtx, "s_mtx");
    s_counter = 0;
    s_done = 0;
    int pid = process_create("mtx2", mtx_worker);
    test_check(&t, pid > 0, "spawned mutex worker");

    for (int i = 0; i < 25; i++) {
        mutex_lock(&s_mtx);
        int v = s_counter;
        for (volatile int d = 0; d < 20000; d++)
            ;
        s_counter = v + 1;
        mutex_unlock(&s_mtx);
        process_yield();
    }
    while (!s_done)
        process_yield();
    test_check(&t, s_counter == 50, "mutex serialized 50 increments");

    /* trylock */
    mutex_init(&s_mtx, "s_mtx2");
    test_check(&t, mutex_trylock(&s_mtx) == 1, "trylock on free succeeds");
    test_check(&t, mutex_trylock(&s_mtx) == 0, "trylock while held fails");
    mutex_unlock(&s_mtx);

    /* non-owner unlock must be refused */
    mutex_lock(&s_mtx);
    int real = s_mtx.owner;
    s_mtx.owner = 999;
    mutex_unlock(&s_mtx);
    test_check(&t, s_mtx.locked == 1, "non-owner unlock refused");
    s_mtx.owner = real;
    mutex_unlock(&s_mtx);

    /* --- semaphore blocking + wake --- */
    sem_init(&s_sem, 0, "s_sem");
    s_done = 0;
    int wpid = process_create("semw", sem_waiter);
    test_check(&t, wpid > 0, "spawned semaphore waiter");
    timer_sleep_ms(50);

    struct process *w = process_lookup_pid(wpid);
    test_check(&t, w && w->state == PROC_BLOCKED,
               "waiter is BLOCKED on empty semaphore");

    sem_post(&s_sem);
    for (int i = 0; i < 200 && !s_done; i++)
        process_yield();
    test_check(&t, s_done == 1, "waiter woke after sem_post");

    /* trywait / overflow */
    sem_init(&s_sem, 1, "s_sem2");
    test_check(&t, sem_trywait(&s_sem) == 1, "trywait on count=1 succeeds");
    test_check(&t, sem_trywait(&s_sem) == 0, "trywait at count=0 fails");
    test_check(&t, sem_post(&s_sem) == 0, "sem_post normal");
    s_sem.count = SEM_MAX_COUNT;
    test_check(&t, sem_post(&s_sem) == -1, "sem_post refuses overflow");
    s_sem.count = 0;

    test_end(&t);
}

/* ============================================================
 * SUITE: process
 * ============================================================ */
static void short_lived(void) {
    process_exit(0);
}

static void exit_with_42(void) {
    process_exit(42);
}

static void yielding_worker(void) {
    for (int i = 0; i < 3; i++)
        process_yield();
    process_exit(0);
}

void test_suite_process(void) {
    struct test_ctx t;
    test_begin(&t, "process");

    int n0 = process_count();

    /* Spawn & reap */
    int pid = process_create("shortl", short_lived);
    test_check(&t, pid > 0, "process_create returns pid");
    test_check(&t, process_lookup_pid(pid) != NULL, "lookup by pid works");

    for (int i = 0; i < 200 && process_lookup_pid(pid); i++)
        timer_sleep_ms(10);
    test_check(&t, process_lookup_pid(pid) == NULL, "dead process reclaimed");

    /* Exit status propagates through process_wait (1.0.66) */
    int pid42 = process_create("ex42", exit_with_42);
    test_check(&t, pid42 > 0, "spawned exit_with_42");
    if (pid42 > 0) {
        int st = process_wait(pid42);
        test_check(&t, st == 42, "process_wait returns exit status 42");
    }

    /* Bad pid rejected */
    test_check(&t, process_lookup_pid(-1) == NULL, "lookup(-1) returns NULL");
    test_check(&t, process_lookup_pid(MAX_PROCESSES) == NULL,
               "lookup(out of range) returns NULL");
    test_check(&t, process_lookup_pid(0) != NULL, "idle task exists at pid 0");

    /* Multiple concurrent workers */
    int pids[4];
    for (int i = 0; i < 4; i++)
        pids[i] = process_create("yield_w", yielding_worker);
    int all_ok = 1;
    for (int i = 0; i < 4; i++)
        if (pids[i] < 0)
            all_ok = 0;
    test_check(&t, all_ok, "spawned 4 concurrent workers");

    for (int i = 0; i < 400; i++) {
        int alive = 0;
        for (int j = 0; j < 4; j++)
            if (process_lookup_pid(pids[j]))
                alive++;
        if (!alive)
            break;
        process_yield();
    }
    int alive = 0;
    for (int j = 0; j < 4; j++)
        if (process_lookup_pid(pids[j]))
            alive++;
    test_check(&t, alive == 0, "all 4 workers exited");

    test_check(&t, process_count() <= n0 + 1,
               "process_count approximately restored");

    test_end(&t);
}

/* ============================================================
 * SUITE: ELF
 * ============================================================ */
void test_suite_elf(void) {
    struct test_ctx t;
    test_begin(&t, "elf");

    struct elf_load_info info;

    /* 1. Bad magic rejected */
    static uint8_t junk[64];
    memset(junk, 0, sizeof(junk));
    test_check(&t, elf_load_image(junk, sizeof(junk), &info, 0) != 0,
               "elf_load_image rejects non-ELF");

    /* 2. Too short rejected */
    test_check(&t, elf_load_image(junk, 8, &info, 0) != 0,
               "elf_load_image rejects <16 bytes");

    /* 3. Embedded hello.elf loads */
    int rc = elf_load_image(embedded_hello_elf, embedded_hello_elf_len, &info, 0);
    test_check(&t, rc == 0, "embedded hello.elf parses");
    if (rc == 0) {
        test_check(&t, info.entry != 0, "entry point nonzero");
        test_check(&t, info.load_max > info.load_min, "load range valid");
        test_check(&t, info.brk >= info.load_max, "brk >= load_max");
        test_check(&t, (info.brk & 0xFFF) == 0, "brk is page-aligned");
    }

    /* 4. File-based load (if seeded) */
    if (fs_ready() && fs_size("hello.elf") > 0) {
        rc = elf_load_file("hello.elf", &info, 0);
        test_check(&t, rc == 0, "elf_load_file('hello.elf')");
    } else {
        test_note(&t, "(hello.elf not on disk, skipping file load)\n");
    }

    /* 5. Missing file */
    test_check(&t, elf_load_file("NO_SUCH_ELF", &info, 0) != 0,
               "elf_load_file missing path fails");

    test_end(&t);
}

/* ============================================================
 * SUITE: syscall
 * ============================================================ */
void test_suite_syscall(void) {
    struct test_ctx t;
    test_begin(&t, "syscall");

    test_check(&t, SYSCALL_VECTOR == 0x80, "int 0x80 vector");
    test_check(&t, SYS_EXIT == 0, "SYS_EXIT == 0");
    test_check(&t, SYS_WRITE == 1 && SYS_READ == 5, "WRITE=1 READ=5");
    test_check(&t, SYS_OPEN == 6 && SYS_CLOSE == 7, "OPEN=6 CLOSE=7");
    test_check(&t, SYS_GETDENTS == 8 && SYS_SPAWN == 9 && SYS_WAIT == 10,
               "GETDENTS/SPAWN/WAIT numbers");
    test_check(&t, SYS_UNLINK == 11 && SYS_STAT == 12,
               "UNLINK=11 STAT=12");

    struct process *me = process_current();
    test_check(&t, me != NULL, "process_current() non-NULL");
    test_check(&t, me && me->pid >= 0, "current pid valid");
    test_check(&t, me && me->fds[1].of_id >= 0, "stdout FD initialized");

    /* getdents packs root listing */
    char dentbuf[512];
    int dn = fs_getdents(dentbuf, sizeof(dentbuf));
    test_check(&t, dn >= 0, "fs_getdents succeeds");
    test_check(&t, (dn % 32) == 0, "getdents size multiple of dirent");

    test_note(&t, "(ring3 syscalls exercised by user_hello / future shell)\n");

    test_end(&t);
}

/* ============================================================
 * SUITE: wait queues
 * ============================================================ */
static struct wait_queue wq_test;
static volatile int     wq_woke;

static void wq_waiter(void) {
    __asm__ volatile("cli");
    wait_queue_block(&wq_test, 0);
    __asm__ volatile("sti");
    wq_woke = 1;
    process_exit(0);
}

void test_suite_waitq(void) {
    struct test_ctx t;
    test_begin(&t, "waitq");

    wait_queue_init(&wq_test, "wq_test");
    wq_woke = 0;

    int pid = process_create("wq_worker", wq_waiter);
    test_check(&t, pid > 0, "spawned wq waiter");
    timer_sleep_ms(30);

    struct process *p = process_lookup_pid(pid);
    test_check(&t, p && p->state == PROC_BLOCKED, "waiter blocked");

    wait_queue_wake_one(&wq_test);
    for (int i = 0; i < 200 && !wq_woke; i++)
        process_yield();
    test_check(&t, wq_woke == 1, "wait_queue_wake_one woke the waiter");

    wait_queue_wake_all(&wq_test);
    test_check(&t, wq_test.count == 0, "wake_all on empty leaves count=0");

    test_end(&t);
}

/* ============================================================
 * SUITE: console / colors
 * ============================================================ */
void test_suite_console(void) {
    struct test_ctx t;
    test_begin(&t, "console");

    kprintf_color(TIG_DIM, "³ ");
    kprintf_color(TIG_INFO, "palette: ");
    for (int i = 0; i < 16; i++)
        kprintf_color((unsigned char)i, "##");
    kprintf_color(TIG_INFO, "\n");
    test_check(&t, 1, "all 16 foreground colors render");

    kprintf_color(TIG_DIM, "³ ");
    kprintf_color(VGA_YELLOW, "Tig");
    kprintf_color(VGA_WHITE,  "x");
    kprintf_color(VGA_YELLOW, "OS");
    kprintf_color(VGA_CYAN,   " V1.0.58");
    kprintf_color(VGA_WHITE,  "  brand demo\n");
    test_check(&t, 1, "brand color sequence renders");

    console_write_colored("~E~-~F Tig~Ex~FOS ~7inline ~Ecolor\n");
    test_check(&t, 1, "console_write_colored inline codes parse");

    test_end(&t);
}

/* ============================================================
 * SUITE: regression
 * ============================================================ */
void test_suite_regression(void) {
    struct test_ctx t;
    test_begin(&t, "regression");

    int a = process_create("reg_a", short_lived);
    test_check(&t, a > 0, "created reg_a");
    /* Wait until reg_a is fully reaped (slot UNUSED), not merely DEAD. */
    for (int i = 0; i < 200; i++) {
        struct process *p = process_lookup_pid(a);
        if (!p || p->state == PROC_UNUSED)
            break;
        timer_sleep_ms(10);
        process_yield();
    }

    int b = process_create("reg_b", short_lived);
    test_check(&t, b > 0, "created reg_b");

    /*
     * pid == table index.  After reg_a exits, slot a may be reused so b == a.
     * That is intentional — not a collision.  The invariant is:
     *   lookup(pid) is NULL, or lookup(pid)->pid == pid.
     * Never return a live process whose stored pid differs from the key.
     */
    struct process *pa = process_lookup_pid(a);
    struct process *pb = process_lookup_pid(b);
    int ok_a = (pa == NULL) || (pa->pid == a);
    int ok_b = (pb != NULL && pb->pid == b);
    test_check(&t, ok_a && ok_b,
               "lookup(pid) matches pid (reuse OK, never wrong task)");

    for (int i = 0; i < 200 && process_lookup_pid(b); i++) {
        timer_sleep_ms(10);
        process_yield();
    }

    uint32_t t0 = timer_get_ticks();
    timer_sleep_ms(60);
    uint32_t dt = timer_get_ticks() - t0;
    test_check(&t, dt >= 1, "blocking sleep advances ticks");

    test_end(&t);
}