# TigxOS changelog

## 1.3.8 — Carry-over audit + DMA allocator

Built on the 1.3.7-update4 baseline (all 333 checks green, SMP 2/4 park-stable).

### Kernel image headroom
- Kernel default `OPT=-Os` so the image stays under the 511-sector bootloader limit.
- Override with `make OPT=` if you need unoptimised builds for debugging.

### HPET via ACPI
- Parse the ACPI **HPET** table for the MMIO base; fall back to `0xFED00000` only when the table is absent.
- i386 still skips HPET (MMIO outside the identity window).

### DMA-safe allocator
- New `dma_alloc` / `dma_free` / `dma_alloc_pages` / `dma_free_pages` in `memory.c`.
- Contiguous physical frames, preferred below 4 GiB for 32-bit bus masters.
- AC97/HDA CORB/RIRB and PCM buffers allocate through this path.

### USB HID keyboards
- Typematic **key repeat** (~400 ms delay, ~50 ms rate) in `usb_hid_poll`.
- Shift map already present; repeat uses the active shift state.

### Green Lizard thread bindings
- `gl_thread_spawn` / `gl_thread_join` / `gl_thread_exit` / `gl_thread_self` in libc
  (alongside the existing channel API from 1.3.7).

### Deadlock detector
- Beyond recursive-mutex refusal: tracks a small per-task lock stack and warns on
  **lock-order inversion** between pairs of mutexes.

### Honest limits (not claimed done)
- Full process vs thread struct split remains incremental (scheduler entity is still `struct process` with group metadata).
- AHCI as root disk on real hardware needs real-HW bring-up (1.3.12).
- HDA stream DMA for full PCM is still partial; AC97 PCM path plays square-wave tones.
- Per-core device interrupt routing is prepared by IOAPIC discovery; full MSI routing stays for GPU/Wi-Fi work.


## 1.3.7-update4 — AP fully silent + pin tasks to BSP

Invalid Opcode on idle-1 at RIP≈0xf continued after update3 because:
1. AP still did `sti;hlt` and could take IPIs
2. `process_pick_cpu()` assigned new tasks to CPU 1 and sent resched IPIs

Fix:
- AP park is **`cli;hlt` only** (no interrupts)
- `process_pick_cpu()` always returns **0** (BSP)
- No resched/TLB IPIs to APs while parked
- Scheduler will not steal AP-bound tasks


## 1.3.7-update3 — AP schedule Invalid Opcode fix

After GDT/IDT load, AP still called `schedule()` while `idle-N` had
`rsp=NULL`. `context_switch` then resumed garbage → **Invalid Opcode**
at RIP≈0xf (pid idle-1). Fix: park AP on `sti;hlt` only; allocate a real
idle stack + `ap_idle_park` frame for a later full multicore scheduler.


## 1.3.7-update2 — SMP AP triple-fault fix

AP `ap_main` never loaded the kernel GDT or IDT after the trampoline.
QEMU showed `IDT=0` → GPF on first interrupt after `sti` → double fault →
triple fault / CPU Reset. Fix: `cli` → `gdt_load_ap()` → `idt_load()` before
LAPIC / schedule / `sti`.


## 1.3.7-update8 — `make test-smp` really runs the tests

- `make test-smp` / `test-smp4` built a *normal* image (no `-DTIGX_BOOT_TESTS`), so QEMU just
  booted to the shell and waited until the timeout: the target-specific `TESTS=1` is not seen by
  the makefile's parse-time checks. They now call `$(MAKE) ARCH=x86_64 TESTS=1 ...-run`.
- Result of update7 confirmed: with 2 CPUs the machine boots, AP "idle-1" runs, shell starts. The
  reboot loop is gone.
- Seen in that log, not fixed: with `-smp 2` the E820 table showed 7 entries (6 real + one
  zero-length) and "Total RAM detected: 12415 MB" — the total counts garbage/reserved ranges.

## 1.3.7-update7 — AP (second CPU) crash-safety, SMP test diagnostics

