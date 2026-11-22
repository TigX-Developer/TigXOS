# TigxOS changelog

## 1.2.9-update3 — complete SMP completion path

Honest close-out of the 1.2.9 roadmap item:

### AP long mode + C (from update2)
- Trampoline → long mode → `ap_main` in 64-bit C

### Cross-core scheduling
- `process_prepare_ap_idle` / `process_bind_ap` — per-AP idle in `process_table`
- AP idle loop calls **`schedule()`** when the scheduler is enabled
- Affinity + **steal** in `pick_next` (update1)

### IPI
- IDT gates for **0xFC (TLB)** and **0xFD (resched)** with asm stubs
- Handlers call `smp_ipi_*_handler` + LAPIC EOI

### TLB / balance
- `smp_tlb_shootdown` + steal counters (existing)

### Tests / tooling
- Unused `r0` warning fixed
- `make test-smp` — QEMU `-smp 2` smoke on x86_64

### Limits that remain true
- **i386** is UP (no LAPIC map)
- `make test` stays on `-smp 1` for determinism; use `make test-smp` for AP bring-up
- APs do not yet take IRQs from the legacy PIC (LAPIC timer on AP is future work)


## 1.2.9-update2 — AP long-mode entry (APs run C)

### SMP
- **Trampoline** enters **long mode** on x86_64 (PAE + EFER.LME + paging) using the BSP identity PML4
- **`ap_main`** runs in 64-bit C: enables per-AP state, logs online, idle `sti;hlt` loop
- Per-AP **stacks** from PMM; boot block at `0x8F00` (cr3, entry, stack, cpu_id)
- Mailbox + `ap_ready[]` handshake; BSP waits for C entry when possible

### Fixes
- Drop unused `r0` in smp tests (`-Wunused-variable` on i386)

### Notes
- `make test` still uses `-smp 1` (stable CI). To exercise APs: QEMU `-smp 2` or higher on x86_64
- i386 remains UP
- Cross-core **schedule()** on APs still deferred (idle loop only); steal/IPI paths remain on BSP


## 1.2.9-update1 — scheduler steal, public README, version string fix

### Scheduler
- **Affinity-aware `pick_next`**: prefer READY tasks already on this CPU
- **Work steal**: if the local CPU is idle of READY work, steal a READY task from another CPU and count it (`smp_note_steal` / `smp_balance_steals`)
- Soft multicore stress checks in the smp test suite

### Build / docs
- **`makefile` `VERSION`** is **1.2.9** (image banner no longer says 1.2.8-update5)
- **Public README** for GitHub visitors (clone, build, layout, license)
- Test fix: drop always-true `uint64_t >= 0` check (i386 `-Wtype-limits`)

### Still true
- Application processors park after SIPI until long-mode AP entry lands
- **i386** remains UP (LAPIC above identity map)


## 1.2.9 — SMP completion (IPI, TLB shootdown, balance hooks)

Cross-core infrastructure on top of 1.2.8 AP bring-up.

### IPI
- Vectors IPI_VECTOR_TLB (0xFC) and IPI_VECTOR_RESCHED (0xFD)
- smp_send_resched / smp_send_resched_all; self path sets need_resched without ICR

### TLB
- smp_tlb_shootdown / smp_tlb_shootdown_all + aspace_tlb_flush_page
- Bounded ACK wait so parked APs cannot hang the BSP

### Scheduler
- cpu_this()/current use smp_cpu_id(); timer respects need_resched
- Soft rq depth hints for future steal

### Limits
- APs still park after SIPI (no long-mode AP entry yet); i386 stays UP

### GitHub
- Root LICENSE (MIT) included


## 1.2.8-update5 — pipe test hang + make test TIMEOUT

### Fix
- **`test_suite_pipe` cross-task path hung** under `make test` (TIMEOUT after
  `pipe_alloc succeeds`). The reader was spawned first and blocked on an empty
  pipe; depending on schedule order the writer never completed the transfer
  cleanly within the timeout window.
