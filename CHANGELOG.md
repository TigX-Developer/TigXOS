# TigxOS changelog

## 1.1.3 — Standard filesystem layout and PATH
- **Layout**: user programs installed as `bin/NAME.elf` on TigFS (makefile + mktigfs).
  `shell.elf` and `args.elf` also kept at root for compatibility.
- **PATH**: shell default `PATH=bin:.` (exported). Bare command names search each
  PATH component; prefers `bin/NAME.elf` (standard layout) then bare / `.elf`.
- **Bugfix**: i386 build — `default_exception_handler` used `frame->rip` on
  both arches; now uses `eip` on i386 and `rip` on x86_64.
- **Bugfix**: PATH search used `getenv` (process environ) instead of shell
  `var_get`, so `export PATH=` was ignored for command lookup.
- **Bugfix**: every PATH probe printed `elf: '…' not found` / `spawn: … failed`
  noise. Missing-file failures are now silent; shell still prints `: not found`
  only when all candidates fail.
- **Bugfix**: shell startup used `getenv("PATH")` to decide default; now uses
  shell `var_get` so the default is set reliably.
- **Bugfix**: `fs_resolve_path` now normalizes `.` and `..` (so `cd ..` works).
- **Bugfix**: open/unlink/stat/spawn reject names ≥ TIGFS_NAME_LEN after resolve.
- **Bugfix**: PATH parser infinite loop on over-long components (now consumed/skipped).
- **Bugfix**: `fs_resolve_path` no longer silently truncates; rejects too-long paths
  and >16 components.
- **Bugfix**: PATH search no longer tries cwd first; only searches PATH in order
  (`.` must be listed explicitly, default is `bin:.`).
- **Bugfix**: auto-format only on blank superblock; never on I/O error or foreign magic.
- **Dev**: makefile uses `-MMD -MP` for automatic header dependencies.
- **Bugfix**: empty `PATH=` no longer falls back to `bin:.` (only unset does).
- **Bugfix**: mount validates every used inode's size and block LBAs; corrupt
  metadata is refused. `fs_read` also rejects out-of-range block pointers.

- Still flat TigFS; path strings with `/` are just names. Real directories later.

## 1.1.2 — Paths and current working directory (cwd)
- Per-process `cwd` (absolute path string, PATH_MAX=64), inherited on spawn.
- `SYS_CHDIR` / `SYS_GETCWD`; libc `chdir` / `getcwd`.
- Path resolution (`fs_resolve_path`): relative paths prepend cwd; absolute start with `/`.
  Full resolved string is the flat TigFS name (still ≤27 chars).
- Shell: real `cd [DIR]`, `pwd`, prompt shows cwd; help updated.
- open / unlink / stat / spawn all resolve via the calling process cwd.
- No real directories yet (flat FS); this is the foundation for 1.1.3 layout + PATH.

**Offset note**: the previous tag that shipped as 1.1.8 is the 1.1.1 baseline below.
No intermediate version numbers for pure bug-fix iterations.

## 1.1.1 — TigFS v5 + clean boot (was labelled 1.1.8)
Stable interactive shell. Debug noise removed; all structural fixes retained:

**Page tables / spawn**
- COW on user AS table walk (`x64_get_or_alloc`) — no shared-kernel PTE mutation
- `aspace_map` takes `uint64_t` flags (`PAGE_NX` not truncated)
- ELF/spawn failure reasons; DEAD slot AS reclaim

**Boot → prompt**
- ELF `p_align` = page-align only; embed fallback for shell.elf
- mktigfs absolute LBAs; EFER.NXE for `PAGE_NX`
- RFLAGS `0x202` on user iret frame
- Keyboard `sti` after `wait_queue_block`
- `filetable_init` before `process_init` (console of_id)
- Console write bounce buffer; shell does not busy-loop on failed read

**Unlink-while-open / malloc / sbrk**
- `fs_read` / `fs_write` / `fs_size` use `find_inode_any` so open FDs survive unlink
- malloc ignores double-free; sbrk rejects negative overflow; process_finalize clears heap_brk

## 1.1.0 — Stable 1.x
Stable baseline from the 1.0.83 RC. No new subsystems — the 1.0 line is complete.