- **`make test` x86_64 `-smp 2` reboot loop**: the machine kept resetting right after
  "2 CPU(s) online". Cause found by reading the code: a started AP stayed on the low-memory
  trampoline GDT and **never loaded an IDT**, so *any* interrupt or exception on that CPU was a
  triple fault, and QEMU resets the whole VM on that. (`make test` only began running `-smp 2/4`
  in 1.3.6, so this had never been exercised.)
- `ap_main` now first calls new `gdt_load_ap()` (kernel GDT, no TSS) and `idt_load()` (the BSP's
  IDT, now actually defined in interrupt.c). A fault on an AP now shows the panic screen
  instead of silently rebooting.
- **Still not done:** per-CPU TSS (APs are ring-0 only) and proof that user processes can run on
  an AP. If the SMP suite still fails, the new panic screen or the debug log will say where.
- `test-smp` / `test-smp4`: run QEMU with `-no-reboot -d cpu_reset,guest_errors -D smp2-debug.log`
  (`smp4-debug.log`). A crash now stops QEMU (no more 10,000-line reboot loops) and the log holds
  the CPU registers at the moment of the fault.
- Size: with `TESTS=1` the x86_64 kernel is 259,188 of 261,632 bytes (99%). Build with `OPT=-Os`.

## 1.3.7-update5 — test timeout + kernel `OPT=` switch

- **i386 `make test` TIMEOUT** (stopped after the ramfs "open /tmp file" check): `vfs.c`/`ramfs.c` are
  byte-identical to 1.3.3, so this is not a ramfs bug. The 45 s limit was already near the edge
  (it was 35 s before) and every version adds boot work. Limits raised to 120 s. If it still stops at
  the same line with 120 s it is a real hang: run the debug command in the notes.
- New **`OPT=`** make variable: extra flags for the kernel only (default none, nothing changes).
  `make OPT=-Os ...` builds the x86_64 kernel at 177,684 bytes instead of 257,492 (-31%).
  A different `OPT` triggers a clean rebuild (stamp now includes it).
- Known cosmetic bugs (not fixed here): the boot log prints `%zu` / `%.6s` literally and the
  memory map lines show garbage (printf lacks `%zu`, `%.Ns`, and 64-bit args are misread).

## 1.3.7-update4 — build fixes (this one compiles)

- **`xhci.c`**: two `pr_warn`/`pr_info` format strings had a real line break inside the quotes
  (the `\n` had been turned into an actual newline), so the file did not compile on x86_64 or
  i386 (`make test` failed at xhci.o). Joined back into `"...\n"`.
- **`usb.c`**: `usb_msc_register_blocks` was used before it was declared (implicit declaration,
  conflicting type) -> prototype added to `usb.h`; removed the unused duplicate
  `hid_mouse_poll_one` (mouse polling is already inside `usb_hid_poll`); silenced an unused
  parameter warning.

## 1.3.7-update1 — xHCI bulk + HDA verbs + process.h fix

### Build fix
- **`process.h`**: `struct proc_group` was inserted before `MAX_FDS` / `struct process` → compile error. Correct order restored.


### USB MSC (honest gap closed)
- **`xhci_config_bulk_eps`**: Configure Endpoint with bulk OUT/IN transfer rings
- **`xhci_bulk`**: Normal TRB + doorbell + transfer event wait
- Config descriptor **endpoint parse** → DCI map
- MSC devices **register as block** (`usb0`) when bulk configure succeeds
- BOT CBW/CSW path uses real DCI numbers

### HDA
- Map BAR0, GCTL reset, **CORB/RIRB** run
- Codec verbs: AFG power D0, amp unmute on nodes 0x02–0x0A
- PCM stream DMA still speaker-backed on HDA; AC97 PCM unchanged

### AHCI
- Unchanged path: PCI → AHCI → default block when port online
- QEMU without `-device ahci` keeps ATA (expected)


## 1.3.7 — Process groups, MSC BOT, AHCI boot path, PCM, ps/top threads

### True process vs thread allocations
- `struct proc_group` table: owns AS, cwd, heap, refcount
- Leader `process_group_bind_leader`; secondary `process_group_bind_thread`
- Threads still schedule via `process_table` (thread slots); group is separately allocated

