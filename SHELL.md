# TigxOS shell (v1.1.4)

Interactive shell for TigxOS. Programs live on TigFS; the kernel embeds only `shell.elf`.

## Builtins

| Command | Meaning |
|---------|---------|
| `help` | This overview |
| `exit [N]` | Exit shell with status N (default 0) |
| `cd [DIR]` | Change current working directory (default `/`) |
| `pwd` | Print current working directory |
| `export NAME[=value]` | Export variable for child processes |
| `unset NAME` | Unset variable |
| `set` | List variables |
| `env` | List exported variables |
| `jobs` | List background jobs |
| `fg [N\|pid]` | Foreground a job |
| `wait [pid]` | Wait for pid or all |
| `kill PID` | SIGKILL a process |
| `sh FILE` | Run a script |

## Syntax

- **Quotes**: `"..."` and `'...'`
- **Vars**: `$NAME`, `${NAME}`, `$?` (last status)
- **Redirection**: `>`, `>>`, `<` (also per pipeline stage)
- **Pipes**: `a \| b \| c` — concurrent stages, real kernel pipes
- **Jobs**: `cmd &` — background; Ctrl-C sends SIGINT to the foreground job only
- **Lists**: `;`, `&&`, `\|\|`, `#` comments

## Paths, cwd and PATH (v1.1.2 / v1.1.4)

- Each process has a cwd (absolute path string). Shell starts at `/`.
- Relative paths are resolved against cwd; absolute paths start with `/`.
- TigFS is still flat: the full path string (≤27 chars) is the file name.
- **Standard layout**: programs are stored as `bin/NAME.elf`.
- **PATH** (default `bin:.`, exported): bare command names are searched in each
  PATH component. Typing `ls` finds `bin/ls.elf`. Names with `/` are used as-is.

## Programs on disk

**Round 1:** `hello`, `echo`, `loop`, `args`, `cat`, `rm`, `ls`, `wc`, `writefile`, `lang65`, `exitn`  
**Round 2 (1.1.4):** `cp`, `mv`, `touch`, `head`, `clear`, `tee`, `sleep`, `basename`, `dirname`  

(`shell` and `args` also under root and `bin/`)

## Environment

Children receive exported variables via `envp` (`getenv` / `environ` in libc).

## Signals (minimal)

| Signal | Number | Effect |
|--------|--------|--------|
| SIGINT | 2 | Force-exit (Ctrl-C → foreground job) |
| SIGKILL | 9 | Force-exit (`kill`) |
| SIGTERM | 15 | Force-exit |

Exit status for signals is `128 + sig` (e.g. 130 for SIGINT).

## Limits (v1.1.4)

- Line length: 256
- Args per command: 32
- Pipeline stages: 4
- Max open FDs per process: 16
- File size: 56 KB (TigFS DIRECT 112)
- Path / cwd length: 64 (FS name limit 28)
- User heap: `SYS_BRK` from `0x02000000` toward the stack

## Build / test

```
make ARCH=x86_64
make ARCH=i386
make test          # both arches, headless QEMU
make test-qemu-64
make run-32        # i386
```