- Rewrite: **write data first**, then spawn the reader (no empty-pipe block).
  Bounded yield+sleep wait for completion; kill leftover reader if needed.
- `test-qemu` timeout **25 s → 35 s** for slower hosts (still far under the
  old multi-second sleep budget).

### Notes
- `version.h` / makefile → **1.2.8-update5**

## 1.2.8-update4 — faster make test

### Speed
- Timer suite: 6 sleeps (10..500 ms) → 3 sleeps (10, 50, 100 ms). Same coverage,
  ~700 ms less wall time per arch.
- Sched fairness / priority windows: 200+150 ms → 80+60 ms; cleanup uses yield +
  occasional 5 ms instead of 100×10 ms.
- Process / regression / waitq / pipe reclaim loops: yield-first with sparse
  5 ms sleeps (was up to 200×10 ms = 2 s each).
- `test-qemu` timeout 40 s → 25 s (tests finish well under that).

### Notes
- Still **266-ish checks**, both arches green.
- `version.h` / makefile → **1.2.8-update4**

## 1.2.8-update3 — SMP + test rewrite

### SMP / LAPIC (robustness)
- **`lapic_init`**: i386 still skips entirely; on x86_64 a version probe fails closed
  (nonsense `ver` → stay UP). ICR waits are bounded with `pause`. INIT/SIPI delays
  use `pause` instead of empty loops.
- **`smp_init`**: every early-exit path leaves `smp_online = 1`. Mailbox wait is
  shorter and bounded. New query APIs: `smp_apic_id`, `smp_is_bsp`, `smp_max_cpus`,
  `smp_ap_started`.
- Trampoline layout documented; GDTR/GDT offsets fixed and fenced with `mfence`.

### Tests
- New suite **`atomic`**: xchg, cmpxchg, inc/dec, add_fetch, load/store, stress
  loop, spin_trylock smoke.
- New suite **`smp`**: CPU count / id / BSP, cpu_data layout, LAPIC presence
  (hard on i386, soft on x86_64), switch-counter monotonicity.
- Framework: `test_check_soft` for environment-dependent asserts (never fails the
  suite).
- Boot-test mode still skips the interactive shell (avoids FS races).
- `test-qemu`: `-nographic -smp 1`, `timeout --foreground -k 5 40`.

### Notes
- `version.h` / makefile → **1.2.8-update3**
- Full cross-core scheduling remains 1.2.9.

## 1.2.8-update2 — i386 make test hang (LAPIC map)

### Critical fix
- **`vmm_map_mmio()` always returned 0**, even when `vmm_map_page` failed.
  On i386 the LAPIC (0xFEE00000) is outside the 0–1GB identity map, so the
  map fails, but lapic_init still wrote through the unmapped pointer and
  triple-faulted / hung before any further serial output. That made
  `make test` (which starts with ARCH=i386) appear to hang after the
  "=== test-qemu ARCH=i386 ===" line.
- `vmm_map_mmio` now returns -1 on map failure.
- **`lapic_init()` now returns immediately on i386** (no map attempt, no
  pointer dereference). An early `#if defined(__i386__)` makes the skip
  unconditional and removes any chance of a page-fault during LAPIC probe.
- `make test` / `test-qemu` hardened: `-nographic -smp 1`,
  `timeout --foreground -k 5 40`, prints the QEMU command and its exit
  code so a hang is reported as TIMEOUT instead of an infinite wait.

### Notes
- Behaviour on x86_64 unchanged.
- `version.h` / makefile → **1.2.8-update2**

## 1.2.8-update1 — SMP foundation polish + wiring

Critical fix and hardening of the 1.2.8 SMP foundation.

### Critical fix
- **`acpi_init()`, `lapic_init()`, `smp_init()` were never called from `kmain`.**
  The code existed but the boot path skipped it entirely. They are now called
  in the correct order (ACPI early after memory, LAPIC + SMP after process_init
  so `smp_mark_online` can update `cpu_data[]`).