### Deadlock detector
- Mutex recursive-acquire detection (`dl_holds` / warn)

### GL thread/channel
- `gl_channel_create/send/recv` in user libc; `examples/threads.gl` stub

### ps / top
- Columns: TID, PID/TGID, TYPE (thr|proc|kern)

### USB MSC
- BOT CBW/CSW + READ(10)/WRITE(10) shape; needs `xhci_bulk` Configure Endpoint (returns -1 until bulk rings exist)

### AC97 PCM DMA
- BDL + PO DMA kick in `audio_play_tone`; speaker fallback

### Boot from AHCI
- `pci_init` → `ahci_init` → `block_init`; AHCI registers `ahci0` and `block_set_default`
- `fs` `read_lba` prefers `block_read_default` then ATA

### Honest limits
- xHCI bulk EP configure still incomplete → USB stick I/O soft-fails
- QEMU without AHCI controller keeps ATA path
- HDA codec verbs not implemented (speaker/AC97 PCM only)


## 1.3.6 — Close deferred gaps (honest status)

### Implemented
| Gap | What landed |
|-----|-------------|
| **1.2.3 process/thread** | Documented model; `tid` field; leaders set `tid=tgid=pid`. Full two-struct allocation still next |
| **1.2.8 I/O APIC** | `ioapic.c`: map MADT IOAPIC, program IRQ0–15 → vectors 32–47, mask 8259 PIC (x86_64) |
| **1.2.9 make test SMP** | `make test` runs `test-smp` (-smp 2) and `test-smp4` (-smp 4) on x86_64 |
| **1.2.12 locks** | Kernel `rwlock_*`; userland `malloc`/`free` under `mutex_t` |
| **1.3.3 AHCI** | HBA detect, port start, DMA READ/WRITE DMA EXT helpers; ATA still default boot disk |
| **1.3.4/5 USB mouse** | Boot-protocol mouse via `usb_hid_poll` → `mouse_inject` |
| **1.3.4/5 USB MSC** | Devices still counted; `usb_msc_read/write` API returns -1 until bulk EPs done |
| **1.3.5 audio** | AC97 mixer unmute when NAMBAR present; **`audio_beep`** via PC speaker |

### Still incomplete (honest)
- Physical process vs thread *allocations* (single `struct process` remains the sched entity)
- Deadlock detector, Green Lizard thread/channel bindings, ps/top full thread columns
- USB MSC bulk BOT data path
- HDA/AC97 PCM DMA playback (beep is PC speaker)
- AHCI as root disk (boot path still ATA PIO)


## 1.3.5-update2 — USB HID wired, KERNEL_SECTS=511

### Bugs fixed
1. **USB keyboard never delivered keys** — `usb_hid_poll()` was never called. Timer IRQ calls it every other tick (~50 Hz). Edge-triggered (no held-key spam) + Shift map.
2. **KERNEL_SECTS** capped at **511** (TigFS at LBA 512; max kernel 261632 bytes, matches mkosimage.py).
3. **HPET** — i386 still skipped; x86_64 uses 0xFED00000 (ACPI HPET table parse deferred).


## 1.3.5-update1 — HPET i386 page-fault fix

- **Bug:** `hpet_init` always read `0xFED00000`. On i386 that address is outside the identity map → page fault during boot tests.
- **Fix:** Skip HPET probe on i386 (same policy as LAPIC). On x86_64, `vmm_map_mmio` then CAP/CONFIG; remove unused `cfg` warning.


## 1.3.5 — USB control/HID/MSC + HPET + audio detect

### USB (completes 1.3.4 path)
- **Control transfers** on xHCI (Setup/Data/Status TRBs)
- **Enable Slot** + **Address Device** with input contexts
- **GET_DESCRIPTOR** (device + config) → class/VID/PID
- **HID boot keyboard**: SET_CONFIGURATION / SET_PROTOCOL / SET_IDLE; `usb_hid_poll` + `keyboard_inject_char`
- **MSC**: devices counted after classify (BOT data path next)

### HPET
- Probe **0xFED00000**, enable main counter, `hpet_read_counter` / `hpet_udelay`

