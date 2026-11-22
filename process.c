/* process.c — preemptive scheduler, wait/kill, fg job, ps snapshot (TigxOS 1.2.3) */



#include "process.h"
#include "smp.h"
#include "filetable.h"
#include "memory.h"
#include "console.h"
#include "string.h"
#include "timer.h"
#include "wait.h"
#include "spinlock.h"
#include "arch/arch.h"
#include "gdt.h"
#include "elf.h"
#include "embed_hello.h"
#include "pipe.h"
#include "aspace.h"

#define TIME_SLICE_TICKS 5  /* ~50 ms at 100 Hz */

static struct process process_table[MAX_PROCESSES];
static int scheduler_enabled = 0;

/* Per-CPU data (SMP-ready). Only CPU 0 is online until 1.2.8. */
static struct cpu_data cpus[NCPU_MAX];
static int ncpu_online = 1;

/* Protects process_table state transitions and pick_next. */
static spinlock_t sched_lock;

/* Round-robin cursor per priority level (CPU 0). */
static int rr_cursor[PRIO_MAX + 1];

struct cpu_data *cpu_this(void) {
    int id = smp_cpu_id();
    if (id < 0 || id >= NCPU_MAX)
        id = 0;
    return &cpus[id];
}

int cpu_count_online(void) {
    return ncpu_online;
}

void smp_mark_online(int count) {
    if (count < 1) count = 1;
    if (count > NCPU_MAX) count = NCPU_MAX;
    ncpu_online = count;
    for (int i = 0; i < NCPU_MAX; i++)
        cpus[i].online = (i < count);
}

/* Convenience: current process on this CPU (per-CPU after 1.2.9) */
#define current (cpu_this()->cur)

void process_init(void) {
    memset(process_table, 0, sizeof(process_table));
    memset(cpus, 0, sizeof(cpus));
    memset(rr_cursor, 0, sizeof(rr_cursor));
    scheduler_enabled = 0;
    ncpu_online = 1;
    spin_lock_init(&sched_lock);

    for (int i = 0; i < NCPU_MAX; i++) {
        cpus[i].id = i;
        cpus[i].online = (i == 0);
        cpus[i].cur = NULL;
        cpus[i].switches = 0;
        cpus[i].ticks = 0;
    }

    /*
     * pid 0 = idle at table index 0.
     * It does not get its own allocated stack up front:
     * kmain()'s trailing yield/hlt loop IS idle. The first context_switch
     * out of idle stores the bootstrap stack pointer into idle->rsp.
     */
    struct process *idle = &process_table[0];
    idle->pid = 0;
    idle->state = PROC_READY;
    idle->parent_pid = -1;
    idle->wait_pid = -1;
    idle->exit_status = 0;
    process_init_fds(idle);
    strncpy(idle->name, "idle", PROCESS_NAME_LEN - 1);
    idle->stack_base = NULL;
    idle->rsp = NULL;
    idle->kernel_stack_top = 0x90000; /* bootstrap stack */
    idle->user_stack_base = NULL;
    idle->is_user = 0;
    idle->time_slice = TIME_SLICE_TICKS;
    idle->priority = PRIO_IDLE;
    idle->ticks_ran = 0;
    idle->on_cpu = 0;
    idle->wake_tick = 0;
    strncpy(idle->cwd, "/", PATH_MAX - 1);
    idle->cwd[PATH_MAX - 1] = '\0';
    cpus[0].cur = idle;
    tss_set_kernel_stack(idle->kernel_stack_top);

    kprintf("Process subsystem OK (preemptive, blocking, ring3, SMP-ready)\n");
}

static uintptr_t *build_stack(void (*entry)(void), uintptr_t *base) {
    uintptr_t *sp = (uintptr_t *)((uint8_t *)base + PROCESS_STACK_SIZE);
    sp = (uintptr_t *)((uintptr_t)sp & ~(sizeof(uintptr_t) - 1));
#if defined(__i386__)
    /* Align to 16 bytes for ABI comfort */
    sp = (uintptr_t *)((uintptr_t)sp & ~0xFUL);
    *--sp = (uintptr_t)entry;
    *--sp = (uintptr_t)task_trampoline; /* sti, then ret into entry */
    *--sp = 0; /* edi */
    *--sp = 0; /* esi */
    *--sp = 0; /* ebx */
    *--sp = 0; /* ebp */
#else
    sp = (uintptr_t *)((uintptr_t)sp & ~0xFULL);
    *--sp = (uintptr_t)entry;
    *--sp = (uintptr_t)task_trampoline; /* sti, then ret into entry */
    *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;
#endif
    return sp;
}

/*
 * pid is the table index. This keeps process_lookup_pid O(1) and correct
 * forever, and prevents the old monotonic-pid vs slot mismatch that broke
 * wait-queue wakeups after create/exit cycles.
 */

static int console_of = -1;

