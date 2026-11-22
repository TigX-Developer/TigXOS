

#ifndef PROCESS_H
#define PROCESS_H
#include <stdint.h>
#include <stddef.h>
#include "fs.h"
#include "filetable.h"
struct address_space;

#define MAX_PROCESSES 16
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

struct process {
    int pid;
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
    struct file_desc fds[MAX_FDS];
    struct address_space *as;
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
int  spawn(const char *path, char *const argv[], char *const envp[]);

void process_exit(int status);
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

#endif