### Audio
- PCI scan for **AC97** (04:01) and **HDA** (04:03); log backend name (PCM DMA later)

### Build
- `KERNEL_SECTS=640`, objects: `hpet.o` `audio.o`

### QEMU tips
```text
-device qemu-xhci -device usb-kbd
-device AC97   # or intel-hda
```


## 1.3.4-update1 — xhci BAR mapping safety, kernel sector limit

- `xhci.c` `map_bar`: if the BAR cannot be mapped the controller is now **skipped** instead of
  being used anyway. Before, a failed map was only a warning, and the next register read would
  page-fault and panic the kernel. On i386 (identity map 0-1 GB only) any BAR above 1 GB is
  skipped; QEMU/VirtualBox put xHCI BARs near 0xFEB00000, so i386 with a USB controller would
  have crashed at boot.
- `KERNEL_SECTS` 576 -> 511: LBA 512 is the TigFS superblock, so 511 is the true maximum and the
  size check in `make` now matches what `mkosimage.py` enforces.

## 1.3.4 — USB xHCI foundation

### Features
- **xHCI host controller driver** (`xhci.c`): PCI discover (class 0C/03/30), BAR0 MMIO, HCRST, DCBAA + command/event rings, Run bit
- **Root-port scan**: CCS / speed / port reset; devices recorded in the USB table
- **USB glue** (`usb.c` / `usb.h`): controller/device queries, `usb_dump`, HID/MSC hooks (stubs until control transfers)
- **Boot**: `USB...` after PCI; messages in `dmesg`
- **Tests**: `test_suite_usb` (safe without a controller)

### How to exercise
```text
make test                          # no xHCI required
make && qemu-system-x86_64 -m 128 -device qemu-xhci \
  -drive file=os-image.bin,format=raw,if=ide,index=0,media=disk
# dmesg / boot log: xhci: ... controller running
```

### Honest limits
- Control transfers / Address Device / HID interrupt / MSC BOT not done yet — PS/2 keyboard & mouse stay primary
- Storage still ATA block layer; USB MSC registers 0 devices until BOT lands
- i386 builds the same code; MMIO mapping is best-effort in the identity window


## 1.3.3-update2 — kernel static memory moved out of the video-memory hole

### Why
After the block layer arrived, the kernel's zero-initialised data (`.bss`, ~396 KB) ran from
0x48240 up to 0xA8FE0. That crosses 0x9FC00 (BIOS data area) and **0xA0000 (VGA video memory)**.
Variables placed there (`lapic_regs`, `ap_stacks`, SMP counters, ...) were not in real RAM, so
reads could return something other than what was written (VGA decodes that window itself).
It happened to work before only because `.bss` ended below the line.

### Fix
- `linker.ld` and `arch/i386/linker.ld`: `.bss` now starts at 1 MB (0x100000).
  Frames below 8 MB are already reserved (`RESERVED_END` in memory.c), so the page allocator
  never hands them out. `kernel.bin` size is unchanged (bss is not stored in the image).

### Notes
- Real maximum kernel image is 511 sectors (LBA 512 is the TigFS superblock); `mkosimage.py`
  already enforces this, so `KERNEL_SECTS=512` only matters for the loader's read count.
- Not yet done: explicit zeroing of `.bss` at boot (relies on RAM being zero, true in QEMU).

## 1.3.3-update1 — kernel size + partition parser warnings

### Build
- **`KERNEL_SECTS` 448 → 512** (256 KiB). x86_64 `kernel.bin` had grown past 229376 bytes (block/part/mount/HAL stack), so the image check failed with `exceeds bootloader KERNEL_SECTS`.

### Warnings
- **`part.c` MBR name path**: removed empty `if (d) ;` body (`-Wempty-body`)
- **`part.c` GPT names**: copy `name_utf16` out of the packed entry before `utf16_to_ascii` (`-Waddress-of-packed-member`)


## 1.3.3 — Block layer, partitions (MBR/GPT), mount table

### Features
- **Block layer** (`block.h` / `block.c`): device registry, ATA backend wrapper,
  32-slot sector cache, `blk_read` / `blk_write`.
