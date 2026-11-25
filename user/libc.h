/* user/libc.h — minimal libc for ring-3 (syscall ABI v1, TigxOS 1.3.8+) */
#ifndef USER_LIBC_H
#define USER_LIBC_H

#include <stddef.h>
#include <stdint.h>

/* Match kernel syscall.h — stable ABI v1 (1.2.4+) */
#define SYS_EXIT         0
#define SYS_WRITE        1
#define SYS_YIELD        2
#define SYS_GETPID       3
#define SYS_SLEEP_MS     4
#define SYS_READ         5
#define SYS_OPEN         6
#define SYS_CLOSE        7
#define SYS_GETDENTS     8
#define SYS_SPAWN        9
#define SYS_WAIT        10
#define SYS_UNLINK      11
#define SYS_STAT        12
#define SYS_DUP         13
#define SYS_DUP2        14
#define SYS_PIPE        15
#define SYS_KILL        16
#define SYS_SETFG       17
#define SYS_BRK         18
#define SYS_CHDIR       19
#define SYS_GETCWD      20
#define SYS_SYSINFO     21
#define SYS_PSINFO      22
#define SYS_HALT        23
#define SYS_MKDIR       24
#define SYS_ABI_VERSION 25
#define SYS_DMESG       26
#define SYS_POWEROFF    27
#define SYS_REBOOT      28
#define SIGINT   2
#define SIGKILL  9
#define SIGTERM 15

#define O_RDONLY        0
#define O_WRONLY        1
#define O_RDWR          2
#define O_CREAT         0x100
#define O_TRUNC         0x200
#define O_APPEND        0x400

#define STDIN_FILENO    0
#define STDOUT_FILENO   1
#define STDERR_FILENO   2

struct dirent {
    char     name[28];
    uint32_t size;
} __attribute__((packed));

struct sys_sysinfo {
    uint32_t uptime_sec;
    uint32_t ticks;
    uint32_t hz;
    uint32_t mem_total_kb;
    uint32_t mem_free_kb;
    uint32_t fs_files;
    uint32_t fs_blocks;
    uint32_t fs_free_blocks;
    uint32_t rtc_year, rtc_mon, rtc_day;
    uint32_t rtc_hour, rtc_min, rtc_sec;
    char     arch[16];
    char     version[24];
};

struct stat {
    uint32_t size;
    uint32_t mode;
};

/* raw syscall */
long syscall(long n, long a0, long a1, long a2);

void _exit(int code);
void exit(int code);
long write(int fd, const void *buf, size_t n);
long read(int fd, void *buf, size_t n);
int  open(const char *path, int flags);
int  chdir(const char *path);
int  getcwd(char *buf, size_t size);
int  sysinfo(struct sys_sysinfo *info);
long dmesg(char *buf, size_t len, size_t off);
void poweroff(void);
void reboot(void);

/* Process table snapshot (SYS_PSINFO) */
struct sys_proc_info {
    int  pid;
    int  ppid;
    int  state;       /* 0 unused 1 ready 2 running 3 blocked 4 dead */
    int  is_user;
    int  exit_status;
    int  tgid;
    int  tid;
    int  is_thread;
    char name[32];
};
#define SYS_PSINFO_MAX 16
struct sys_psinfo {
    int count;
    struct sys_proc_info procs[SYS_PSINFO_MAX];
};
int  psinfo(struct sys_psinfo *info);  /* returns count or -1 */
void halt(void);                      /* SYS_HALT — stop the machine */
int  close(int fd);
int  dup(int oldfd);
int  dup2(int oldfd, int newfd);
int  pipe(int fd[2]);
int  kill(int pid);             /* SIGKILL */
int  kill_sig(int pid, int sig);
int  setfg(int pid);            /* 0 = clear fg job */
int  getdents(void *buf, size_t n);
int  unlink(const char *path);
int  mkdir(const char *path);
int  stat(const char *path, struct stat *st);
int  spawn(const char *path, char *const argv[]); /* env from shell via spawn_env */
int  spawn_env(const char *path, char *const argv[], char *const envp[]);
extern char **environ;
char *getenv(const char *name);
int  wait(int pid);
int  wait_noblock(int pid); /* -2 if still running */
int  getpid(void);
void yield(void);
void sleep_ms(unsigned ms);

size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
char  *strcpy(char *d, const char *s);
void  *memset(void *d, int c, size_t n);
void  *memcpy(void *d, const void *s, size_t n);
int    printf(const char *fmt, ...);

void *malloc(size_t n);
void  free(void *p);
void *realloc(void *p, size_t n);
void *sbrk(long incr);
const char *str_concat(const char *a, const char *b);
int    putchar(int c);
int    puts(const char *s);


/* bounds-safe index helpers used by glc codegen */
long        str_index(const char *s, int i);   /* s[i] as unsigned char, or 0 if OOB/NULL */
const char *argv_get(char **argv, int argc, int i); /* argv[i], or "" if OOB/NULL */

/* helpers for green-lizard tools */
long readch(int fd);              /* one byte, or -1 on EOF/error */
long writech(int fd, int c);      /* write one byte */
void listdir(void);               /* print "name  size" lines for root */

#endif

/* ---- 1.2.12 threads + sync (pthread-like) ---- */
#define SYS_CLONE       29
#define SYS_THREAD_EXIT 30
#define SYS_THREAD_JOIN 31
#define SYS_SET_TLS     32
#define SYS_GETTID      33
#define SYS_FUTEX       34
#define FUTEX_WAIT      0
#define FUTEX_WAKE      1

extern int errno;

int  gettid(void);
int  thread_create(void (*entry)(void), void *stack_top); /* stack 0 = kernel maps */
int  thread_join(int tid, int *status);
void thread_exit(int status);
int  futex(volatile int *uaddr, int op, int val);

typedef struct { volatile int lock; } mutex_t;
void mutex_init(mutex_t *m);
void mutex_lock(mutex_t *m);
void mutex_unlock(mutex_t *m);
int  mutex_trylock(mutex_t *m);

typedef struct {
    volatile int seq;
    mutex_t     *m;
} cond_t;
void cond_init(cond_t *c);
int  cond_wait(cond_t *c, mutex_t *m);
int  cond_signal(cond_t *c);
int  cond_broadcast(cond_t *c);
/* 1.3.8 Green Lizard thread bindings */
int gl_thread_spawn(void (*entry)(void), void *stack_top);
int gl_thread_join(int tid, int *status);
void gl_thread_exit(int status);
int gl_thread_self(void);