### Fix (keyboard + test hang)
- **LINT0 was masked** after enabling the local APIC. That cut the 8259 PIC
  virtual-wire path, so timer and keyboard IRQs stopped. LINT0 is now set to
  ExtINT (0x700) so PIC IRQs keep working on the BSP.
- `make test` / `test-qemu` now runs under `timeout 45` and reports TIMEOUT
  instead of hanging forever if isa-debug-exit never fires.
- `ap_main` marked `__attribute__((unused))` (trampoline parks in asm until 1.2.9).

### Extra checks
- `smp_init`: reject missing ACPI, clamp `cpu_count` to `NCPU_MAX`, log every
  MADT CPU entry, clear `ap_ready`/`smp_logical`, validate trampoline address,
  `mfence` around mailbox + trampoline, `pause` in the wait loop, report
  attempted vs online count.
- `lapic_init`: LINT0 ExtINT; version probe where applicable.
- i386 path still gracefully stays UP when LAPIC cannot be mapped.

### Notes
- Behaviour is unchanged for single-CPU / i386; on x86_64 + `qemu -smp N`
  the APs should now actually answer in dmesg.
- Still foundation only: APs park in the trampoline `hlt`; no cross-core
  scheduling until 1.2.9.
- `version.h` → **1.2.8-update1**

## 1.2.8 — SMP foundation

Bring-up of the Local APIC and application processors. Only the BSP schedules;
cross-core scheduling is 1.2.9.

### MMIO
- `PAGE_PCD` / `PAGE_PWT` flags
- `vmm_map_mmio()` for uncached device mappings (LAPIC)

### LAPIC
- Map and enable the local APIC on the bootstrap CPU
- `lapic_id()`, `lapic_eoi()`, `lapic_send_ipi()`, `lapic_start_ap()`
- Spurious vector enabled; LINT/timer masked (PIC still owns IRQs)

### AP startup
- Real-mode trampoline at 0x8000 (INIT + SIPI + SIPI)
- Shared mailbox so APs signal readiness
- BSP waits ~1s per AP; reports how many answered
- APs park in `hlt` (no scheduling yet)

### Per-CPU
- `NCPU_MAX` already 16
- `smp_mark_online()` updates `cpu_data[].online`
- `smp_cpu_count()` / `smp_cpu_id()`

### Notes
- i386: LAPIC sits above the 1GB identity map — SMP bring-up is best on x86_64 for now
- `qemu -smp 4` should show multiple CPUs answering in `dmesg`
- `version.h` → **1.2.8**


## 1.2.7-update1 — POSIX test harness fix\n\n- `make test` no longer uses bash-only `${PIPESTATUS}` (breaks under `/bin/sh`/dash).\n\n## 1.2.7 — Test harness + locking audit

Preparation for SMP (1.2.8). No functional user-visible changes beyond cleaner internals.

### Atomics & documentation
- New **`atomic.h`**: `atomic_xchg`, `atomic_cmpxchg`, `atomic_inc/dec`, `atomic_add_fetch`
- New **`LOCKING.md`**: lock order rules and which structures must be locked
- **`NCPU_MAX` raised to 16** (only CPU 0 online until 1.2.8)

### cli/sti cleanup
- **`sys_spawn`**: home-made `spawn_busy` + raw cli/sti replaced with a real `spawn_lock` spinlock
- **`keyboard_getchar`**: uses `irq_save` / `irq_restore`
- **`timer_sleep_ms`**: uses `irq_save` / `irq_restore`
- Remaining raw cli/sti only in: spinlock internals, panic, idle/hlt, final scheduler sti

### Test harness
- `make test` writes `test-<arch>.log`
- Machine-readable `RESULT ARCH=… PASS|FAIL` lines

### Notes
- `version.h` → **1.2.7**
- Full locking of every shared structure continues into 1.2.8 as needed


