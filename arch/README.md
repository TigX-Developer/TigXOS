# Arch layer (TigxOS 1.2.0)

Shared architecture interface + per-CPU implementation.

## Supported architectures

| ARCH     | Mode                         | QEMU                  |
|----------|------------------------------|-----------------------|
| x86_64   | Long mode (default)          | qemu-system-x86_64    |
| i386     | 32-bit protected mode + PSE  | qemu-system-i386      |

```
make                  # 64-bit
make ARCH=i386        # 32-bit
make run-32 / run-64
```

## Ring 3 + syscalls

- `gdt.c` reloads GDT with kernel/user code+data and a TSS
- `tss_set_kernel_stack()` updates RSP0/ESP0 on every context switch
- `int 0x80` (DPL=3) → `syscall_stub` → `syscall_dispatch`
- `process_create_user()` builds an iret frame + `user_trampoline`
- Panic dump shows CS.RPL and user RSP/SS when RPL=3

### Syscall ABI (stable through 1.2.0)

| # | Name         | Args                          |
|---|--------------|-------------------------------|
| 0 | SYS_EXIT     | —                             |
| 1 | SYS_WRITE    | fd, buf, len                  |
| 2 | SYS_YIELD    | —                             |
| 3 | SYS_GETPID   | —                             |
| 4 | SYS_SLEEP_MS | milliseconds                  |
| 5 | SYS_READ     | fd, buf, len                  |
| 6 | SYS_OPEN     | path, flags                   |
| 7 | SYS_CLOSE    | fd                            |
| 8 | SYS_GETDENTS | buf, len                      |
| 9 | SYS_SPAWN    | path, argv                    |
| 10| SYS_WAIT     | pid                           |
| 11| SYS_UNLINK   | path                          |
| 12| SYS_STAT     | path, stat_buf                |
| 13| SYS_DUP      | oldfd                         |
| 14| SYS_DUP2     | oldfd, newfd                  |
| 15| SYS_PIPE     | pipefd[2]                     |
| 16| SYS_KILL     | pid, sig                      |
| 17| SYS_SETFG    | pid                           |
| 18| SYS_BRK      | new_brk                       |
| 19| SYS_CHDIR    | path                          |
| 20| SYS_GETCWD   | buf, size                     |
| 21| SYS_SYSINFO  | struct sys_sysinfo *          |
| 22| SYS_PSINFO   | struct sys_psinfo *           |
| 23| SYS_HALT     | —                             |

Open flags: O_RDONLY=0 O_WRONLY=1 O_RDWR=2 O_CREAT=0x100 O_TRUNC=0x200 O_APPEND=0x400

x86_64: number in RAX, args RDI/RSI/RDX; return in RAX  
i386:   number in EAX, args EBX/ECX/EDX; return in EAX

## Layout

```
arch/
  arch.h
  x86_64/   boot entry switch interrupt
  i386/     boot entry switch interrupt linker.ld
```