- **Partitions** (`part.h` / `part.c`): MBR (4 primary) and GPT parsing, whole-disk
  fallback when no table is present (TigFS image layout).
- **Mount table** (`mount.h` / `mount.c`): multiple mounts, longest-prefix lookup.
  Boot seeds `/` → TigFS and `/tmp` → ramfs.
- Existing ATA PIO driver remains the storage backend. AHCI is deferred to a
  later driver pass (PCI class is already visible via 1.3.0).
- Version **1.3.3**.

### Notes
- TigFS still talks to `disk_read`/`disk_write` directly; the block layer is the
  path new filesystems (FAT32/NTFS) and partition-relative I/O will use.
- Protective MBR + GPT is detected; MBR type 0xEE does not create fake MBR
  partitions.
- No format/partition editor yet — that comes with the installer (1.10.x).

### Files added
- `block.h`, `block.c`, `part.h`, `part.c`, `mount.h`, `mount.c`

---

## 1.3.2 — PS/2 mouse

### Features
- **`mouse.h` / `mouse.c`**: standard PS/2 mouse over the 8042 auxiliary port.
  - Reset, stream mode, enable reporting.
  - IRQ12 handler, 3-byte packets, button mask, clamped X/Y position.
  - `mouse_get_state()`, `mouse_poll()`, `mouse_set_bounds()`.
  - Bounds follow the active framebuffer when present, else 1024×768 virtual.
- Wired after `fb_init` in `kernel.c`. Version **1.3.2**.

### Notes
- Shares ports 0x60/0x64 with the keyboard; mouse commands use the 0xD4 prefix.
- IRQ2 (cascade) is enabled so IRQ12 can fire.
- No GUI cursor yet — that arrives with the compositor (1.4 / 1.5). This layer
  only delivers coordinates and buttons for later use.
- QEMU: use `-device usb-tablet` or the default PS/2 mouse; absolute tablet is
  not required for this driver.

### Files added
- `mouse.h`, `mouse.c`

---

## 1.3.1 — Linear framebuffer (VBE / BGA)

### Features
- **`fb.h` / `fb.c`**: linear framebuffer abstraction.
  - Boot VBE handoff at `0x9800` (`ARCH_BOOT_FB_INFO`, magic `TIGF`) when
    boot.asm has already set a mode (real hardware path).
  - **Bochs/QEMU VBE DISPI (BGA)** is probed at boot but **not enabled**, so the
    VGA text console stays visible. Call `fb_enable_bga(w,h,bpp)` when you want
    graphics (compositor / tests). Maps the high LFB with `vmm_map_mmio`.
  - Drawing: `fb_clear`, `fb_put_pixel`, `fb_fill_rect`, `fb_hline`, `fb_vline`.
  - Graceful no-op when inactive (i386 / no BGA stays text mode).
- Wired after PCI in `kernel.c`. Version **1.3.1**.

### Notes
- **Important:** enabling BGA switches the display out of text mode. That is
  why auto-enable was removed — otherwise the shell and `make test` output
  disappear and the screen looks black.
- GUI renderer (1.4.0+) will call `fb_enable_bga` when it starts.
- i386: BGA LFB is above 1 GB → enable fails, text mode remains (roadmap: GUI
  is x86_64-only).

### Files added
- `fb.h`, `fb.c`
- `ARCH_BOOT_FB_INFO` in `arch/arch.h`

---


## 1.3.0 — HAL + PCI enumeration (MSI / MSI-X)

### Features
- **HAL** (`hal.h` / `hal.c`): machine identity (arch, vendor/brand, CPU count,
  APIC/MSI presence), thin device registry, MSI vector allocation that uses the
  existing LAPIC from 1.2.8.
- **PCI** (`pci.h` / `pci.c`): full config-space access (0xCF8/0xCFC), bus 0–7
  scan, multi-function devices, Type-0 BAR decode (I/O + 32/64-bit memory),
  capability walk (MSI, MSI-X, PCIe), `pci_enable_device`, single-message MSI
  and first MSI-X table entry helpers.
- Wired into `kernel.c` after SMP bring-up; objects added to `KERNEL_OBJS`.
- Version bumped to **1.3.0** (`version.h` is source of truth).

