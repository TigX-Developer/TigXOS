/* user/syscall.h — ring-3 syscall numbers */
#ifndef USER_SYSCALL_H
#define USER_SYSCALL_H

#define SYS_EXIT        0
#define SYS_WRITE       1
#define SYS_YIELD       2
#define SYS_GETPID      3
#define SYS_SLEEP_MS    4
#define SYS_READ        5
#define SYS_OPEN        6
#define SYS_CLOSE       7
#define SYS_GETDENTS    8
#define SYS_SPAWN       9
#define SYS_WAIT       10
#define SYS_UNLINK     11
#define SYS_STAT       12
#define SYS_DUP        13
#define SYS_DUP2       14
#define SYS_PIPE       15
#define SYS_KILL       16
#define SYS_SETFG      17
#define SYS_BRK        18   /* new_brk → previous brk or (uintptr_t)-1 */
#define SYS_CHDIR      19   /* path → 0 or -1 */
#define SYS_GETCWD     20
#define SYS_SYSINFO    21   /* buf, size → 0 or -1 */
#define SYS_PSINFO     22
#define SYS_MKDIR      24
#define SYS_HALT       23   /* power off / hang cleanly */

#define O_RDONLY        0
#define O_WRONLY        1
#define O_RDWR          2
#define O_CREAT         0x100
#define O_TRUNC         0x200
#define O_APPEND        0x400

#endif
