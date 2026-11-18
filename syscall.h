

/* syscall.h — int 0x80 ring-3 syscall interface (1.1.12) */
#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

#define SYSCALL_VECTOR  0x80

/* Numbers (rax / eax) — stable ABI for userland / future language */
#define SYS_EXIT        0
#define SYS_WRITE       1   /* fd, buf, len → bytes or -1 */
#define SYS_YIELD       2
#define SYS_GETPID      3
#define SYS_SLEEP_MS    4   /* ms → 0 */
#define SYS_READ        5   /* fd, buf, len → bytes or -1 */
#define SYS_OPEN        6   /* path, flags → fd or -1 */
#define SYS_CLOSE       7   /* fd → 0 or -1 */
#define SYS_GETDENTS    8   /* buf, len → bytes or -1 (root listing) */
#define SYS_SPAWN       9   /* path, argv → pid or -1 (argv may be 0) */
#define SYS_WAIT       10   /* pid → status or -1 */
#define SYS_UNLINK     11   /* path → 0 or -1 */
#define SYS_STAT       12   /* path, stat_buf → 0 or -1 */
#define SYS_DUP        13   /* oldfd → newfd or -1 */
#define SYS_DUP2       14
#define SYS_PIPE       15
#define SYS_KILL       16   /* pid, sig → 0 or -1 */
#define SYS_SETFG      17
#define SYS_BRK        18   /* new_brk → previous brk or -1 */
#define SYS_CHDIR      19   /* path → 0 or -1 */
#define SYS_GETCWD     20   /* buf, size → 0 or -1 */
#define SYS_SYSINFO    21   /* struct sys_sysinfo * → 0 or -1 */
#define SYS_PSINFO     22
#define SYS_MKDIR      24
#define SYS_HALT       23   /* power off / hang cleanly */

/* open flags */
#define O_RDONLY        0
#define O_WRONLY        1
#define O_RDWR          2
#define O_CREAT         0x100
#define O_TRUNC         0x200
#define O_APPEND        0x400

/* Standard FDs */
#define STDIN_FILENO    0
#define STDOUT_FILENO   1
#define STDERR_FILENO   2

/* Dirent returned by SYS_GETDENTS (packed, fixed size) */
struct sys_dirent {
    char     name[28];
    uint32_t size;
} __attribute__((packed));

/* Result of SYS_STAT */
struct sys_stat {
    uint32_t size;   /* file size in bytes */
    uint32_t mode;   /* 1 = regular file (for now) */
};

/* Result of SYS_SYSINFO (must match struct sys_sysinfo in user/libc.h) */
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

/* One process row for SYS_PSINFO (must match user/libc.h) */
struct sys_proc_info {
    int32_t  pid;
    int32_t  ppid;
    int32_t  state;      /* enum process_state */
    int32_t  is_user;
    int32_t  exit_status;
    char     name[32];
};

/* Table returned by SYS_PSINFO */
#define SYS_PSINFO_MAX 16
struct sys_psinfo {
    int32_t count;
    struct sys_proc_info procs[SYS_PSINFO_MAX];
};

void syscall_init(void);

#endif