**What 1.1.0 is**
- Dual-arch (i386 + x86_64), thin kernel, green lizard userland
- Per-process address spaces, concurrent pipes/jobs, Ctrl-C / fg job
- Shared open-file table, envp for children, SYS_BRK + malloc
- Programs on TigFS (shell embedded); `make test` both arches
- Docs: `SHELL.md`, this changelog

**1.0 theme summary**
| Ver     | Theme |
|---------|--------|
| 1.0.73  | Guard rails (test-qemu, kill/respawn) |
| 1.0.74  | Programs on disk |
| 1.0.75–76 | Address spaces |
| 1.0.77  | Concurrent pipelines + jobs |
| 1.0.78  | Ctrl-C / foreground |
| 1.0.79  | Shared open files |
| 1.0.80  | env for children |
| 1.0.81  | Kernel bug sweep |
| 1.0.82  | Userland + language (brk/malloc/str_concat/glc) |
| 1.0.83  | RC polish (orphan reap, unlink-while-open, SHELL.md) |

Post-1.1 work (not in this tag): TigFS dirs/paths, richer signals, VTs, networking, etc.

## 1.0.83 — RC
- **Orphan reap**: children of DEAD parents are finalized (not only UNUSED).
- **process_exit**: reparents children to a living shell (same as kill).
- **Unlink-while-open**: `fs_detach_name` marks inode `TIGFS_UNLINKED` so
  lookup/dirents hide the name; blocks freed on last `of_decref` via `fs_remove`.
- **SHELL.md**: user-facing shell reference for the RC.
- Feature freeze for 1.0.x — polish only. Done-when: docs match behavior;
  both arches build; residual HIGH bugs from prior sweeps closed.

## 1.0.82 — Userland + language sweep
- **SYS_BRK (18)**: per-process heap break at `USER_HEAP_BASE` (0x02000000);
  pages mapped on demand up to stack guard.
- **libc**: `sbrk`, `malloc`, `free`, `realloc` (freelist over brk).
- **str_concat**: allocates with `malloc` — `s = s + x` in a 10k loop is safe.
- **printf**: `%i`, `%p`, `%l`d/u/x, `%X`.
- **Shell**: `LINE_MAX` 160→256, `MAX_ARGS` 16→32.
- **glc**: `/` and `%` guard divide-by-zero (yield 0); non-main functions get a
  64-frame recursion depth guard (`__gl_call_depth`).
- Done-when: 10,000-iteration string loop does not corrupt earlier results.

## 1.0.81 — Kernel bug sweep
- **FS lock**: `spinlock_t fs_lock` serializes all TigFS ops (shared `sector[]`,
  inodes, bitmap). `fs_write` creates under the same lock (no nested public call).
- **copy_from_user / copy_to_user**: page-chunked phys copies (not per-byte).
- **spawn_busy**: clear under `cli` on success and error paths.
- Carries 1.0.80-update3: copy_* via AS, write size cap, stack partial free,
  pipe fail paths, console pin, fg_remove on kill, pipeline kill-on-fail.
- ELF `load_segment` already rejects vaddr outside USER_LOAD_MIN..MAX (kept).
- Done-when: concurrent FS ops no longer clobber `sector[]`; spawn staging
  is interrupt-safe on UP.

## 1.0.80-update3 — critical/high bug sweep
- **copy_from_user / copy_to_user** via AS phys for read/write/pipe/stat/getdents/pipefds
- user_range_ok requires process AS (no weak global fallback)
- sys_write caps at TIGFS_MAX_FILE before kmalloc
- aspace_map_user_stack frees unmapped tail frames on partial failure
- sys_spawn serializes static arg/env stores (spawn_busy)
- sys_pipe fail paths free pipe + both of objects
- of_decref: pin console forever; O_RDWR drops reader+writer pipe refs
- process_fg_remove on kill; exit_status documented as 0..255
- mktigfs comment VERSION 6; spawn table-full comment updated

## 1.0.80 — env for children
- `spawn` / `SYS_SPAWN` take an **envp** vector; stack layout is
  `[argc][argv…][NULL][envp…][NULL][strings]`