### Notes
- Thin-kernel rule kept: no driver policy inside the HAL.
- i386 still builds and boots; PCI scan works, MSI is gated on APIC presence.
- Ready for 1.3.1 (linear framebuffer) and 1.3.3 (AHCI) which need this layer.
- Debug: `pci_dump_all()`, `hal_dump_machine()`, `hal_dump_devices()`.

### Files added
- `hal.h`, `hal.c`, `pci.h`, `pci.c`

---


## 1.2.12-update3 — i386 regression crash: stale wait-queue registry entries

### What broke
Same panic after update2: `Invalid Opcode`, `pid=3 name="reg_b"`, `EIP=0x00000003`, with
**identical register values** in both runs. Identical values mean it was not a timing
race, so the update2 "half-built process" theory was not the cause. (The irq-off change
in `process_create*` stays as harmless hardening.)

### Why
`wait_queue_init` keeps every queue's address in a global registry so a dying process
can be removed from every queue (`wait_queue_remove_pid`). Many queues live on **stacks or
heap blocks that are freed and reused** (kernel mutexes/semaphores created inside test
functions, etc.). Their registry entries were never removed.

Update1 grew the registry from 32 to 128 slots (to stop the futex "registry full" spam). With
the old size the registry filled up early, so later dangling queues were simply refused. At 128
they were all accepted. When `reg_a` exited, `wait_queue_remove_pid` walked those dangling
pointers, read garbage as `count`/`waiters`, found "pid" matches and `remove_at()` **shifted
words and wrote -1 into memory that now belonged to something else**, here the freshly
allocated kernel stack of `reg_b`. The first word `reg_b` returns into (its entry address) was
overwritten, so it "returned" to address 3 (its pid) on its first instruction.

### Fix
- `struct wait_queue` has a `magic` field set by `wait_queue_init`.
- `wait_queue_remove_pid` only touches a registered queue that still has the magic and a sane
  `count` (0..WAIT_QUEUE_MAX). Stale entries are skipped, never written to.
- When the registry is full it first **compacts** (drops stale entries), so long-lived systems
  do not leak slots.

### Notes
- In-OS version stays **1.2.12**.
- Proper follow-up (1.2.13): add `wait_queue_destroy()` and call it from `mutex_destroy` / `sem_destroy`,
  or track the queue a process is blocked on in `struct process` so there is no global registry.

## 1.2.12-update2 — i386 regression-suite crash (half-built process scheduled)

### What broke
After update1, `make test` on i386 passed every suite up to `regression`, then
panicked: `Invalid Opcode`, `pid=3 name="reg_b"`, `EIP=0x00000003`.

### Why
`process_create` (and the user/thread variants) marked the new process
`PROC_READY` **before** its kernel stack was allocated and its first stack frame was
built. Allocating and zeroing a 16 KB stack takes a while, and process_create ran with
interrupts on. If the timer tick landed in that window, the scheduler picked the
half-built process and switched to a stack with no valid frame, so it "returned" to a
garbage address (EIP=3). It is a timing race: it only fails when the tick lands in
that window, which is why earlier runs happened to pass and any layout/timing change
could expose it.

### Fix
- `process_create`, `process_create_user`, `process_create_user_at` and
  `process_thread_create` now run with interrupts off (`irq_save` / `irq_restore` on
  every return path) from slot selection until the process is fully built.

### Notes
- In-OS version stays **1.2.12**.
- Still open for 1.2.13+/SMP: on several CPUs the slot pick needs a real lock (another
  CPU could still see a half-built slot); mark the process READY as the **last** step.

## 1.2.12-update1 — x86_64 test crash (boot stack overlapped the log buffer)

### What broke
`make test` on x86_64 stopped in the `syscall` suite with a kernel fault whose
registers were full of ASCII text (`RBX=0x73657274` is "tres"), then `System halted`
and `RESULT ARCH=x86_64 TIMEOUT`. i386 passed.