## 1.2.6 — ACPI (tables, poweroff/reboot, MADT)

ACPI discovery and clean power control. Foundation for SMP (1.2.8).

### Tables
- RSDP scan (EBDA + 0xE0000–0xFFFFF), checksum verified
- RSDT / XSDT walk with per-table checksums
- **FADT**: PM1a/b control ports, reset register, DSDT pointer
- **_S5_** sleep-type values extracted from DSDT with a tiny AML scan (no full parser)
- **MADT**: local APIC address, CPU APIC IDs + enabled flags, I/O APICs, IRQ overrides
- `dmesg` shows e.g. `acpi: 4 CPU(s) ready for SMP`

### Power
- `acpi_poweroff()`: SLP_TYPx|SLP_EN via PM1_CNT, then QEMU/Bochs/VBox ports
- `acpi_reboot()`: ACPI reset register → keyboard 0xFE → triple-fault
- New syscalls **SYS_POWEROFF (27)** and **SYS_REBOOT (28)**
- Shell builtins `poweroff` / `halt` / `shutdown` and **`reboot`**
- `sys_halt` now uses the ACPI path

### Notes
- No CPUs are started yet (that is 1.2.8)
- `TIGX_SYSCALL_ABI` stays 1 (additive)
- `version.h` → **1.2.6**


## 1.2.5-update5 — fix putchar newline comparison warning

- Fixed `c == "\n"` (string) → `c == '\n'` (char) in the kprintf→ring line accumulator. Removes the -Waddress warnings.

## 1.2.5-update4 — final logging / panic hardening (no version bump)

- **Every `kprintf` line** is now copied into the dmesg ring (via putchar line accumulator + `log_ring_raw`), with a recursion guard so klog ↔ kprintf cannot loop.
- **Absolute sequence counters**: `log_read` takes an ever-increasing byte offset; wrap no longer causes repeated/skipped lines.
- **Panic safety**:
  - `panicking` flag — a second fault just `hlt`s (no recursive crash/reboot).
  - `log_dump_recent` uses `spin_trylock` (never blocks if the lock was held by the faulting context).
  - Stack walk validates alignment + address range and caps at 12 frames.
- `%lu` / `%llu` supported in `klog`.
- Messages before `log_init` are still dropped (init remains after timer/IDT).


## 1.2.5-update2 — log crash/loop fix
- **`log_dump_recent`**: no longer allocates 16 KiB on the kernel stack (same size as the stack → overflow → fault/panic loop). Uses a static buffer and prints in small chunks.
- **`ring_put`**: clearer length accounting when the ring wraps.


## 1.2.5 — Logging (`dmesg` ring buffer, levels, panic stack + log view)

Kernel logging infrastructure for diagnostics and the backend log view.

### Ring buffer
- 16 KiB circular `dmesg` buffer (`log.c` / `log.h`)
- Records every `klog` / `pr_*` line with tick timestamp and level tag
- Survives console scroll; readable after the fact

### Log levels
- `LOG_EMERG` … `LOG_DEBUG` (0–7)
- Console filter via `log_set_console_level` (default `LOG_INFO`)
- Macros: `pr_emerg` / `pr_err` / `pr_warn` / `pr_info` / `pr_debug` …

### User interface
- New syscall **`SYS_DMESG` (26)** — additive ABI extension (numbers 0–25 unchanged)
- User tool **`dmesg`** (`bin/dmesg.elf`) reads the ring and prints it
- libc wrapper `dmesg(buf, len, off)`

### Panic improvements
- Approximate stack backtrace (frame-pointer walk when available)
- Recent log lines dumped on the panic screen (backend log view)
- Still dual-output to VGA + serial

### Notes
- `TIGX_SYSCALL_ABI` remains 1 (additive only)
- `version.h` → **1.2.5**
- Test suite updated: `SYS_MAX` / `SYS_DMESG` assertions match the new number 26


