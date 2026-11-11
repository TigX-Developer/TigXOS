# TigxOS

A dual-architecture (i386 + x86_64) hobby operating system with a thin kernel,
flat TigFS filesystem, concurrent shell, and Green Lizard userland language.

**Current version:** 1.1.3

## Features (1.1.x Foundations)

- Dual-arch kernel: i386 protected mode and x86_64 long mode
- Per-process address spaces, preemptive scheduling, pipes and jobs
- TigFS flat filesystem with path resolution and cwd
- Standard layout: programs under `bin/`; shell `PATH=bin:.`
- Interactive shell: redirection, pipelines, background jobs, Ctrl-C, variables
- Green Lizard (`.gl`) compiled to native ELF user programs
- Shared open-file table, `malloc`/`brk`, signals (SIGINT/SIGKILL/SIGTERM)

## Requirements

- `nasm`, `gcc`, `ld`, `objcopy`, `python3`
- QEMU (`qemu-system-i386` / `qemu-system-x86_64`) to run
- ~128 MB RAM guest, 512 MB disk image

## Build

```bash
make                # x86_64 (default)
make ARCH=i386      # 32-bit
make clean
make check          # build both arches from scratch
```

## Run

```bash
make run            # x86_64 in QEMU
make run-32         # i386 in QEMU
make test           # headless tests both arches (if configured)
```

## Layout

| Path | Purpose |
|------|---------|
| `arch/` | Boot, entry, interrupt, context switch (i386 + x86_64) |
| `user/` | Shell, libc, crt0, linker scripts |
| `examples/` | Green Lizard programs (`hello`, `ls`, `cat`, …) |
| `lizard/` | `glc` compiler and ELF embed helper |
| `tools/` | `mktigfs.py`, `mkosimage.py` |
| `version.h` | Single source of version string |
| `CHANGELOG.md` | Release history |
| `SHELL.md` | Shell user reference |

## Versioning

See the master roadmap. `version.h` is authoritative. Bug fixes stay in the
current version unless a release bump is explicitly approved.

## License

Hobby / educational project. See repository for license details if present.