### Why
The boot/idle stack was hard-coded at **0x90000** (`mov rsp, 0x90000`). The kernel's
own `.bss` (zero-filled data: tables, buffers) grew in 1.2.12 (futex keys) and the
dmesg `log_buf` moved to 0x8c1a0–0x901a0, so it sat **on top of the stack**. Once
the log wrapped, log text overwrote the saved registers/return addresses of the
idle task, and the next context switch jumped into text.

### Fix
1. **The boot stack is now a reserved `.bss` object** (`boot_stack_top`, 16 KB) in
   both `arch/*/entry.asm`. `process.c` (idle task, TSS fallback) and `gdt.c`
   (initial `tss.esp0/rsp0`) use it instead of the magic 0x90000. It can no longer
   be overlapped by anything the linker places.
2. **Wait-queue registry 32 → 128.** `futex_init` creates 64 queues, which flooded the
   boot log with `wait: registry full (32), 'futex' not tracked` and left futex
   waiters out of exit cleanup (a killed process could stay on a futex queue).
3. **`process_load_tls`** now ignores kernel tasks and non-canonical addresses
   (WRMSR on a non-canonical FS base raises #GP in ring 0). Defense only; it was
   not the root cause.
4. **`KERNEL_SECTS` 416 → 448.** The kernel was at 95% of the loader limit
   (203,008 of 212,992 bytes).

### Notes
- In-OS version stays **1.2.12**.
- Lesson: never hard-code a stack address inside memory the kernel image can grow into.

## 1.2.12 — Thread synchronization + libc

### Features
- **`SYS_FUTEX` (34)** — `FUTEX_WAIT` / `FUTEX_WAKE` on user-aligned ints, hashed wait queues
- **User libc**: `thread_create` / `thread_join` / `thread_exit` / `gettid` / `futex`
- **`mutex_t`**: userspace mutex via CAS + futex
- **`cond_t`**: condition variable (wait / signal / broadcast)
- **`errno`** symbol for libc (process-scoped for now; TLS-backed later)
- Kernel **mutex/semaphore** unchanged and still covered by tests

### Tests
- New **`futex`** suite: ABI numbers, rejection of kernel/misaligned addresses
- `SYS_MAX` updated to 34

### Notes
- Futex timeout is infinite in this release (3-arg syscall); timeout arg can land later
- i386 builds and boots; parallel threads remain an x86_64 focus per roadmap
- Next after 1.2.x freeze: **1.3.0 HAL + PCI**

## 1.2.11-update4 — user-thread correctness

### Fixes
1. **`exit_thread_only` is per-task**, not a global. `process_thread_exit` set a
   global flag then called `process_exit` (which never returns), so the clear
   never ran and every later `process_exit` skipped group kill.
2. **Address-space refcount** (`aspace_ref` / `aspace_unref`). Threads share the
   leader’s AS; the last exit destroys it. A lone `SYS_THREAD_EXIT` of the leader
   no longer frees memory under remaining threads.
3. **User-pointer checks** on `SYS_CLONE` (entry/stack in user window) and
   `SYS_THREAD_JOIN` (`status*` must be writable user memory).
4. **`sched_lock`** around thread create, join generation capture, and group kill.
5. **`thr_gen` generation** on each thread create so join cannot attach to a
   reused process-table slot.

### Tests
- `uthread` suite checks `exit_thread_only` and AS ref helpers.

## 1.2.11-update3 — sched test hang

### Fix
- **sched fairness**: do **not** lower the test-runner below worker priority.
  That starved `timer_sleep_ms` forever under strict priority and caused
  `RESULT ARCH=i386 TIMEOUT` after "spawned two equal-prio workers".
- Prove concurrency with **spin counters** (hard check); keep `ticks_ran` soft.
- `make test-qemu` timeout 35s → 45s.

## 1.2.11-update2 — sched fairness test (x86_64)

### Fix
- **sched suite**: equal-prio workers now do short busy work (not pure yield),
  the test-runner drops to `PRIO_MIN` while they run, and "both workers
  accrued ticks" is a **soft** check. Under host load a pure-yield loop could
  miss timer samples and hard-fail `make test` on x86_64.

## 1.2.11-update1 — kthread create race (i386)

### Fix
- **`kthread_create`**: install `kthread_fn` / `is_kthread` under `irq_save` so a
  timer tick cannot run the trampoline before the entry pointer is set.
  Without this, i386 `make test` saw `kthread alive` / `join status 42` / `body
  ran` fail (trampoline exited with 0).
- Silence unused `ap_main` on i386 (`-Wunused-function`).

### Notes
- In-OS version remains **1.2.11**. Tarball: `TigxOS-v1.2.11-update1.tar.gz`.

> **Versioning:** in-OS string is the release (`1.2.11`). Tarball `-updateN` is for pure
> bugfixes; Git tags are `v1.2.11`.

## 1.2.11 — User threads

### Features
- **`process_thread_create(entry, stack)`** — new user thread in the same address
  space (shared `as` + open-file refs). `stack == 0` maps a **guarded** per-thread
  user stack below the main stack.
- **`process_thread_exit` / `process_thread_join` / `process_group_kill`**
- **`process_exit` kills the whole thread group** (POSIX-style); `thread_exit` is
  single-thread only.
- **TLS:** `tls_base` per thread; `process_load_tls` on context switch (FS base via
  `WRMSR` on x86_64).
- **Syscalls (ABI v1, appended):** `SYS_CLONE` (29), `SYS_THREAD_EXIT` (30),
  `SYS_THREAD_JOIN` (31), `SYS_SET_TLS` (32), `SYS_GETTID` (33)

### Tests
- New **`uthread`** suite: ABI numbers, clone refusal in kernel context, TLS store,
  group_kill / join error paths

### Notes
- Secondary threads do **not** destroy the shared address space on finalize
- Full ring-3 clone stress is available via `SYS_CLONE` from user programs
- Next: **1.2.12** thread sync + thread-safe libc

> **Versioning:** in-OS string is always the release (`1.2.10`). Tarball names
> may use `-updateN` for pure bugfix packages; Git tags are `v1.2.10`.

## 1.2.10 — Kernel threads

### Features
- **`kthread_create(name, entry, arg)`** — kernel thread with own stack, shared
  kernel address space, joinable by default
- **`kthread_join` / `kthread_detach` / `kthread_exit` / `kthread_alive`**
- Trampoline delivers `void *arg` to `entry(void *)`
- Joinable DEAD kthreads are **not auto-reaped** until join/detach (no lost status)
- Exit wakes a registered joiner

### Tests
- New **`kthread`** suite: create/join status, arg passing, detach, 4-way concurrent
  join, error paths

### Notes
- Built on the existing scheduler (`process_table` slots with `is_kthread`)
- User threads (`clone`) arrive in **1.2.11**

> **Versioning:** the in-OS string (`version.h` / banner / `TIGX_VERSION`) is
> always **1.2.9** for this series. Bugfix/tarball names use
> `TigxOS-v1.2.9-updateN.tar.gz`; Git tags are `v1.2.9`.

## 1.2.9 (update4 tarball) — true multicore scheduling + load balance + IPI ack

### Scheduling
- **Per-CPU idle fallback** in `pick_next` — APs no longer share `process_table[0]`
- **`sched_lock`** held across pick_next / state transition (released before context_switch)
- **Load-balance on create**: new tasks get `process_pick_cpu()` (least-loaded) and
  remote CPUs receive **resched IPI**
- **Affinity API**: `process_set_affinity` / `process_get_affinity` / `process_pick_cpu`
- Steal prefers the **busiest remote CPU** (rq depth); never steals idle

### IPI
- Resched IPI tracks **ACK generation** with bounded wait (no hang if AP offline)
- TLB shootdown ACK path retained

### LAPIC
- `lapic_ap_init` enables the local APIC on application processors
- Timer helpers present (vector wiring optional)

### Tests
- Multicore workers with affinity + progress counters
- Soft check for work on ≥2 CPUs when `-smp 2`
- IPI resched/TLB stress loops
- `make test-smp` runs `TESTS=1` with `-smp 2` and isa-debug-exit

### Notes
- i386 remains UP
- `make test` still uses `-smp 1` for deterministic CI; use `make test-smp` for APs

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

