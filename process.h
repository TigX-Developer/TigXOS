

#ifndef PROCESS_H
#define PROCESS_H
#include <stdint.h>
#include <stddef.h>
#include "fs.h"
#include "filetable.h"
struct address_space;

#define MAX_PROCESSES 32

#define NCPU_MAX      16   /* SMP-ready layout; only CPU 0 online until 1.2.8 */
#define PRIO_MIN       0
#define PRIO_MAX       3
#define PRIO_DEFAULT   1
#define PRIO_IDLE      0   /* idle task only */
#define PROCESS_STACK_SIZE (16*1024)
#define PROCESS_NAME_LEN 32
#define MAX_FDS 16
#define PATH_MAX 64

enum process_state {
    PROC_UNUSED = 0,
    PROC_READY,
    PROC_RUNNING,
    PROC_BLOCKED,
    PROC_DEAD
};

/* Per-process FD slot: points at a shared open_file (filetable.h). */
struct file_desc {
    int of_id;                     /* -1 = free; else index into open-file table */
};

/* Compatibility aliases for type checks via of_get()->type */
enum fd_type {
    FD_NONE = 0,
    FD_CONSOLE = 1,
    FD_FILE = 2,
    FD_PIPE = 3
};

#define MAX_PROC_GROUPS 16

struct proc_group {
    int used;
    int tgid;
    int refcount;
    struct address_space *as;
    struct file_desc fds[MAX_FDS];
    char cwd[PATH_MAX];
    uintptr_t heap_brk;
    uintptr_t heap_limit;
};

/*
 * 1.3.8+ process/thread model (true allocation split):
 *   - struct proc_group  = *process* (AS, fds, cwd, heap, tgid) — one per leader
 *   - struct process     = *thread*  (stack, rsp, state, prio, TLS, on_cpu)
 * process_current() is the current *thread*. Shared state goes through
 * process_group_of() / process_fds() / process_aspace(). Legacy mirror
 * fields on struct process stay for compat but are synced from the group.
 */
struct process {
    int pid;             /* [thread] tid; for leaders also tgid */
    int tid;             /* explicit thread id (=pid for main) */
    enum process_state state;
    char name[PROCESS_NAME_LEN];
    uintptr_t *stack_base;
    uintptr_t *rsp;
    uintptr_t kernel_stack_top;
    uintptr_t *user_stack_base;
    int is_user;
    int time_slice;
    int priority;          /* PRIO_MIN..PRIO_MAX; higher runs first */
    uint32_t wake_tick;
    uint32_t ticks_ran;     /* accounting for fairness tests */
    int on_cpu;            /* cpu id that last ran this task, or -1 */
    int parent_pid;
    int wait_pid;
    int exit_status;
    /* 1.2.10 kernel threads */
    int is_kthread;       /* 1 = pure kernel thread */
    int joinable;         /* 1 until joined or detached */
    int joiner_pid;       /* pid blocked in kthread_join, or -1 */
    void *kthread_arg;    /* arg for entry(void *) trampoline */
    void (*kthread_fn)(void *);
    /* 1.2.11 user threads */
    int tgid;             /* thread-group id (= leader pid); self for leaders */
    int is_thread;        /* 1 = secondary user thread in a group */
    int thread_slot;      /* 0 = main stack; 1..N = aspace_map_thread_stack */
    uintptr_t tls_base;   /* user TLS base (FS on x86_64, GS on i386) */
    int exit_thread_only; /* 1 = SYS_THREAD_EXIT; do not kill the group */
    uint32_t thr_gen;     /* generation; join captures this to avoid reuse races */
    struct file_desc fds[MAX_FDS];
    struct proc_group *group; /* shared process group (AS/fds) */
    struct address_space *as; /* mirror of group->as for compat */
    uintptr_t heap_brk;   /* user heap break (SYS_BRK); 0 = not started */
    uintptr_t heap_limit; /* max bytes above USER_HEAP_BASE (0 = USER_HEAP_LIMIT) */
    char cwd[PATH_MAX];   /* current working directory (absolute path string) */
};

void process_init(void);
int  process_prepare_ap_idle(int cpu_id); /* reserve idle slot for AP */
void process_bind_ap(int cpu_id);         /* AP: set current + online */

int  process_create(const char *name, void (*entry)(void));
int  process_create_user(const char *name, void (*user_entry)(void));
int  process_create_user_at(const char *name, uintptr_t entry, uintptr_t user_sp);
/* 1.2.11: new user thread in the same address space as the caller.
 * user_stack: 0 → kernel maps a guarded stack; else must be a valid user SP.
 * Returns tid or -1. */
int  process_thread_create(void (*entry)(void), uintptr_t user_stack);
void process_thread_exit(int status); /* exit only this thread */
int  process_thread_join(int tid, int *status);
void process_load_tls(struct process *p); /* apply TLS on context switch */
int  process_group_kill(int tgid, int status); /* kill all threads in group */

int  spawn(const char *path, char *const argv[], char *const envp[]);

void process_exit(int status);
void process_reap_pid(int pid); /* finalize a DEAD slot (kthread join) */
int  process_kill(int pid, int status);

/* Foreground job for Ctrl-C (SIGINT). */
void process_fg_clear(void);
int  process_fg_add(int pid);
void process_fg_interrupt(void); /* kill all fg with SIGINT status */
void process_fg_remove(int pid);

/* Stop shell auto-respawn; pair with arch halt / hlt loop. */
void process_request_halt(void);
int  process_halt_requested(void);

void process_yield(void);
void schedule(void);
void scheduler_tick(void);
void scheduler_start(void);
int  scheduler_is_enabled(void);
int  process_set_priority(int pid, int prio);
int  process_get_priority(int pid);
void process_reset_ticks(void); /* zero ticks_ran for all */
/* SMP affinity: pin READY/RUNNING task to a logical CPU (-1 = any). */
int  process_set_affinity(int pid, int cpu);
int  process_get_affinity(int pid);
int  process_pick_cpu(void); /* least-loaded online CPU */
/* Per-CPU (1.2.3): layout for SMP; only CPU 0 is online today. */
struct cpu_data {
    int             id;
    int             online;
    struct process *cur;     /* current process on this CPU */
    uint64_t        switches;
    uint64_t        ticks;
};
struct cpu_data *cpu_this(void);
int              cpu_count_online(void);
struct process *process_current(void);
struct process *process_lookup_pid(int pid);
int  process_count(void);
void process_list(void);
/* Fill user-facing process table; returns number of entries. */
int  process_snapshot(void *out_procs, int max);
void process_wake_timed(void);
void process_block_current(uint32_t wake_tick);
int  scheduler_is_enabled(void);

int  process_wait(int pid);
int  process_try_wait(int pid); /* -2 if still live */

void process_init_fds(struct process *p);
void process_inherit_fds(struct process *child, struct process *parent);
int  process_alloc_fd(struct process *p);
void process_set_parent(struct process *child, int parent_pid);

/* Pipe refcount helpers (syscall layer) */
void fd_close(struct process *p, int fd);
int  fd_type_of(struct process *p, int fd);

struct proc_group *process_group_of(struct process *t);
struct proc_group *process_group_create(int tgid);
void process_group_ref(struct proc_group *g);
void process_group_unref(struct proc_group *g);
void process_group_bind_leader(struct process *p, struct address_space *as);
void process_group_bind_thread(struct process *leader, struct process *thr);
struct file_desc *process_fds(struct process *t);
struct address_space *process_aspace(struct process *t);
char *process_cwd(struct process *t);

#endif