- crt0 sets **`environ`**; libc **`getenv(name)`** walks it
- Shell **`export`** builds `NAME=value` entries and passes them via
  `spawn_env` so children see exported variables
- `args.elf` prints environ and `getenv(FOO)` for a quick check
- Done-when: `export FOO=bar` then `args` shows FOO=bar

## 1.0.79 — Shared open files
- Global open-file table (`filetable.c`, 64 slots): **refcounts**, shared
  **offset** for regular files, close-on-last-ref
- Process FD slots hold `of_id` only; `dup`/`dup2`/`inherit` call `of_incref`
- Pipes also use shared objects; last close drops pipe reader/writer refs
- **Unlink while open**: `of_mark_unlink` defers `fs_remove` until last close
- Done-when: two processes writing the same open file advance one shared offset

## 1.0.78 — Ctrl-C and foreground job
- Keyboard **Ctrl-C** delivers SIGINT to the **foreground job**, not the shell
- Kernel tracks a foreground pid list (`process_fg_add` / `process_fg_clear` /
  `process_fg_interrupt`); shell marks stages via `setfg` before `wait`
- Minimal signal numbers behind `SYS_KILL`: SIGINT=2, SIGTERM=15, SIGKILL=9
  (all force-exit for now; exit status 128+sig)
- `SYS_SETFG` (17): add pid to fg job, or 0 to clear
- libc: `setfg(pid)`, `kill_sig(pid, sig)`; `kill(pid)` → SIGKILL
- Done-when: `loop` then Ctrl-C stops it and the prompt returns

## 1.0.77 — Real concurrency in the shell
- Concurrent pipelines: `a | b | c` spawns **all stages at once** with real
  `pipe()` connections (no temp files, no 4KB sequential limit)
- Spawn inherits only stdio (0/1/2) so extra pipe ends do not pin writers open
- True background: `cmd &` runs without waiting for prior jobs; `jobs`,
  `fg [N|pid]`, `wait [pid]` with `[pid] Done` notices
- Removed `jobs_wait_all` before every spawn (AS isolation makes this safe)
- Done-when: `cat big | wc` streams; `loop &` runs while you type
- TigFS: DIRECT 80→96 (48KB max); VERSION 5 (shell grew with concurrent pipe code)
- FIX: sys_pipe claimed the same FD twice — reserve read slot before write alloc
- MAX_FDS 8→16 (3–4 stage pipelines + saved stdio)
- jobs_reap_done via wait_noblock; Done notices at prompt; `cmd | cmd &` works

## 1.0.77-update2 — review fixes
- **CRITICAL** fs write: allocate `max(existing, offset+len)` before fs_read (heap overflow)
- **CRITICAL** user pointers: `user_peek_byte` / mapped phys for path+argv copy-in
- **HIGH** `user_range_writable` for sys_read and sys_pipe user buffers
- **HIGH** pipeline: reject leading/trailing/empty `|`; per-stage `>`/`>>`/`<`
- **HIGH** pipeline fail: `kill` survivors then wait (no hang on full pipe)
- **HIGH** early dup fail closes `saved_in` (no FD leak)
- **HIGH** pipe_alloc/incref/decref under cli (preemption-safe refcounts)

## 1.0.76 — Address spaces II
- Full AS teardown: intermediate page tables tracked and freed on destroy
- User stacks mapped at `0x03E00000–0x03F00000` (16 KB) with an unmapped
  guard page below; no more heap-backed user stacks
- ELF `p_flags` → page permissions: text R-X (no WRITE), data RW + NX
  (x86_64 NX bit; i386 has no NX without PAE)
- Linker scripts emit separate PT_LOAD segments (PHDRS) so RWX warnings
  are gone for correctly linked user ELFs
- Done-when: two copies of the same ELF can run with separate memory;
  a stray write below the stack guard faults only that process

## 1.0.75 — Address spaces I
- Per-process page tables (`struct address_space`)
  - i386: private PD; kernel 4MB PSE entries copied; user uses 4KB tables
  - x86_64: private PML4; kernel half shared; user 4KB pages (huge split)