void process_init_fds(struct process *p) {
    if (!p) return;
    if (console_of < 0) {
        console_of = of_alloc();
        if (console_of >= 0) {
            struct open_file *o = of_get(console_of);
            if (o) {
                o->type = OF_CONSOLE;
                /* of_alloc set refs=1; process_init_fds will incref per stdio */
            }
        }
    }
    for (int i = 0; i < MAX_FDS; i++)
        p->fds[i].of_id = -1;
    for (int i = 0; i < 3; i++) {
        p->fds[i].of_id = console_of;
        if (console_of >= 0)
            of_incref(console_of);
    }
    /* Drop the of_alloc baseline ref once (first process only keeps it via stdio).
     * Subsequent processes only of_incref. */
    static int console_base_dropped;
    if (!console_base_dropped && console_of >= 0) {
        of_decref(console_of);
        console_base_dropped = 1;
    }
}

/* Inherit stdio (0/1/2) by sharing the same open_file objects (refcount++).
 * Offsets for OF_FILE are shared; close in parent does not close child. */
void process_inherit_fds(struct process *child, struct process *parent) {
    if (!child) return;
    if (!parent) {
        process_init_fds(child);
        return;
    }
    for (int i = 0; i < MAX_FDS; i++)
        child->fds[i].of_id = -1;
    for (int i = 0; i < 3 && i < MAX_FDS; i++) {
        int id = parent->fds[i].of_id;
        child->fds[i].of_id = id;
        if (id >= 0)
            of_incref(id);
    }
}

int process_alloc_fd(struct process *p) {
    if (!p) return -1;
    for (int i = 3; i < MAX_FDS; i++) {
        if (p->fds[i].of_id < 0)
            return i;
    }
    return -1;
}

void fd_close(struct process *p, int fd) {
    if (!p || fd < 0 || fd >= MAX_FDS)
        return;
    int id = p->fds[fd].of_id;
    if (id < 0)
        return;
    p->fds[fd].of_id = -1;
    of_decref(id);
}

int fd_type_of(struct process *p, int fd) {
    if (!p || fd < 0 || fd >= MAX_FDS)
        return FD_NONE;
    struct open_file *o = of_get(p->fds[fd].of_id);
    if (!o) return FD_NONE;
    if (o->type == OF_CONSOLE) return FD_CONSOLE;
    if (o->type == OF_FILE) return FD_FILE;
    if (o->type == OF_PIPE) return FD_PIPE;
    return FD_NONE;
}

void process_set_parent(struct process *child, int parent_pid) {
    if (child)
        child->parent_pid = parent_pid;
}

static void process_wake_waiters(int dead_pid) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        struct process *p = &process_table[i];
        if (p->state != PROC_BLOCKED)
            continue;
        if (p->wait_pid == dead_pid || p->wait_pid == -2) {
            p->wait_pid = -1;
            p->wake_tick = 0;
            p->state = PROC_READY;
        }
    }
}

static void process_finalize(struct process *p);


/* Non-blocking wait: returns status if DEAD, -2 if still live, -1 if error. */
int process_try_wait(int pid) {
    struct process *cur = process_current();
    if (!cur || pid <= 0)
        return -1;
    struct process *c = process_lookup_pid(pid);
    if (!c) {
        if (pid > 0 && pid < MAX_PROCESSES) {
            struct process *slot = &process_table[pid];
            if (slot->pid == pid && slot->state == PROC_DEAD) {
                if (slot->parent_pid != cur->pid)
                    return -1;
                int st = slot->exit_status;
                process_finalize(slot);
                return st;
            }
        }
        return -1;
    }
    if (c->parent_pid != cur->pid)
        return -1;
    if (c->state == PROC_DEAD) {
        int st = c->exit_status;
        process_finalize(c);
        return st;
    }
    return -2; /* still running */
}

int process_wait(int pid) {
    struct process *cur = process_current();
    if (!cur)
        return -1;

    for (;;) {
        if (pid > 0) {
            struct process *c = process_lookup_pid(pid);
            if (!c) {
                /* May already be DEAD — check table slot (pid == index) */
                if (pid > 0 && pid < MAX_PROCESSES) {
                    struct process *slot = &process_table[pid];
                    if (slot->pid == pid && slot->state == PROC_DEAD) {
                        if (slot->parent_pid != cur->pid)
                            return -1; /* not our child */
                        int st = slot->exit_status;
                        process_finalize(slot);
                        return st;
                    }
                }
                return -1;
            }
            /* Only the parent may wait on a specific child */
            if (c->parent_pid != cur->pid)
                return -1;
            if (c->state == PROC_DEAD) {
                int st = c->exit_status;
                process_finalize(c);
                return st;
            }
            cur->wait_pid = pid;
        } else {
            /* any child */
            int found_live = 0;
            for (int i = 1; i < MAX_PROCESSES; i++) {
                struct process *c = &process_table[i];
                if (c->parent_pid != cur->pid)
                    continue;
                if (c->state == PROC_DEAD) {
                    int st = c->exit_status;
                    process_finalize(c);
                    return st;
                }
                if (c->state != PROC_UNUSED)
                    found_live = 1;
            }
            if (!found_live)
                return -1;
            cur->wait_pid = -2;
        }

        /* Block until child exits (process_exit wakes us). */
        {
            unsigned long flags = irq_save();
            cur->state = PROC_BLOCKED;
            cur->wake_tick = 0; /* not timed */
            schedule();
            irq_restore(flags);
        }
    }
}

