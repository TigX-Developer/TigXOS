# TigxOS

**A dual-architecture hobby OS — thin kernel in C, userland in C and Green Lizard.**

[![Version](https://img.shields.io/badge/version-1.2.1-blue)](CHANGELOG.md)
[![Arch](https://img.shields.io/badge/arch-i386%20%7C%20x86__64-green)](#build)
[![Lang](https://img.shields.io/badge/kernel-C-orange)](#languages)
[![Lang](https://img.shields.io/badge/userland-C%20%2B%20Green%20Lizard-purple)](#languages)

TigxOS is an educational operating system that boots on **i386** and **x86_64**,
runs a concurrent shell, hosts programs on **TigFS**, and ships its own small
language — **Green Lizard** (`.gl`) — compiled down to native ELF.

```
  ┌─────────────────────────────────────────────────────────┐
  │  Shell  ·  Green Lizard programs  ·  C userland (libc)  │
  ├─────────────────────────────────────────────────────────┤
  │  syscalls · pipes · jobs · cwd / PATH · TigFS           │
  ├─────────────────────────────────────────────────────────┤
  │  Kernel (C)  ·  scheduler  ·  AS  ·  interrupts         │
  ├─────────────────────────────────────────────────────────┤
  │  arch/i386  ·  arch/x86_64  ·  boot                     │
  └─────────────────────────────────────────────────────────┘
```

---

## Languages

| Layer | Language | Notes |
|-------|----------|--------|
| **Kernel** | **C** (+ NASM for boot/entry/switch) | Freestanding, dual-arch |
| **Shell / libc / tools** | **C** | `user/shell.c`, `user/libc.c`, … |
| **User programs** | **Green Lizard** (`.gl`) and **C** | `.gl` → `glc` → C → ELF |
| **Build helpers** | Python 3 | `lizard/glc.py`, `tools/mktigfs.py` |

### Green Lizard at a glance

Easy mix of C-style structure and Python-ish ergonomics:

```gl
fn main() {
    print("hello from Tig!")
    let x = 1 + 2
    if x > 2 {
        print("yes")
    }
    exit(0)
}
```

Pipeline:

```text
examples/hello.gl  →  glc  →  hello.gl.c  →  gcc  →  bin/hello.elf  →  spawn
```

See **[lizard/README.md](lizard/README.md)** for syntax, `argc`/`argv`, and limits.

### Current series — 1.2.x Kernel infrastructure

| Version | Highlights |
|---------|------------|
| **1.2.0** | GDT/IDT, address-space, and memory audit fixes |
| **1.2.1** | **VFS** + hierarchical **TigFS v6**, ramfs at `/tmp`, expanded tests |


---

## Features (v1.2.1 — VFS + TigFS directories + ramfs)

- **Dual-arch kernel** — i386 protected mode & x86_64 long mode  
- **Processes** — address spaces, preemptive scheduling, wait / kill / ps / top  
- **Shell** — pipes, redirs, jobs, Ctrl-C, history, Tab complete, aliases, `shell.rc`, `halt`  
- **TigFS** — flat filesystem, cwd, `PATH=bin:.`, programs as `bin/NAME.elf`  
- **Green Lizard** — typed `.gl`, `int[N]` arrays, `import`, `len` / `substr`  
- **System tools** — date, uptime, uname, free, df, sysinfo, edit, ps, kill, top  
- **File tools** — ls, cat, cp, mv, rm, touch, head, tee, wc, …  
- **Tests** — `make test` headless gate on **both arches** (84 checks)

**Foundations freeze:** **[FOUNDATIONS.md](FOUNDATIONS.md)** · Shell: **[SHELL.md](SHELL.md)** · History: **[CHANGELOG.md](CHANGELOG.md)**

---

## Testing

```bash
make test              # i386 + x86_64 headless suites (release gate)
make ARCH=x86_64 run   # interactive QEMU
make size              # kernel / shell / image sizes
```

Both arches must print `ALL TESTS PASSED` and `=== both arches green ===`.
See **[FOUNDATIONS.md](FOUNDATIONS.md)** for the freeze checklist.

## Requirements

| Tool | Role |
|------|------|
| `nasm` | Boot / entry / interrupt / switch |
| `gcc` / `ld` / `objcopy` | Kernel & user ELF |
| `python3` | `glc`, image builders |
| QEMU | `qemu-system-i386` / `qemu-system-x86_64` |

Guest: ~128 MB RAM, 512 MB disk image.

---

## Build

```bash
make                  # x86_64 (default)
make ARCH=i386        # 32-bit
make clean
make check            # both arches from scratch
```

## Run

```bash
make run              # x86_64 in QEMU
make run-32           # i386 in QEMU
```

At the prompt:

```text
/> hello
hello from Tig!
/> ls
bin/hello.elf ...
/> echo hi | cat
```

---

## Repository layout

| Path | Purpose |
|------|---------|
| `arch/` | Boot, entry, interrupts, context switch (i386 + x86_64) |
| `*.c` / `*.h` | Kernel (scheduler, FS, syscalls, memory, …) |
| `user/` | Shell, libc, crt0, linker scripts (**C**) |
| `examples/` | Green Lizard programs (`.gl`) |
| `lizard/` | **Green Lizard** compiler (`glc`) + embed helper |
| `tools/` | `mktigfs.py`, `mkosimage.py` |
| `version.h` | Single source of version string |
| `SHELL.md` | Shell user guide |
| `CHANGELOG.md` | Release notes |

---

## Versioning

- `version.h` is the only version source of truth  
- Current series: **1.2.x — Kernel infrastructure** (at 1.2.1)  
- Bug fixes usually stay in the same version; bumps are deliberate  

Roadmap target: usable desktop-style OS by **2.0** (graphics, networking, apps).

---

## License

Hobby / educational project. Add a `LICENSE` file if you want a specific license (MIT, etc.).