- ELF loader maps PT_LOAD into private frames in the process AS
- `aspace_switch` on schedule (user processes run with their own CR3)
- Syscall `user_range_ok` checks the process's own mappings
- `aspace_destroy` frees user frames on finalize
- Done-when: one program runs as before, on new tables

## 1.0.74 — Programs off the kernel image
- **tools/mktigfs.py**: host tool builds a complete TigFS image (super +
  bitmap + inodes + data) containing every user ELF. Supports \`--strip\`.
- Kernel embeds **only shell.elf** (init). All other programs live on disk.
- Boot mounts the pre-filled TigFS; formats only if magic/version missing.
  \`seed_shell_if_missing()\` writes the embedded shell only when the file
  is absent (fresh format / corruption recovery).
- **KERNEL_SECTS 480 → 256** (both boot.asm + makefile). Kernel image no
  longer grows when tools are added — the treadmill stops.
- Makefile: \`tigfs.img\` is built and spliced into \`os-image.bin\` at LBA 512.
- \`make size\` also reports tigfs.img size.

## 1.0.73 — Guard rails
- **FIX (timer-test hang)**: Suites used to run *before* `sti` / `scheduler_start`.
  `timer_sleep_ms` blocks the current process; with the scheduler not yet live
  the sleep never woke → permanent hang. Cause → symptom → fix:
  - Cause: test runner ordered before interrupts and scheduling.
  - Symptom: `make TESTS=1` hung inside the timer suite.
  - Fix: spawn a kernel thread (`test-runner`) *after* `scheduler_start`; it
    runs every suite with IRQs on, then exits QEMU via isa-debug-exit.
  - How to test: `make test-qemu-32` / `make test-qemu-64` (or `make test`).
- **make test-qemu**: headless QEMU (`-display none -serial stdio
  -device isa-debug-exit,iobase=0xf4,iosize=0x04`). Kernel writes exit code
  to port 0xf4. Exit status 0 = all checks passed, 1 = failures.
  One command validates a release: `make test`.
- **SYS_KILL review**:
  - Still rejects pid ≤ 0 and non-user processes.
  - Closes all FDs (pipe refcounts drop correctly).
  - Reparents children of the killed process to a living shell (or marks
    them orphan) so they are not stuck with a dead parent_pid.
- **Shell respawn hardening**:
  - Rate-limited (at most one successful respawn per ~2 s at 100 Hz).
  - After 3 consecutive failures, respawn is disabled (avoids spin).
  - New shell always receives a clean console fd set (spawn → process_init_fds).
- **make size**: reports kernel.bin vs KERNEL_SECTS and shell.elf size.
- Global test counters (`test_total_failed` / `test_total_checks`) for the
  QEMU exit code.

## 1.0.72
- Scripts: `sh FILE` runs a file line-by-line; `#` comments (outside quotes)
- List ops: `cmd ; cmd`  `cmd && cmd`  `cmd || cmd`  (spaces required)
- Background: `cmd &` — tracked in job table; `jobs` lists them
  - Before next program spawn, pending jobs are waited (shared 0x400000)
- writefile: no text args → read stdin until EOF (`echo x | writefile f`)
- LINE_MAX 128→160, MAX_ARGS 8→16
- TigFS: inode 256→512B, DIRECT 52→80 (40KB max file); VERSION 4
- FIX: shell `|` uses temp files (no 4KB pipe deadlock on sequential stages)
- FIX: `sh` reports error on lines longer than LINE_MAX
- SYS_KILL (16) + `kill PID` builtin; removes job from table
- PIPE_BUFSZ 4K→8K for direct pipe(2) users


## 1.0.71
- SYS_PIPE (15): ring buffer 4KB × 8 pipes, blocking read/write via wait queues
- FD_PIPE with reader/writer refcounts; close/exit/dup/dup2/inherit adjust refs
- EOF when all writers closed; write fails when all readers closed
- libc: `pipe(int fd[2])`
- Shell: `cmd1 | cmd2` (and multi-stage); sequential stages (shared 0x400000)
  - left must fit in 4KB pipe buffer before it exits (no concurrent reader yet)
- help lists Pipe syntax
- FIX: TigFS max file 24KB→26.5KB (`TIGFS_DIRECT` 48→52); x86_64 shell.elf ~26KB failed to seed
  `TIGFS_VERSION` 2→3 so old images reformat