int process_create(const char *name, void (*entry)(void)) {
    struct process *p = NULL;
    int slot = -1;

    for (int i = 1; i < MAX_PROCESSES; i++) {
        if (process_table[i].state == PROC_UNUSED ||
            process_table[i].state == PROC_DEAD) {
            p = &process_table[i];
            slot = i;
            break;
        }
    }
    if (!p || slot < 0)
        return -1;

    /* Defensive: free any leftover stack from a previous DEAD occupant */
    if (p->stack_base) {
        kfree(p->stack_base);
        p->stack_base = NULL;
    }

    memset(p, 0, sizeof(*p));
    p->pid = slot;                    /* stable: pid == table index */
    p->state = PROC_READY;
    if (name)
        strncpy(p->name, name, PROCESS_NAME_LEN - 1);
    else
        strncpy(p->name, "proc", PROCESS_NAME_LEN - 1);
    p->name[PROCESS_NAME_LEN - 1] = '\0';

    p->stack_base = (uintptr_t *)kmalloc(PROCESS_STACK_SIZE);
    if (!p->stack_base) {
        memset(p, 0, sizeof(*p));
        return -1;
    }
    memset(p->stack_base, 0, PROCESS_STACK_SIZE);
    p->rsp = build_stack(entry, p->stack_base);
    p->kernel_stack_top = (uintptr_t)p->stack_base + PROCESS_STACK_SIZE;
    p->user_stack_base = NULL;
    p->is_user = 0;
    p->parent_pid = current ? current->pid : -1;
    p->wait_pid = -1;
    p->exit_status = 0;
    process_init_fds(p);
    p->time_slice = TIME_SLICE_TICKS;
    p->priority = PRIO_DEFAULT;
    p->ticks_ran = 0;
    p->on_cpu = -1;
    p->wake_tick = 0;
    if (current && current->cwd[0]) {
        strncpy(p->cwd, current->cwd, PATH_MAX - 1);
        p->cwd[PATH_MAX - 1] = '\0';
    } else {
        strncpy(p->cwd, "/", PATH_MAX - 1);
        p->cwd[PATH_MAX - 1] = '\0';
    }
    return p->pid;
}


static uintptr_t *build_user_stack_at(uintptr_t entry, uintptr_t usp,
                                       uintptr_t *kbase) {
    /* usp must already be correctly aligned by the caller (setup_user_argv
     * or plain top-of-stack).  Do not re-mask — that would slide past argc. */
    uintptr_t *sp = (uintptr_t *)((uint8_t *)kbase + PROCESS_STACK_SIZE);
    sp = (uintptr_t *)((uintptr_t)sp & ~0xFUL);

#if defined(__i386__)
    *--sp = (uintptr_t)USER_DS;
    *--sp = usp;
    *--sp = 0x202; /* IF + reserved bit 1 */
    *--sp = (uintptr_t)USER_CS;
    *--sp = entry;
    *--sp = (uintptr_t)user_trampoline;
    *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;
#else
    *--sp = (uintptr_t)USER_DS;
    *--sp = usp;
    *--sp = 0x202; /* IF + reserved bit 1 */
    *--sp = (uintptr_t)USER_CS;
    *--sp = entry;
    *--sp = (uintptr_t)user_trampoline;
    *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;
#endif
    return sp;
}

int process_create_user(const char *name, void (*user_entry)(void)) {
    struct process *p = NULL;
    int slot = -1;

    for (int i = 1; i < MAX_PROCESSES; i++) {
        if (process_table[i].state == PROC_UNUSED ||
            process_table[i].state == PROC_DEAD) {
            p = &process_table[i];
            slot = i;
            break;
        }
    }
    if (!p || slot < 0)
        return -1;

    /* Reclaim a DEAD slot fully (AS + stacks) before reuse */
    if (p->state == PROC_DEAD) {
        if (p->as) {
            aspace_destroy(p->as);
            p->as = NULL;
        }
    }
    if (p->stack_base) { kfree(p->stack_base); p->stack_base = NULL; }
    if (p->user_stack_base) { kfree(p->user_stack_base); p->user_stack_base = NULL; }

    memset(p, 0, sizeof(*p));
    p->pid = slot;
    p->state = PROC_READY;
    if (name)
        strncpy(p->name, name, PROCESS_NAME_LEN - 1);
    else
        strncpy(p->name, "user", PROCESS_NAME_LEN - 1);
    p->name[PROCESS_NAME_LEN - 1] = '\0';

    p->stack_base = (uintptr_t *)kmalloc(PROCESS_STACK_SIZE);
    p->user_stack_base = (uintptr_t *)kmalloc(PROCESS_STACK_SIZE);
    if (!p->stack_base || !p->user_stack_base) {
        if (p->stack_base) kfree(p->stack_base);
        if (p->user_stack_base) kfree(p->user_stack_base);
        memset(p, 0, sizeof(*p));
        return -1;
    }
    memset(p->stack_base, 0, PROCESS_STACK_SIZE);
    memset(p->user_stack_base, 0, PROCESS_STACK_SIZE);
    {
        uintptr_t usp = ((uintptr_t)p->user_stack_base + PROCESS_STACK_SIZE) & ~0xFUL;
        p->rsp = build_user_stack_at((uintptr_t)user_entry, usp, p->stack_base);
    }
    p->kernel_stack_top = (uintptr_t)p->stack_base + PROCESS_STACK_SIZE;
    p->is_user = 1;
    p->parent_pid = current ? current->pid : -1;
    p->wait_pid = -1;
    p->exit_status = 0;
    process_inherit_fds(p, current);
    p->time_slice = TIME_SLICE_TICKS;
    p->priority = PRIO_DEFAULT;
    p->ticks_ran = 0;
    p->on_cpu = -1;
    p->wake_tick = 0;
    if (current && current->cwd[0]) {
        strncpy(p->cwd, current->cwd, PATH_MAX - 1);
        p->cwd[PATH_MAX - 1] = '\0';
    } else {
        strncpy(p->cwd, "/", PATH_MAX - 1);
        p->cwd[PATH_MAX - 1] = '\0';
    }
    return p->pid;
}


