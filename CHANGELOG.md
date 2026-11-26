# Changelog

All notable changes to TigxOS are documented here.

## 1.3.8 — Full AP scheduling, process groups, devices, polish

### SMP
- **Application processors run the scheduler.** After GDT/IDT load, each AP enables a LAPIC timer (vector 32) and enters `schedule()` / idle `hlt`, same model as the BSP.
- Reschedule and TLB IPIs are delivered to all online CPUs (no longer skipped as “parked”).
- `process_pick_cpu()` chooses the least-loaded online CPU using run-queue depth hints.

### Process model
- **Process group vs thread:** `struct proc_group` owns address space, file descriptors, cwd, and heap; `struct process` is the scheduling entity (stack, state, TLS).
- `process_fds` / `process_aspace` / `process_cwd` prefer the group so threads share one process view.

### Devices
- **AHCI** registers as `ahci0` and becomes the default block device when a HBA + active port is present; TigFS mounts through the block layer. Use `make run-ahci` under QEMU.
- **HDA** programs stream descriptor 0 (BDL + RUN) for PCM tone playback when a controller is present.
- **MSI:** `pci_route_msi(dev, cpu, handler)` targets a logical CPU via MADT APIC ID; AHCI tries MSI on init.

### Memory / build
- DMA helper `dma_alloc` / `dma_free` for contiguous low frames; 4 GiB check is x86_64-only (fixes i386 `-Wtype-limits`).
- Kernel defaults to `-Os` to stay under the 511-sector bootloader limit.
- `.gitignore` drops build products and QEMU/debug logs.

### Also included in this series
- HPET base from ACPI HPET table (fallback `0xFED00000`).
- USB HID typematic key repeat + Shift map.
- Green Lizard `gl_thread_*` helpers; mutex lock-order warnings.

### Honest limits
- i386 remains single-CPU (by design).
- Real-hardware AHCI needs a machine with AHCI; QEMU `make run-ahci` exercises the software path.
- FAT32 remains next on the storage roadmap.

## 1.3.0–1.3.7 — Hardware layer (summary)

- HAL + PCI (incl. MSI caps), framebuffer, PS/2 mouse, ATA/block/partition/mount,
  xHCI HID/MSC hooks, HPET/audio detect, IOAPIC discovery, SMP test targets.
- Process-group foundations, USB MSC BOT, richer `ps`/`top`.

## 1.2.x — Kernel infrastructure (summary)

- VFS + TigFS v6 + ramfs; memory hardening; scheduler/SMP foundation (APIC, SIPI,
  IPI, TLB); kthreads and user `clone`/futex ABI; syscall ABI v1.

## 1.1.x — Foundations (summary)

- Shell, PATH/cwd, TigFS, Green Lizard, editor, process tools — freeze at 1.1.12.