## 1.0.70
- Shell quoting: `'literal'`, `"expand $VAR"`, bare words with `\` escapes
- Variables: `$NAME`, `${NAME}`, `$?` (still works)
- Builtins: `set`, `export`, `unset`, `env`
  - `set` / `set NAME=value` / `set NAME value`
  - `export NAME` / `export NAME=value`; `env` lists exported
- Variable expansion applied to argv before spawn (children see expanded args)
- Unclosed quote → syntax error
- KERNEL_SECTS 448 → 480 (x86_64 image grew past limit)
- FIX: console `\b` is non-destructive (cursor left only); shell erase via redraw
- FIX: `\"\$VAR\"` / `\$VAR` no longer expands (escape checked before `$`)
- FIX: tokenize errors distinguished: unclosed quote / pool full / unclosed `${`

## 1.0.69-fix2
- Ring-3 exceptions kill the faulting process (no whole-OS panic); kernel faults still panic
- USER_LOAD_MAX / USER_VADDR_MAX tightened to 64MB (0x04000000)
- spawn: check free slot + alloc stack before ELF load (failed spawn no longer clobbers when full)
- fs_write: allocate new blocks first; only free old data after success (no empty-file on OOM)
- listdir buffer 1024→2048 (~64 dirents)
- shell respawned when it exits
- elf bad-magic silent (shell prints not found)
- str_concat comment matches 8 buffers

## 1.0.69
- Line editing in the shell:
  - Cursor left/right, Home/End
  - Backspace and Delete (mid-line)
  - Insert in the middle of the line
  - Up/Down command history (16 entries, skips duplicate of last)
- Keyboard: extended scancodes (E0) for arrows, Home, End, Delete delivered as
  special bytes 0x80–0x86 through the console read path
- help lists Edit: keys
- FIX: TigFS max file 16KB → 24KB (`TIGFS_DIRECT` 32→48); shell.elf (~19KB) failed to seed.
  Inode layout adjusted; `TIGFS_VERSION` 1→2 so old images reformat cleanly.

## 1.0.68
- SYS_DUP (13) and SYS_DUP2 (14): value-copy of file_desc slots.
  - close() may now close any fd including 0/1/2 (needed for restore).
- Shell redirection (spaces required):
  - `cmd > file`   truncate/create, stdout
  - `cmd >> file`  append/create, stdout
  - `cmd < file`   stdin
  - one in + one out ok; save/dup2/spawn/wait/restore around the child
  - inherits snapshot FDs (see 1.0.67 notes — independent offsets, no refcount)
- libc: `dup()`, `dup2()`
- help lists redir syntax
- FIX: builtins also honor `>`, `>>`, `<` (parse_redirs runs before dispatch;
  `true > f` creates/truncates f; `pid > f` captures the pid line)

## 1.0.67
- FD inheritance on spawn: children copy parent's open fd table (0/1/2 + open files).
  - `process_inherit_fds()` used by `process_create_user` / `process_create_user_at`.
  - **Snapshot semantics (documented in process.c):**
    - Independent offsets/path per process — not a shared open-file object.
    - Two writers to the same TigFS name can clobber (FS not multi-open-aware).
    - No refcount/revoke: parent unlink while child holds open may fail later I/O.
    - Parent close does not affect child (value copy). Fine for sequential shell.
- glc bounds: `argv[i]` → `argv_get()` (OOB → `""`), `s[i]` → `str_index()` (OOB → 0).
  - No GCC statement-expressions; portable libc helpers. OOB is silent by design
    (no crash); check `argc` if you need to distinguish missing vs empty.
- Seed `lang65` and `exitn` into TigFS.
- KERNEL_SECTS 384 → 448 (makefile + both boot.asm) so x86_64 fits after extra ELFs.
- str_concat pool 4 → 8 buffers (loop `s = s + x` still unsafe).
- Shell builtins (`pid`, `help`, `true`, `false`) all set `$?` / last_status.
- (still open) Shared ELF load 0x400000 — sequential only until 1.0.76 address spaces.
- (still open) RWX user ELF segments (no page perms yet).