int process_create_user_at(const char *name, uintptr_t entry, uintptr_t user_sp) {
    /* user_sp must sit in the user-accessible identity window */
    if (user_sp < 0x400000UL || user_sp >= 0x04000000UL) {
        kprintf("process: bad user_sp %p\n", (void *)user_sp);
        return -1;
    }
    if (entry < 0x400000UL || entry >= 0x04000000UL) {
        kprintf("process: bad user entry %p\n", (void *)entry);
        return -1;
    }

    struct process *p = NULL;
    int slot = -1;

    for (int i = 1; i < MAX_PROCESSES; i++) {
        if (process_table[i].state == PROC_UNUSED ||
            process_table[i].state == PROC_DEAD) {
            p = &process_table[i];
            slot = i;
            break;
        }
    }
    if (!p || slot < 0)
        return -1;

    if (p->stack_base) { kfree(p->stack_base); p->stack_base = NULL; }
    if (p->user_stack_base) { kfree(p->user_stack_base); p->user_stack_base = NULL; }

    memset(p, 0, sizeof(*p));
    p->pid = slot;
    p->state = PROC_READY;
    if (name)
        strncpy(p->name, name, PROCESS_NAME_LEN - 1);
    else
        strncpy(p->name, "user", PROCESS_NAME_LEN - 1);
    p->name[PROCESS_NAME_LEN - 1] = '\0';

    p->stack_base = (uintptr_t *)kmalloc(PROCESS_STACK_SIZE);
    if (!p->stack_base) {
        memset(p, 0, sizeof(*p));
        return -1;
    }
    memset(p->stack_base, 0, PROCESS_STACK_SIZE);
    p->user_stack_base = NULL; /* caller may attach later */
    p->rsp = build_user_stack_at(entry, user_sp, p->stack_base);
    p->kernel_stack_top = (uintptr_t)p->stack_base + PROCESS_STACK_SIZE;
    p->is_user = 1;
    p->parent_pid = current ? current->pid : -1;
    p->wait_pid = -1;
    p->exit_status = 0;
    process_inherit_fds(p, current);
    p->time_slice = TIME_SLICE_TICKS;
    p->priority = PRIO_DEFAULT;
    p->ticks_ran = 0;
    p->on_cpu = -1;
    p->wake_tick = 0;
    if (current && current->cwd[0]) {
        strncpy(p->cwd, current->cwd, PATH_MAX - 1);
        p->cwd[PATH_MAX - 1] = '\0';
    } else {
        strncpy(p->cwd, "/", PATH_MAX - 1);
        p->cwd[PATH_MAX - 1] = '\0';
    }
    return p->pid;
}

/* Layout (low → high at entry USP):
 *   [argc][argv0]...[NULL][env0]...[NULL][string bytes...]
 * Physical addresses written here; translated to user VAs by caller offset.
 */
static uintptr_t setup_user_argv(uintptr_t *ustack_base, char *const argv[],
                                 char *const envp[]) {
    uint8_t *stack_hi = (uint8_t *)((uintptr_t)ustack_base + PROCESS_STACK_SIZE);
    uint8_t *stack_lo = (uint8_t *)ustack_base;
    uint8_t *sp = stack_hi;
    int argc = 0, envc = 0;
    if (argv) {
        while (argv[argc])
            argc++;
    }
    if (envp) {
        while (envp[envc])
            envc++;
    }
    if (argc > 16) argc = 16;
    if (envc > 16) envc = 16;

    uintptr_t arg_ptrs[16];
    uintptr_t env_ptrs[16];

    /* env strings first (higher addresses), then argv strings */
    for (int i = envc - 1; i >= 0; i--) {
        size_t n = strlen(envp[i]) + 1;
        if (sp - n < stack_lo) {
            kprintf("spawn: env strings exceed user stack\n");
            return 0;
        }
        sp -= n;
        memcpy(sp, envp[i], n);
        /* user virtual address of this string */
        env_ptrs[i] = USER_STACK_BASE + ((uintptr_t)sp - (uintptr_t)stack_lo);
    }
    for (int i = argc - 1; i >= 0; i--) {
        size_t n = strlen(argv[i]) + 1;
        if (sp - n < stack_lo) {
            kprintf("spawn: argv strings exceed user stack\n");
            return 0;
        }
        sp -= n;
        memcpy(sp, argv[i], n);
        arg_ptrs[i] = USER_STACK_BASE + ((uintptr_t)sp - (uintptr_t)stack_lo);
    }

    /* argc + argv[] + NULL + envp[] + NULL */
    size_t ptr_bytes = (size_t)(1 + argc + 1 + envc + 1) * sizeof(uintptr_t);
    uintptr_t after_strings = (uintptr_t)sp;
    if (after_strings < ptr_bytes) {
        kprintf("spawn: argv/env frame exceeds user stack\n");
        return 0;
    }
    uintptr_t block_bottom = (after_strings - ptr_bytes) & ~0xFUL;
    if (block_bottom < (uintptr_t)stack_lo) {
        kprintf("spawn: argv/env frame below stack base\n");
        return 0;
    }
    sp = (uint8_t *)(block_bottom + ptr_bytes);

    /* envp NULL terminator */
    sp -= sizeof(uintptr_t);
    *(uintptr_t *)sp = 0;
    for (int i = envc - 1; i >= 0; i--) {
        sp -= sizeof(uintptr_t);
        *(uintptr_t *)sp = env_ptrs[i];
    }
    /* argv NULL terminator */
    sp -= sizeof(uintptr_t);
    *(uintptr_t *)sp = 0;
    for (int i = argc - 1; i >= 0; i--) {
        sp -= sizeof(uintptr_t);
        *(uintptr_t *)sp = arg_ptrs[i];
    }
    sp -= sizeof(uintptr_t);
    *(uintptr_t *)sp = (uintptr_t)argc;

    return (uintptr_t)sp;
}