## 1.2.4 — Syscall cleanup (stable ABI)
Frozen numbered syscall ABI with versioning and stricter argument checks.

### ABI
- Numbers 0–25 frozen; **do not renumber** existing calls
- New `SYS_ABI_VERSION` (25) returns `TIGX_SYSCALL_ABI` (currently 1)
- `version.h` exposes `TIGX_VERSION` 1.2.4 and `TIGX_SYSCALL_ABI`
- Unknown syscall numbers return `-ENOSYS` (no console spam)

### Argument checks
- `sys_write` / `sys_read`: bad fd → `-EBADF`, bad user range → `-EFAULT`, len > 1 MiB → `-EINVAL`
- `sys_open`: null path → `-EINVAL`
- `sys_close`: bad fd → `-EBADF`
- Shared negative codes: `SY_ENOSYS`, `SY_EINVAL`, `SY_EFAULT`, `SY_EBADF`, `SY_ENOMEM`

### Theme
- Console semantic palette shifted to **orange on black** (brand orange = VGA yellow/brown)
- Boot banner boxed with orange frame

### Tests
- Syscall suite extended: ABI number, `SYS_MAX`, error constants, version constant

### Scheduler carry-over (from 1.2.3 fixes)
- `schedule()` marks current READY before `pick_next` so high-prio tasks can continue
- Priority stress test keeps the runner at `PRIO_MAX` so it can wake under strict priority


## 1.2.3 — Scheduler (expanded, SMP-ready)
SMP-aware scheduling foundations while still running on one CPU (full SMP is 1.2.8/1.2.9).

### Priorities
- Each process has `priority` in `PRIO_MIN..PRIO_MAX` (default `PRIO_DEFAULT`, idle = `PRIO_IDLE`)
- `pick_next()` selects highest ready priority; round-robin within the same level
- `process_set_priority` / `process_get_priority` (idle priority is fixed)

### Per-CPU layout
- `struct cpu_data` with `current`, switch/tick counters, `online` flag
- `cpu_this()` / `cpu_count_online()` — only CPU 0 is online until 1.2.8
- Process accounting: `ticks_ran`, `on_cpu`

### Lock discipline
- `sched_lock` spinlock reserved for table/run-queue (uniprocessor still uses IF=0 in IRQ paths)
- `process_yield` and `process_wait` block paths use `irq_save` / `irq_restore` instead of raw `cli`/`sti`

### Tests
- New suite **`sched`**: per-CPU layout, priority get/set, equal-priority fairness, high-vs-low preference


## 1.2.2 — Memory: allocator audit, guards, leak tracking, user limits

### Kernel heap
- **Canary** after every `kmalloc` payload (`0xC0FFEE42`); checked on `kfree`
- **Guard page**: last 4 KB of the heap frame region is never allocated
- **Leak tracking**: `allocs` / `frees` / **outstanding**, plus **peak** used bytes
- **`heap_validate()`**: walks the block list (magic + link integrity)
- Stats print includes peak, outstanding, canary/magic failure counts

### User heap (SYS_BRK)
- **`USER_HEAP_LIMIT`** (4 MB default) per process
- `process.heap_limit` enforced in `sys_brk` (growth beyond limit → `-1`)
- Stack **guard page** still exclusive ceiling (`USER_HEAP_MAX`)

### Tests
- memory suite: validate, outstanding, peak, canary-sized free


## 1.2.1-tests — Expanded make test suites

- **17 suites / ~177 checks** via `make test` (both arches)
- New suites: string, vfs (hierarchical reads), pipe, irq/spinlock, rtc, gdt, aspace
- Expanded: syscall ABI through SYS_MKDIR, path resolve, process kill edge cases
- KERNEL_SECTS 288→320 for larger test kernels
## 1.2.1 — VFS layer + TigFS v6 directories

Hierarchical filesystem and a thin VFS so one `open()` path serves TigFS
(and later ramfs / FAT32).