## 1.0.66
- Exit codes end-to-end:
  - Kernel: process_exit(status) + wait() return status (fixed in 1.0.65 update)
  - Shell: `$?` holds last command status; expands when used as an argument (`echo $?`)
  - Builtins: `true` (0), `false` (1), `exit [N]` / `quit [N]`
  - Command not found → status 127
- Shell help lists true/false and $?
- FIX: process_wait only reaps children of the caller (parent_pid check)
- FIX: exit status stored as 0..255 so wait failure (-1) is unambiguous; shell $? always 0..255
- FIX: status_str safe for full range; exit [N] clamped to 0..255

## 1.0.65
- Language growth 1 (green lizard / glc):
  - `for` loops (C-style: `for let i = 0; i < n; i = i + 1 { ... }`, parts optional)
  - string concatenation with `+` (via `str_concat` rotating static buffers in libc)
  - string indexing `s[i]` → char as int
  - string compare `==` / `!=` (strcmp)
  - char literals `'x'`, `'\n'`, etc. (as int)
  - `break` / `continue` (already present; now solid with for)
  - better errors: almost all diagnostics include source line numbers
- libc: `str_concat(a, b)` for gl string `+`
- Demo: examples/lang65.gl exercises the new features
- FIX: exit status was always 0. `_exit(code)` now passes the code; `process_exit(int status)` stores it; `wait()` can return non-zero (unlocks 1.0.66).
- FIX: str_concat length clamp no longer underflows; documented rotating-buffer lifetime (loop `s = s + x` is unsafe).
- Shell help lists quit and ?.

## 1.0.64 (includes crt0/argc fix)
- FIX (the big one): programs always saw argc == 0. user/crt0.c read the stack pointer from inside a C function, after the compiler's prologue had already moved it, so it read below [argc]. _start is now assembly and passes the true initial SP to _start_c(). Fixes echo, writefile, cat, rm, wc, args on both i386 and x86_64.
- FIX: kprintf now supports the '-' flag and width for %s (process list printed "%-8s ring116129" because the unknown flag shifted every later argument).
- FIX: console decodes UTF-8 and maps to CP437 (em dash, arrows, box drawing) instead of printing "GammaCe" garbage on VGA text mode.
- Shell: removed unused cmd_ls (ls is a program).
- NOTE: x86_64 kernel.bin is ~93% of KERNEL_SECTS=384; raise it (or move ELFs to disk) before adding more programs.

## 1.0.64 (earlier changes)
- Core tools in green lizard: cat, rm, ls, wc, writefile (examples/*.gl).
- glc: readch, writech, listdir, unlink, stat, open flags, break/continue.
- libc helpers backing readch/writech/listdir.
- Seeded into TigFS; shell runs them by name.


## 1.0.63
- File syscalls hardened: O_APPEND, O_TRUNC; O_RDONLY/O_WRONLY access checks on read/write.
- SYS_UNLINK (11) and SYS_STAT (12); struct sys_stat { size, mode }.
- libc: unlink(), stat(); open flags O_TRUNC / O_APPEND.
- Kernel tests: rewrite/shrink, remove-missing, syscall number checks.
- Bug fix: wait/reap race — DEAD children kept until wait(); only orphans auto-reaped.
- Bug fix: TESTS=1 runs after scheduler_start (no hang on timer_sleep).
- Bug fix: shell args[] room for NULL terminator (no OOB at MAX_ARGS).
- Bug fix: PAGE_USER only on 4MB–64MB window (kernel low mem not user-writable).
- Bug fix: copy_user_argv validates each argv slot before dereference.
- Bug fix: read(fd,buf,0) returns 0; open() rejects invalid access modes.
- Bug fix: glc tracks str vs int for let (argv[i] stays str through variables).

## 1.0.62
- Real echo.gl: prints argv[1..] separated by spaces (uses argc / argv[i]).
- glc: argc and argv[i] available inside main (see lizard/README.md).
- Shell passes argv to spawn (from 1.0.61).

## 1.0.61
- sys_spawn copies user argv into the kernel; args.elf test; shell passes argv.

## 1.0.58
- version.h single source; green lizard rename; makefile cleanup; dead kernel menu removed.