int spawn(const char *path, char *const argv[], char *const envp[]) {
    /* Interim guard: refuse if no free process slot BEFORE loading ELF.
     * Per-process AS (1.0.75+): refuse when table full before loading. */
    int have_slot = 0;
    for (int i = 1; i < MAX_PROCESSES; i++) {
        if (process_table[i].state == PROC_UNUSED ||
            process_table[i].state == PROC_DEAD) {
            have_slot = 1;
            break;
        }
    }
    if (!have_slot)
        return -1;

    struct address_space *as = aspace_create();
    if (!as) {
        kprintf("spawn: aspace_create failed\n");
        return -1;
    }

    /* Mapped user stack in the process AS (guard page below USER_STACK_BASE) */
    void *stack_phys = 0;
    uintptr_t vtop = aspace_map_user_stack(as, &stack_phys);
    if (!vtop || !stack_phys) {
        kprintf("spawn: user stack map failed\n");
        aspace_destroy(as);
        return -1;
    }
    uintptr_t usp_phys = setup_user_argv((uintptr_t *)stack_phys, argv, envp);
    if (usp_phys == 0) {
        kprintf("spawn: setup_user_argv failed\n");
        aspace_destroy(as);
        return -1;
    }
    /* Translate physical SP → virtual SP in the fixed stack window */
    uintptr_t usp = USER_STACK_BASE + (usp_phys - (uintptr_t)stack_phys);

    struct elf_load_info info;
    if (elf_load_file(path, &info, as) != 0) {
        /* Prefer disk; fall back to kernel-embedded image for shell/hello. */
        int used_embed = 0;
        if (path) {
            const char *base = path;
            for (const char *q = path; *q; q++)
                if (*q == '/') base = q + 1;
            for (unsigned i = 0; i < embedded_elves_count; i++) {
                const struct embedded_elf *e = &embedded_elves[i];
                if (!e->name || !e->data || e->len == 0)
                    continue;
                if (strcmp(base, e->name) != 0)
                    continue;
                if (elf_load_image(e->data, e->len, &info, as) == 0) {
                    kprintf("spawn: loaded '%s' from kernel embed\n", e->name);
                    used_embed = 1;
                    break;
                }
            }
        }
        if (!used_embed) {
            /* Silent when file simply missing (PATH probes many names). */
            aspace_destroy(as);
            return -1;
        }
    }

    const char *base = path;
    for (const char *q = path; *q; q++)
        if (*q == '/')
            base = q + 1;

    int pid = process_create_user_at(base, info.entry, usp);
    if (pid < 0) {
        kprintf("spawn: process_create_user_at failed entry=%p sp=%p\n",
                (void *)info.entry, (void *)usp);
        aspace_destroy(as);
        return -1;
    }
    /* Stack frames owned by aspace; no separate heap user_stack_base */
    process_table[pid].user_stack_base = NULL;
    process_table[pid].as = as;
    process_table[pid].heap_brk = 0; /* SYS_BRK starts at USER_HEAP_BASE on first use */
    process_table[pid].heap_limit = USER_HEAP_LIMIT;
    return pid;
}

/*
 * Fully reclaim DEAD processes that are no longer running.
 * - Free the stack
 * - Return the slot to PROC_UNUSED so create can reuse it cleanly
 * Never touch the process we are currently executing on.
 */
/* Free stacks and mark UNUSED. Caller must hold the only reference (wait or orphan). */
static void process_finalize(struct process *p) {
    if (!p)
        return;
    if (p->stack_base) {
        kfree(p->stack_base);
        p->stack_base = NULL;
    }
    if (p->user_stack_base) {
        kfree(p->user_stack_base);
        p->user_stack_base = NULL;
    }
    if (p->as) {
        aspace_destroy(p->as);
        p->as = NULL;
        p->heap_brk = 0;
        p->heap_limit = USER_HEAP_LIMIT;
    }
    p->rsp = NULL;
    p->wake_tick = 0;
    p->time_slice = 0;
    p->wait_pid = -1;
    p->parent_pid = -1;
    p->name[0] = '\0';
    p->state = PROC_UNUSED;
}