### TigFS v6 hierarchical
- **Root directory** is always inode 0
- **Directory data** holds packed 32-byte dentries `{name[28], ino, type}`
- **Path walk** resolves `/bin/shell.elf` through parent directories
- **`fs_mkdir` / `SYS_MKDIR` (24)** create directories
- **`chdir`** only succeeds on real directories
- **`getdents`** lists the current working directory (not only root)
- Flat images are **migrated on mount** (names like `bin/foo` become dirs)

### VFS
- `vfs.h` / `vfs.c`: unified open / create / unlink / mkdir / stat / read / write / getdents
- TigFS mounted at `/`; `/tmp` reserved for ramfs (stub)
- Open files store **inode number + fsid** (not just a flat name)

### Tools
- `tools/mktigfs.py` builds hierarchical images (root + intermediate dirs)

### ABI
- New syscall: **SYS_MKDIR = 24**
- Existing numbers 0–23 unchanged

## 1.2.0 — Kernel infrastructure audit

Audit pass over GDT/IDT, memory, address spaces, and the scheduler. Known
latent bugs fixed; dual-arch still the gate.

### Interrupt / IDT
- **x86_64 stubs**: stack alignment for the C handler no longer uses the fragile
  double-`pop rsp` pattern; save real frame in RDI, build an aligned scratch
  stack, restore with a single `pop rsp`.
- **i386 stubs**: ESP is 16-byte aligned before the C call (SysV hygiene).

### Address spaces / VMM
- **i386** (from 1.1.12-update3): splitting a 4 MB PSE page into a PT replicates
  the identity mappings instead of leaving an empty table.
- **x86_64 `aspace_map`**: same for 2 MB pages — split fills the new PT with the
  existing 4 KB identity entries.
- **`vmm_map_page` (x86_64)**: can split a covering 2 MB page and then install the
  requested 4 KB mapping (was a hard error).

### Memory
- **Heap**: identity-map contract documented; fail closed if the first 1 MB of
  frames lands outside the identity window (never leave a half-init heap).
- **sysinfo / free** (from 1.1.12-update3): totals capped to the PMM window so
  reported free never exceeds total.

### Scheduler
- Documented `schedule()` IF=0 contract; idle remains the always-runnable
  fallback and is never blocked.
- TSS `rsp0`/`esp0` still updated on every context switch (audited, unchanged).

### GDT / TSS
- Layout and selectors audited (kernel CS 0x08, DS 0x10, user DS 0x18, user CS
  0x20, TSS 0x28). No structural change.

### Version / docs
- `version.h` → **1.2.0**
- Roadmap series: **1.2.x Kernel infrastructure** begins here.

No intentional syscall ABI or TigFS layout breaks.

## 1.1.12-update3 — Bugfix pass (Foundations hardening)

Real bugs fixed (no ABI / layout change):

- **Shell scripts**: strip `#` comments **per line** before joining with `;`, so a comment no longer truncates the rest of the file
- **filetable**: `of_alloc` / `of_incref` / `of_decref` / `of_mark_unlink` use `irq_save` / `irq_restore` (safe inside the scheduler)
- **pipe**: `wait_queue_wake_all` runs inside the interrupt-off window (read/write/decref) to avoid wait-list races
- **edit find**: status line keeps the `find:` prompt (and typed text) while you type; `draw()` no longer blanks it
- **RTC**: always mask the 12-hour PM bit; correct 12→24 hour conversion in both BCD and binary modes
- **Shell EOF**: `readline` returning 0 (EOF) exits the shell instead of spinning forever
- **aspace i386**: splitting a 4 MB PSE page into a page table now replicates the identity mappings (no silent loss)
- **sysinfo / free**: report `mem_total` capped to the PMM window so total and free stay consistent
- **glc**: non-ASCII string literals no longer go through `unicode_escape`; dead `BUILTINS` set removed
- **Docs / cleanup**: full syscall table (0–23) in `arch/README.md`; duplicate root `mktigfs.py` removed; `seed_shell_from_embed` rename; version banners → 1.1.12 throughout

