// kernel.c - TigxOS V1.0.58
// Expanded test framework + semantic color palette
#include "version.h"
#include "interrupt.h"
#include "console.h"
#include "keyboard.h"
#include "timer.h"
#include "memory.h"
#include "aspace.h"
#include "process.h"
#include "filetable.h"
#include "disk.h"
#include "fs.h"
#include "wait.h"
#include "sync.h"
#include "font.h"
#include "gdt.h"
#include "syscall.h"
#include "pipe.h"
#include "string.h"
#include "elf.h"
#include "embed_hello.h"
#include "test.h"
#include <stdint.h>

/* ---------- Test registry ---------- */
struct test_entry {
    char        key;
    const char *name;
    void      (*fn)(void);
};

#ifdef TIGX_BOOT_TESTS
#include "io.h"

static const struct test_entry test_table[] = {
    { '1', "sync",       test_suite_sync       },
    { '2', "waitq",      test_suite_waitq      },
    { '3', "timer",      test_suite_timer      },
    { '4', "process",    test_suite_process    },
    { '5', "memory",     test_suite_memory     },
    { '6', "fs",         test_suite_fs         },
    { '7', "elf",        test_suite_elf        },
    { '8', "syscall",    test_suite_syscall    },
    { '9', "console",    test_suite_console    },
    { '0', "regression", test_suite_regression },
};
#define TEST_TABLE_N (sizeof(test_table)/sizeof(test_table[0]))

/* isa-debug-exit: QEMU exits with status (code << 1) | 1 */
#define DEBUG_EXIT_PORT 0xf4

static void qemu_exit(uint8_t code) {
    outb(DEBUG_EXIT_PORT, code);
    /* If the device is missing we just hang cleanly */
    while (1)
        __asm__ volatile("hlt");
}

/*
 * Run every suite. Must be called with interrupts enabled and the
 * scheduler running — otherwise timer_sleep_ms (which blocks the
 * current process) hangs forever. That was the classic timer-test bug:
 * suites used to run before sti / scheduler_start.
 */
static void run_all_tests(void) {
    test_reset_totals();
    kprintf_color(TIG_WARN, "\n╔══ ALL TEST SUITES ═════════════════════════════════════════════╗\n");
    for (size_t i = 0; i < TEST_TABLE_N; i++)
        test_table[i].fn();
    kprintf_color(TIG_WARN, "╚════════════════════════════════════════════════════════════════╝\n\n");

    int failed = test_total_failed();
    int checks = test_total_checks();
    if (failed == 0) {
        kprintf_color(TIG_OK, "ALL TESTS PASSED (%d checks)\n", checks);
        qemu_exit(0);           /* QEMU status 1 → success for make */
    } else {
        kprintf_color(TIG_FAIL, "TESTS FAILED: %d/%d\n", failed, checks);
        qemu_exit(1);           /* QEMU status 3 → failure */
    }
}

/* Kernel thread entry that runs the suites then exits QEMU. */
static void test_runner_task(void) {
    run_all_tests();
    /* not reached */
}
#endif


/* ---------- Banner ---------- */
static void print_banner(void) {
    kprintf_color(TIG_BRAND_A, "Tig");
    kprintf_color(TIG_BRAND_B, "x");
    kprintf_color(TIG_BRAND_A, "OS");
    kprintf_color(TIG_INFO,    " V" TIGX_VERSION);
    kprintf_color(TIG_ACCENT,  " — ring3 + syscalls + test framework\n");
}



/*
 * Only the shell is still embedded in the kernel (needed as init).
 * All other programs are written into the disk image at build time by
 * tools/mktigfs.py. At boot we only ensure shell.elf exists so a
 * freshly-formatted FS (or a corrupted one) can still reach a prompt.
 */
static void seed_shell_if_missing(void) {
    if (!fs_ready()) {
        kprintf("seed: FS not ready\n");
        return;
    }
    if (fs_size("shell.elf") >= 0)
        return; /* already on disk (normal path after mktigfs) */

    for (unsigned i = 0; i < embedded_elves_count; i++) {
        const struct embedded_elf *e = &embedded_elves[i];
        if (!e->name || !e->data || e->len == 0)
            continue;
        if (strcmp(e->name, "shell.elf") != 0)
            continue;
        int rc = fs_write(e->name, e->data, e->len);
        if (rc == 0)
            kprintf("seeded shell.elf from kernel image (%u bytes)\n", e->len);
        else
            kprintf("Failed to seed shell.elf (rc=%d)\n", rc);
        return;
    }
    kprintf("seed: no embedded shell.elf\n");
}



/* ---------- kmain ---------- */
void kmain(void) {
    serial_init();

    console_clear();
    font_init();

    print_banner();
    kprintf_color(TIG_HEADER, "Req: ");
    kprintf_color(TIG_INFO, "128 MB RAM installed / 512 MB disk\n");
    kprintf_color(TIG_WARN, "======================================\n\n");

    memory_init();
    aspace_init();

    kprintf_color(TIG_HEADER, "GDT+TSS...\n");
    gdt_init();
    kprintf_color(TIG_HEADER, "IDT...\n");
    idt_init();
    kprintf_color(TIG_HEADER, "Syscalls...\n");
    syscall_init();
    kprintf_color(TIG_HEADER, "Timer...\n");
    timer_init(100);
    kprintf_color(TIG_HEADER, "Keyboard...\n");
    keyboard_init();
    kprintf_color(TIG_HEADER, "Disk...\n");
    disk_init();
    kprintf_color(TIG_HEADER, "FS...\n");
    fs_init();
    kprintf_color(TIG_WARN, "\nProcesses (preemptive, blocking)...\n");
    /* filetable BEFORE process_init — idle's stdio allocates console_of */
    filetable_init();
    pipe_init();
    process_init();
    seed_shell_if_missing();


    /* Userland shell owns the keyboard. Do NOT start kernel shell_task —
       it single-key binds (h=help, l=list, …) and steals typed commands. */
    {
        char *argv[] = { "shell.elf", NULL };
        int spid = spawn("shell.elf", argv, NULL);
        if (spid > 0)
            kprintf_color(TIG_OK, "shell ready (pid %d)\n", spid);
        else
            kprintf_color(TIG_FAIL, "shell.elf spawn failed\n");
    }
    process_list();

    __asm__ volatile("sti");
    kprintf_color(TIG_OK, "Interrupts on — timer will preempt\n");
    scheduler_start();

#ifdef TIGX_BOOT_TESTS
    /*
     * Timer suite (and any blocking test) needs a real scheduled process
     * with IRQs live. Create a kernel thread to run the suites; it will
     * exit QEMU via isa-debug-exit when finished.
     */
    {
        int tid = process_create("test-runner", test_runner_task);
        if (tid < 0)
            kprintf_color(TIG_FAIL, "test-runner spawn failed\n");
        else
            kprintf_color(TIG_INFO, "test-runner started (pid %d)\n", tid);
    }
#endif

    while (1) {
        process_yield();
        __asm__ volatile("hlt");
    }
}