/*
 * Auto-reap only orphans: if a parent still exists (not UNUSED), leave DEAD
 * so process_wait can observe exit_status. Prevents wait/reap race.
 *
 * Shell respawn: rate-limited (at most one successful respawn per ~2 s) and
 * the new shell always gets a clean console fd set via process_init_fds
 * (spawn already does this for a fresh process).
 */
static uint32_t last_shell_respawn_tick = 0;
static int      shell_respawn_fail_streak = 0;
static int      system_halt_requested = 0;

void process_request_halt(void) {
    system_halt_requested = 1;
}

int process_halt_requested(void) {
    return system_halt_requested;
}

static void process_reap(void) {
    for (int i = 1; i < MAX_PROCESSES; i++) {
        struct process *p = &process_table[i];
        if (p->state != PROC_DEAD || p == current)
            continue;
        int orphan = 1;
        if (p->parent_pid > 0 && p->parent_pid < MAX_PROCESSES) {
            struct process *par = &process_table[p->parent_pid];
            /* Living parent may still wait(); DEAD/UNUSED cannot — treat as orphan. */
            if (par->pid == p->parent_pid &&
                (par->state == PROC_READY || par->state == PROC_RUNNING ||
                 par->state == PROC_BLOCKED))
                orphan = 0;
        }
        if (orphan) {
            int was_shell = (strcmp(p->name, "shell") == 0 ||
                             strcmp(p->name, "shell.elf") == 0);
            process_finalize(p);
            /* Shell exit used to leave the system with only idle — respawn. */
            if (was_shell && !system_halt_requested) {
                uint32_t now = timer_get_ticks();
                /* ~2 s at 100 Hz; also stop after repeated failures */
                if (shell_respawn_fail_streak >= 3) {
                    kprintf("shell respawn disabled (too many failures)\n");
                } else if (last_shell_respawn_tick != 0 &&
                           (int32_t)(now - last_shell_respawn_tick) < 200) {
                    /* rate-limit: skip this cycle */
                } else {
                    char *argv[] = { "shell.elf", NULL };
                    int np = spawn("shell.elf", argv, NULL);
                    if (np > 0) {
                        /* spawn() already calls process_init_fds → clean console */
                        kprintf("shell respawned (pid %d)\n", np);
                        last_shell_respawn_tick = now;
                        shell_respawn_fail_streak = 0;
                    } else {
                        kprintf("shell respawn failed\n");
                        shell_respawn_fail_streak++;
                    }
                }
            }
        }
    }
}


void process_exit(int status) {
    if (!current || current->pid == 0)
        return;

    /*
     * Do NOT free the stack here — we are still running on it.
     * Mark DEAD, drop from every wait queue, then schedule away.
     * process_reap() finishes the cleanup after the switch.
     */
    wait_queue_remove_pid(current->pid);
    /* Drop pipe refs so the other end sees EOF / EPIPE */
    for (int i = 0; i < MAX_FDS; i++) {
        fd_close(current, i);
    }
    /* Reparent children so they are not stuck under a dead parent_pid */
    {
        int new_parent = -1;
        for (int i = 1; i < MAX_PROCESSES; i++) {
            struct process *c = &process_table[i];
            if (c->state != PROC_UNUSED && c->state != PROC_DEAD &&
                (strcmp(c->name, "shell") == 0 || strcmp(c->name, "shell.elf") == 0) &&
                c != current) {
                new_parent = c->pid;
                break;
            }
        }
        for (int i = 1; i < MAX_PROCESSES; i++) {
            struct process *c = &process_table[i];
            if (c->parent_pid == current->pid)
                c->parent_pid = new_parent;
        }
    }
    /* Keep 0..255 so wait()-failure (-1) is unambiguous in the shell */
    current->exit_status = status & 0xff;
    current->state = PROC_DEAD;
    current->wake_tick = 0;
    process_wake_waiters(current->pid);

    unsigned long __eflags = irq_save();
    schedule();
    irq_restore(__eflags);
    while (1)
        __asm__ volatile("hlt");
}

/*
 * Block current process until something marks it READY, or until
 * wake_tick (if nonzero) has passed.
 *
 * MUST be called with IRQs disabled.  Returns with IRQs still disabled (caller sti).
 *
 * The caller is responsible for having performed any condition check
 * atomically with this call (i.e., with IF=0), otherwise a wakeup can
 * race between the check and the enqueue (lost wakeup).
 */

/* ---- Foreground job (Ctrl-C / SIGINT) ---- */
#define MAX_FG 8
static int fg_list[MAX_FG];
static int fg_n;

void process_fg_clear(void) {
    fg_n = 0;
}

int process_fg_add(int pid) {
    if (pid <= 0 || fg_n >= MAX_FG)
        return -1;
    /* avoid duplicates */
    for (int i = 0; i < fg_n; i++)
        if (fg_list[i] == pid)
            return 0;
    fg_list[fg_n++] = pid;
    return 0;
}

int process_kill(int pid, int status); /* fwd */