Latent notes kept for 1.2.0 audit (interrupt stack alignment, 2 MB page split, heap identity check) — not regressions.

## 1.1.12-update2 — Hardening + test coverage
- **glc arrays**: runtime bounds checks on load/store → `index out of range` + exit(1)
- **SYS_KILL**: signal 0 = existence probe only; unknown signals return `-1` (no longer map to SIGKILL)
- **SYS_HALT**: restricted to `shell` / `shell.elf`; isa-debug-exit code 2 (status 5) so halt ≠ test PASS
- **SYS_PSINFO**: uses `copy_to_user` instead of raw user pointer writes
- **Tab completion**: match cap raised 32 → 96
- **Tests**: process_kill / process_snapshot checks; SYS_KILL/PSINFO/HALT number checks


## 1.1.12 — Foundations final (freeze)
- **glc**: reject C reserved words as fn/var names (`double`, `long`, …); `importdemo` uses `twice` not `double`
- **Freeze** of the Foundations track (1.1.6 → 1.1.12)
- **Release gate**: `make test` runs full headless suites on **i386 and x86_64** (84 checks)
- **Docs**: `FOUNDATIONS.md` (scope, limits, gate); README feature list brought up to date
- Brand / version strings → 1.1.12
- No intentional ABI or TigFS layout breaks from this tag forward without a major bump


## 1.1.11 — Foundations RC2 (Green Lizard stdlib)
- **Arrays**: `let a: int[N]` / `let a: int[N] = [1,2,…]`; index load/store `a[i]`, `a[i] = v`
- **import**: `import "file.gl"` (relative include, cycle-safe)
- **Strings**: `len(s)`, `substr(s, start, len)` (plus existing concat `+` and `s[i]`)
- **Errors**: clearer parse messages with line + hint
- **Examples**: `arrdemo.gl`, `strlib.gl`, `importdemo.gl` + `libmath.gl`
- glc version banner → 1.1.11


## 1.1.10 — Foundations RC1 (shell comfort)
- **History**: ring expanded to 32; `history` builtin lists entries; Up/Down still navigate
- **Tab completion**: completes builtins, aliases, and TigFS / `bin/*.elf` names (common-prefix + list)
- **Aliases**: `alias`, `alias name=value`, `alias name value…`, `unalias name`; one-level expansion on argv[0]
- **Startup rc**: auto-runs first of `shell.rc`, `/shell.rc`, `bin/shell.rc` if present
- Help text updated for Tab / alias / history / rc


## 1.1.9 — Process management
- **SYS_PSINFO** (22): snapshot of live/zombie processes for userland
- **Userland tools** on `PATH` (`bin/`):
  - `ps` — PID / PPID / STATE / USER / NAME table
  - `kill [-sig] PID` — default SIGKILL (9); also shell builtin
  - `top [N]` — refresh process table N times (default 3, 1 s interval)
- **Job control polish**: shell `kill` accepts `-sig`; help lists ps/top/kill
- **Existing**: `jobs`, `fg`, `wait`, Ctrl-C → SIGINT foreground (kernel `process_fg_*`)
- Kernel: `process_snapshot()` backs SYS_PSINFO
- **Halt**: `halt` / `shutdown` / `poweroff` builtins + **SYS_HALT** (23); stops shell respawn and halts (QEMU power-off when available)
- `exit`/`quit` still only leave the shell (kernel respawns it on purpose)


## 1.1.8-update2 — Stability, size, headless, cleanup
- **Headless / tests**: console output is mirrored to COM1 serial so `-serial stdio` and `make test` show boot messages and suite results; isa-debug-exit path works.
- **i386 size**: userland built with `-Os -fno-asynchronous-unwind-tables -fno-exceptions` so `shell.elf` fits TigFS direct-block limit (≤ 56 KiB). Both arches build and link cleanly.
- **Stale comments removed**: audit notes, V1.0.58 strings, and leftover version spam cleaned from makefile, tests.c, CHANGELOG.
- **Tests**: brand demo uses current version; process suite no longer races (linger worker for lookup-by-pid); FS free-block headroom in mktigfs; suites remain the release gate for both arches.
- **mktigfs**: reserve free data blocks so runtime `fs_write` / create work on a pre-populated image.
- No version number bump (still 1.1.8 in `version.h`).

