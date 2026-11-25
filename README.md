# TigxOS

**A dual-architecture hobby operating system** — thin kernel in C, userland in C and [Green Lizard](lizard/README.md).

[![Version](https://img.shields.io/badge/version-1.3.8-blue)](CHANGELOG.md)
[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)
[![Arch](https://img.shields.io/badge/arch-i386%20%7C%20x86__64-lightgrey)](#build)

Boots on **i386** and **x86_64**, runs a concurrent shell on **TigFS**, and ships a small native language (**Green Lizard**) that compiles to ELF.

### Current release — 1.3.8
- Kernel size headroom (`-Os` by default, under 511-sector boot limit)
- HPET base from the ACPI HPET table (fallback `0xFED00000`)
- DMA-safe contiguous allocator (`dma_alloc` / `dma_free`)
- USB HID typematic key repeat + Shift map
- Green Lizard thread helpers (`gl_thread_spawn` / `join` / `exit` / `self`)
- Mutex lock-order deadlock warnings
- SMP: APs online and parked stable; full AP scheduling still later



```
  User programs  ·  Shell  ·  Green Lizard
 ─────────────────────────────────────────
  Syscalls · VFS · Pipes · Jobs · PATH/cwd
 ─────────────────────────────────────────
  Kernel (C) · Scheduler · Memory · SMP prep
 ─────────────────────────────────────────
  arch/i386  ·  arch/x86_64  ·  boot
```

## Features

- **USB xHCI** — host controller init + port scan (1.3.4)
 (1.2.9)

- **Dual-arch kernel** — same source tree for i386 and x86_64  
- **TigFS v6** — hierarchical filesystem + **VFS** (`/`, `/tmp` ramfs)  
- **Processes** — preemptive scheduling, priorities, pipes, wait/kill  
- **SMP foundation** — ACPI MADT, LAPIC, AP bring-up; IPI/TLB/steal hooks (x86_64). i386 stays single-CPU by design  
- **Shell** — PATH, cwd, scripting (`if` / `while` / `test`)  
- **Tools** — editor, `dmesg`, date/uptime/free/df/sysinfo, ps/top, classic utils  
- **Tests** — `make test` runs boot-time suites on both architectures  

## Quick start

**Requirements:** `gcc`, `nasm`, `python3`, `qemu-system-x86` (or `qemu-system-i386`).

```bash
git clone https://github.com/TigX-Developer/TigXOS.git
cd TigXOS
make            # x86_64
make run        # QEMU
```

```bash
make ARCH=i386        # 32-bit build
make run-32
make test             # both arches, headless suites
```

## Layout

| Path | Purpose |
|------|---------|
| `arch/` | Boot, entry, interrupt stubs per architecture |
| `kernel` sources (`*.c`) | Memory, processes, VFS, syscalls, SMP, … |
| `user/` | Shell, libc, utilities |
| `examples/*.gl` | Green Lizard sample programs |
| `lizard/` | Green Lizard compiler (`glc`) |
| `tools/` | Disk/image builders (`mktigfs`, `mkosimage`) |

## Documentation

- [CHANGELOG.md](CHANGELOG.md) — what changed per version  
- [FOUNDATIONS.md](FOUNDATIONS.md) — design notes for the 1.1.x series  
- [LOCKING.md](LOCKING.md) — lock ordering (SMP prep)  
- [SHELL.md](SHELL.md) — shell behaviour  
- [lizard/README.md](lizard/README.md) — Green Lizard language  

## License

[MIT](LICENSE) — free to use, modify, and distribute with attribution.

## Status

Educational / hobby project. The public roadmap targets a full desktop-style OS (graphics, networking, installers). Expect rapid change; tags mark known-good points.

**Current tag focus:** kernel infrastructure through **1.2.9** (memory hardening, VFS, logging, SMP foundation).