void process_fg_remove(int pid) {
    int w = 0;
    for (int i = 0; i < fg_n; i++) {
        if (fg_list[i] != pid)
            fg_list[w++] = fg_list[i];
    }
    fg_n = w;
}

void process_fg_interrupt(void) {

    /* Copy list: process_kill may re-enter / change state */
    int local[MAX_FG];
    int n = fg_n;
    for (int i = 0; i < n; i++)
        local[i] = fg_list[i];
    fg_n = 0;
    for (int i = 0; i < n; i++)
        process_kill(local[i], 128 + 2); /* 130 = 128+SIGINT */
}

/* Force-exit a user process (e.g. kill builtin). Returns 0 or -1.
 *
 * Safety:
 *  - reject pid <= 0 (idle) and out-of-range
 *  - reject non-user / already dead / unused
 *  - close all FDs (pipe refcounts drop → EOF/EPIPE on other end)
 *  - remove from wait queues
 *  - reparent any children to pid 1 (or mark orphan if no suitable parent)
 *    so they do not stay stuck with a dead parent_pid
 */
int process_kill(int pid, int status) {
    if (pid <= 0 || pid >= MAX_PROCESSES)
        return -1;
    struct process *p = &process_table[pid];
    if (p->state == PROC_UNUSED || p->state == PROC_DEAD)
        return -1;
    if (!p->is_user)
        return -1; /* never kill kernel/idle */
    if (p == current) {
        process_exit(status);
        return 0; /* not reached */
    }

    wait_queue_remove_pid(p->pid);

    for (int i = 0; i < MAX_FDS; i++)
        fd_close(p, i);

    /* Reparent children so they become orphans (auto-reaped) or
     * attach to a still-living shell if one exists. */
    int new_parent = -1;
    for (int i = 1; i < MAX_PROCESSES; i++) {
        struct process *c = &process_table[i];
        if (c->state != PROC_UNUSED && c->state != PROC_DEAD &&
            (strcmp(c->name, "shell") == 0 || strcmp(c->name, "shell.elf") == 0)) {
            new_parent = c->pid;
            break;
        }
    }
    for (int i = 1; i < MAX_PROCESSES; i++) {
        struct process *c = &process_table[i];
        if (c->parent_pid == pid)
            c->parent_pid = new_parent;
    }

    process_fg_remove(pid);
    /* exit_status is always 0..255 (128+sig for signals). */
    p->exit_status = status & 0xff;
    p->state = PROC_DEAD;
    p->wake_tick = 0;
    process_wake_waiters(p->pid);
    return 0;
}



void process_block_current(uint32_t wake_tick) {
    /* Requires IF=0 on entry. Returns with IF still 0 (caller restores). */
    /* Idle (pid 0) must never block — it is the always-runnable fallback. */
    if (!current || current->pid == 0)
        return;

    current->state     = PROC_BLOCKED;
    current->wake_tick = wake_tick;

    schedule();   /* does not return until someone marks us READY */

    /* Resumed: RUNNING again. Clear timeout, full quantum.
     * Still IF=0 here — timer_sleep_ms (and other callers) must sti. */
    current->wake_tick  = 0;
    current->time_slice = TIME_SLICE_TICKS;
}

/* Called from timer IRQ (IF=0, single CPU): wake timed-out blocked procs. */
void process_wake_timed(void) {
    uint32_t now = timer_get_ticks();
    int woke = 0;
    for (int i = 1; i < MAX_PROCESSES; i++) {
        struct process *p = &process_table[i];
        if (p->state == PROC_BLOCKED && p->wake_tick != 0 &&
            (int32_t)(now - p->wake_tick) >= 0) {
            p->wake_tick = 0;
            p->state = PROC_READY;
            woke = 1;
        }
    }
    /* Preempt current task so a just-woken sleeper is not delayed up to a
       full time slice (otherwise short timer_sleep_ms tests flake). */
    if (woke && current)
        current->time_slice = 0;
}

void process_yield(void) {
    if (scheduler_enabled) {
        unsigned long flags = irq_save();
        schedule();
        irq_restore(flags);
    }
}

struct process *process_current(void) { return current; }

/*
 * O(1) lookup. pid is the table index by construction.
 * Rejects UNUSED and DEAD so a recycled slot cannot be woken by a stale pid.
 */
struct process *process_lookup_pid(int pid) {
    if (pid < 0 || pid >= MAX_PROCESSES)
        return NULL;
    struct process *p = &process_table[pid];
    if (p->pid != pid)
        return NULL;   /* should never happen with stable-pid design */
    if (p->state == PROC_UNUSED || p->state == PROC_DEAD)
        return NULL;
    return p;
}

int process_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i].state != PROC_UNUSED &&
            process_table[i].state != PROC_DEAD)
            n++;
    }
    return n;
}

