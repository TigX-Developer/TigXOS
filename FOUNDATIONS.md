# TigxOS Foundations freeze (v1.1.12)

This release **freezes the Foundations track** (roadmap 1.1.6 → 1.1.12).
The dual-arch kernel, shell, TigFS userland, and Green Lizard toolchain are
the stable base for 1.2.x work.

## Release gate

```bash
make test          # i386 then x86_64 headless QEMU suites
# Expect: ALL TESTS PASSED (84 checks) on both arches
#         === both arches green ===
```

| Arch    | Target           | Exit |
|---------|------------------|------|
| i386    | `test-qemu-32`   | QEMU status 1 → PASS |
| x86_64  | `test-qemu-64`   | QEMU status 1 → PASS |

Suites: sync, waitq, timer, process, memory, fs, elf, syscall, console, regression.

Interactive smoke (optional):

```bash
make ARCH=x86_64 run
# ps · top · jobs · alias · Tab · halt
```

## What Foundations includes

| Area | Highlights |
|------|------------|
| **Kernel** | Dual-arch (i386 / x86_64), preemptive scheduler, syscalls, pipes, FG/Ctrl-C, halt |
| **Memory** | PMM, VMM/AS, heap (`kmalloc` / `brk`) |
| **FS** | TigFS image, cwd, PATH=`bin:.`, spawn ELF from disk |
| **Shell** | Pipes, redirs, jobs, history, Tab, aliases, `shell.rc`, halt |
| **Tools** | C: edit, date, uptime, uname, free, df, sysinfo, ps, kill, top |
| **Green Lizard** | Typed `.gl`, arrays, import, len/substr, glc → ELF |
| **Tests** | Headless serial + `isa-debug-exit` for both arches |

## Intentional limits (not bugs)

- TigFS is flat; max direct-block file size ~56 KiB (shell/userland size budget).
- Fixed `int[N]` arrays only in Green Lizard (no structs yet).
- Shell auto-respawns on `exit`/`quit`; use **`halt`** to stop the machine.
- `MAX_PROCESSES` is small (hobby scale).

## After the freeze

Roadmap continues at **1.2.x** (see project roadmap). Prefer not to break:

- Syscall numbers in `syscall.h` / `user/libc.h`
- TigFS on-disk layout used by `tools/mktigfs.py`
- `make test` as the dual-arch gate

Bugfixes on 1.1.12 are fine; new features belong in 1.2+.

## 1.2.0 status

Kernel infrastructure audit (GDT/IDT, memory, aspace, scheduler) landed as **1.2.0**. Foundations gate (`make test` both arches) still applies.

### 1.1.12-update2 hardening
Kill signal 0 / unknown signals, shell-only halt, psinfo `copy_to_user`,
array bounds in glc, tab match cap 96, extra process/syscall checks.

### 1.1.12-update3 bugfix
Shell script comments, filetable/pipe IRQ safety, edit find prompt,
RTC 12h binary mode, shell EOF exit, i386 huge-page split, sysinfo
memory consistency, glc string literals, docs/cleanup.

### 1.2.1
VFS layer + hierarchical TigFS directories; SYS_MKDIR; cwd-aware getdents.