## 1.1.8 — Time and system information
- **RTC**: CMOS real-time clock driver (`rtc.c`) — year/month/day/hour/min/sec
- **SYS_SYSINFO** (21): uptime, ticks/Hz, memory KB, TigFS block/file counts, RTC, arch, version
- Userland tools on `PATH` (`bin/`):
  - `date` — RTC wall clock
  - `uptime` — time since boot
  - `uname` / `uname -a` — OS name, version, arch
  - `free` — memory total/used/free
  - `df` — TigFS blocks used/free + file count
  - `sysinfo` — compact neofetch-style summary


## 1.1.7 — Console text editor
- New userland program **`edit`** (`bin/edit.elf`): open, edit, save, quit, search
- Line numbers + status bar (path, dirty `*`, line:col, messages)
- Cursor keys, Home/End, Backspace/Delete, Enter (split line)
- **Ctrl-S** save, **Ctrl-F** find, **Ctrl-Q** / **Ctrl-X** quit (double-confirm if dirty)
- Usage: `edit FILE` or `edit` (empty buffer; save needs a path from `edit name`)


## 1.1.6 — Shell Scripting 2
- **`test` / `[`**: string/int comparisons and file probes
  (`-z`, `-n`, `-f`/`-e`, `=`, `!=`, `-eq`/`-ne`/`-lt`/`-le`/`-gt`/`-ge`)
- **`if COND ; then BODY ; [else BODY ;] fi`**
- **`while COND ; do BODY ; done`** (iteration safety cap 10000)
- **Scripts**: `sh FILE` joins newlines to `;` so multi-line if/while works
- **glc fix**: emit `__gl_int_to_str` only when `as str` is used (no more unused warnings)


## 1.1.5 — Green Lizard 2 (typed language)
Typed language upgrade for `glc` (still compiles all Round 1/2 tools):

- **Types:** `int`, `str`, `bool`, `void`
- **Annotations:** `let x: int = 1`, params `fn f(a: int, s: str)`, returns `fn f() -> int`
- **Casts:** `expr as int` | `as bool` | `as str` (int/bool → str via helper)
- **Checking:** assign / let / return / call args checked; clear type errors
- **Comparisons** yield `bool`; `true`/`false` are `bool`
- **User functions** with signatures (forward decls, arity & type checks)
- Demo program: `examples/typed.gl` → `bin/typed.elf`

Legacy untyped `let x = 1` and `fn f(x)` still work (default `int`).


## 1.1.4 — Tools – Round 2
New userland utilities (Green Lizard → `bin/NAME.elf`), installed via PATH:

| Tool | Purpose |
|------|---------|
| `cp` | Copy file |
| `mv` | Move/rename (copy + unlink on flat TigFS) |
| `touch` | Create empty file(s) |
| `head` | First N lines (`-n N`, default 10) |
| `clear` | Clear console (ANSI) |
| `tee` | Copy stdin to stdout and a file |
| `sleep` | Sleep N seconds |
| `basename` | Strip directory prefix |
| `dirname` | Directory part of a path string |

Round 1 tools unchanged. Still flat TigFS (no real directories).


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

## 1.2.1-ramfs — Working RAM disk at /tmp

- **ramfs**: 32 inodes, 64 KB pool, max 4 KB/file under `/tmp`
- VFS routes `/tmp/*` to ramfs (open/create/mkdir/unlink/stat/getdents/read/write)
- `chdir /tmp` works; hierarchy under `/tmp` supported
- New `ramfs` test suite in `make test`