/* Snapshot live + zombie (DEAD, not yet reaped) processes for ps/top. */
int process_snapshot(void *out_procs, int max) {
    if (!out_procs || max <= 0)
        return 0;
    /* Layout matches struct sys_proc_info in syscall.h */
    struct {
        int32_t pid, ppid, state, is_user, exit_status;
        char name[32];
    } *out = out_procs;
    int n = 0;
    for (int i = 0; i < MAX_PROCESSES && n < max; i++) {
        struct process *p = &process_table[i];
        if (p->state == PROC_UNUSED)
            continue;
        out[n].pid = p->pid;
        out[n].ppid = p->parent_pid;
        out[n].state = (int32_t)p->state;
        out[n].is_user = p->is_user;
        out[n].exit_status = p->exit_status;
        int j;
        for (j = 0; j < 31 && p->name[j]; j++)
            out[n].name[j] = p->name[j];
        out[n].name[j] = 0;
        n++;
    }
    return n;
}

void process_list(void) {
    kprintf("\nPID  STATE     NAME\n");
    for (int i = 0; i < MAX_PROCESSES; i++) {
        struct process *p = &process_table[i];
        if (p->state == PROC_UNUSED)
            continue;
        const char *st = "?";
        if      (p->state == PROC_READY)   st = "READY";
        else if (p->state == PROC_RUNNING) st = "RUNNING";
        else if (p->state == PROC_DEAD)    st = "DEAD";
        else if (p->state == PROC_BLOCKED) st = "BLOCKED";
        kprintf("%3d  %-8s  ring%d  %s%s\n", p->pid, st,
                p->is_user ? 3 : 0, p->name,
                p == current ? " *" : "");
    }
    kprintf("\n");
}

/* Pick highest-priority READY task; RR within the same priority.
 * Caller must hold sched_lock (or IF=0 on uniprocessor). */
static struct process *pick_next(void) {
    for (int prio = PRIO_MAX; prio >= PRIO_MIN; prio--) {
        int start = rr_cursor[prio];
        for (int n = 0; n < MAX_PROCESSES; n++) {
            int i = (start + n) % MAX_PROCESSES;
            struct process *p = &process_table[i];
            if (p->state == PROC_READY && p->priority == prio) {
                rr_cursor[prio] = (i + 1) % MAX_PROCESSES;
                return p;
            }
        }
    }
    /* Fallback: idle (pid 0). Force READY if somehow invalid. */
    if (process_table[0].state != PROC_READY &&
        process_table[0].state != PROC_RUNNING)
        process_table[0].state = PROC_READY;
    process_table[0].priority = PRIO_IDLE;
    return &process_table[0];
}

/* schedule() requires IF=0 (caller cli or sched_lock). Returns with IF still 0.
 * Idle (pid 0) is never blocked; pick_next falls back to it if nothing READY.
 *
 * Current must be marked READY *before* pick_next so a sole high-prio task
 * can be selected again (otherwise it is invisible while still RUNNING and
 * always loses the CPU to any lower-prio READY task on every yield/preempt). */
void schedule(void) {
    if (!scheduler_enabled || !current)
        return;
    process_reap();
    /* Demote first so pick_next can re-select us at our priority. */
    if (current->state == PROC_RUNNING)
        current->state = PROC_READY;
    struct process *next = pick_next();
    if (next == current) {
        current->state = PROC_RUNNING;
        current->time_slice = TIME_SLICE_TICKS;
        return;
    }
    struct process *prev = current;
    /* prev is already READY (or was BLOCKED/DEAD — leave those alone). */
    next->state = PROC_RUNNING;
    next->time_slice = TIME_SLICE_TICKS;
    next->on_cpu = smp_cpu_id();
    {
        struct cpu_data *cd = cpu_this();
        cd->cur = next;
        cd->switches++;
    }
    if (next->as)
        aspace_switch(next->as);
    else
        aspace_switch(aspace_kernel());
    /* Privilege-change IRQs need a valid ring-0 stack in the TSS */
    tss_set_kernel_stack(next->kernel_stack_top ? next->kernel_stack_top : 0x90000);
    context_switch(&prev->rsp, next->rsp);
    /* Now running as next; safe to free any DEAD stacks (including prev) */
    process_reap();
}

void scheduler_tick(void) {
    if (!scheduler_enabled || !current)
        return;
    current->ticks_ran++;
    cpu_this()->ticks++;
    if (smp_need_resched()) {
        smp_clear_need_resched();
        schedule();
        return;
    }
    if (--current->time_slice <= 0) {
        /* Called from timer IRQ with interrupts already disabled */
        schedule();
    }
}

void scheduler_start(void) {
    scheduler_enabled = 1;
    if (current)
        current->state = PROC_RUNNING;
    kprintf("Scheduler started (preemptive, blocking)\n");
}

int scheduler_is_enabled(void) { return scheduler_enabled; }

int process_set_priority(int pid, int prio) {
    if (prio < PRIO_MIN || prio > PRIO_MAX)
        return -1;
    if (pid == 0)
        return -1; /* idle stays PRIO_IDLE */
    struct process *p = process_lookup_pid(pid);
    if (!p)
        return -1;
    unsigned long flags = irq_save();
    p->priority = prio;
    irq_restore(flags);
    return 0;
}

int process_get_priority(int pid) {
    struct process *p = process_lookup_pid(pid);
    if (!p)
        return -1;
    return p->priority;
}

void process_reset_ticks(void) {
    unsigned long flags = irq_save();
    for (int i = 0; i < MAX_PROCESSES; i++)
        process_table[i].ticks_ran = 0;
    cpus[0].ticks = 0;
    irq_restore(flags);
}
