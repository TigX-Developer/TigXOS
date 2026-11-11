# TigxOS — dual arch + ELF spawn (version lives in version.h)
# Requirements: 128 MB RAM installed (120 MB usable), 512 MB disk
#
#   make / make ARCH=x86_64 | make ARCH=i386
#   make run-32 / run-64

ASM=nasm
GLC=python3 lizard/glc.py
VERSION := $(shell sed -n 's/.*TIGX_VERSION "\(.*\)"/\1/p' version.h)
GL_PROGS = hello echo loop cat rm ls wc writefile lang65 exitn
GL_ELFS  = $(GL_PROGS:%=user/%.gl.elf)
CC=gcc
LD=ld
OBJCOPY=objcopy

ARCH ?= x86_64
ARCH_DIR=arch/$(ARCH)
ARCH_STAMP=.arch-stamp

ifneq ($(shell cat $(ARCH_STAMP) 2>/dev/null),$(ARCH))
  $(info ARCH=$(ARCH) (was: $(shell cat $(ARCH_STAMP) 2>/dev/null || echo none)) — cleaning stale objects)
  $(shell rm -f *.o *.d *.bin kernel.elf os-image.bin tigfs.img embed_hello.c user/*.o user/*.d user/*.elf user/*.gl.c)
  $(shell echo $(ARCH) > $(ARCH_STAMP))
endif


KERNEL_OBJS=entry.o interrupt.o switch.o console.o interrupt_c.o keyboard.o timer.o \
	memory.o string.o process.o filetable.o disk.o fs.o spinlock.o wait.o sync.o font.o font_data.o \
	gdt.o syscall.o pipe.o aspace.o elf.o embed_hello.o test.o tests.o kernel.o

# Only the shell is still embedded in the kernel image (init). All other
# user programs live on the TigFS image produced by tools/mktigfs.py.
# 256 sectors ≈ 128 KB — comfortable headroom for shell + kernel code.
KERNEL_SECTS=288
KERNEL_MAX_BYTES=$(shell echo $$(( $(KERNEL_SECTS) * 512 )))
TIGFS_OFFSET=$$((512 * 512))   # LBA 512 in bytes


ifeq ($(ARCH),i386)
  CFLAGS=-ffreestanding -fno-pie -fno-stack-protector -m32 -nostdlib -I. -Wall -Wextra -MMD -MP
  USER_CFLAGS=-ffreestanding -fno-pie -fno-stack-protector -m32 -nostdlib -static -Wall -I. -Iuser -MMD -MP
  USER_LD=user/user32.ld
  SHELL_LD=user/shell32.ld
  LIBGCC=$(shell $(CC) -m32 -print-libgcc-file-name)
  LDFLAGS=-m elf_i386 -T $(ARCH_DIR)/linker.ld -nostdlib
  ASM_ELF=elf32
  QEMU=qemu-system-i386
  ARCH_LABEL=i386 protected mode
else
  ARCH=x86_64
  CFLAGS=-ffreestanding -fno-pie -fno-stack-protector -m64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -nostdlib -I. -Wall -Wextra -MMD -MP
  USER_CFLAGS=-ffreestanding -fno-pie -fno-stack-protector -m64 -nostdlib -static -Wall -I. -Iuser -MMD -MP
  USER_LD=user/user64.ld
  SHELL_LD=user/shell64.ld
  LDFLAGS=-m elf_x86_64 -T linker.ld -nostdlib
  LIBGCC=
  ASM_ELF=elf64
  QEMU=qemu-system-x86_64
  ARCH_LABEL=x86_64 long mode
endif

# make TESTS=1 runs every kernel test suite at boot (before the shell starts)
TESTS ?= 0
ifeq ($(TESTS),1)
  CFLAGS += -DTIGX_BOOT_TESTS
endif
ifneq ($(shell cat .tests-stamp 2>/dev/null),$(TESTS))
  $(shell rm -f *.o *.d kernel.elf kernel.bin os-image.bin embed_hello.c; echo $(TESTS) > .tests-stamp)
endif

all: os-image.bin

# ---- user programs ----
user/crt0.o: user/crt0.c user/syscall.h
	$(CC) $(USER_CFLAGS) -c user/crt0.c -o $@

user/libc.o: user/libc.c user/libc.h user/syscall.h
	$(CC) $(USER_CFLAGS) -c user/libc.c -o $@

# green lizard programs: examples/NAME.gl -> glc -> user/NAME.gl.c -> user/NAME.gl.elf
user/%.gl.c: examples/%.gl lizard/glc.py
	$(GLC) $< -o $@

user/%.gl.elf: user/%.gl.c user/crt0.o user/libc.o $(USER_LD)
	$(CC) $(USER_CFLAGS) -T $(USER_LD) -o $@ user/crt0.o user/libc.o $<

# shell is C (for now); it uses its own link script
user/shell.o: user/shell.c user/libc.h version.h
	$(CC) $(USER_CFLAGS) -c user/shell.c -o $@

user/shell.elf: user/crt0.o user/libc.o user/shell.o $(SHELL_LD)
	$(CC) $(USER_CFLAGS) -T $(SHELL_LD) -o $@ user/crt0.o user/libc.o user/shell.o

user/args.o: user/args.c user/libc.h
	$(CC) $(USER_CFLAGS) -c user/args.c -o $@

user/args.elf: user/crt0.o user/libc.o user/args.o $(USER_LD)
	$(CC) $(USER_CFLAGS) -T $(USER_LD) -o $@ user/crt0.o user/libc.o user/args.o

# core tools are green lizard (examples/*.gl) via GL_PROGS


.PHONY: user-gl
user-gl: $(GL_ELFS)
	@echo "green lizard ELFs ready: $(GL_PROGS)"

# Only the shell is embedded in the kernel (needed as init before TigFS is up).
# All other programs live on the pre-built TigFS image.
embed_hello.c: user/shell.elf lizard/embed_user_elves.py
	python3 lizard/embed_user_elves.py user/shell.elf:shell.elf > $@

embed_hello.o: embed_hello.c
	$(CC) $(CFLAGS) -c embed_hello.c -o $@

# Host-built TigFS image containing every user program (stripped).
# Written into os-image.bin at LBA 512 so boot mounts a populated FS.
# Standard layout (v1.1.3): user programs under bin/; shell + args also at root for compat.
tigfs.img: $(GL_ELFS) user/shell.elf user/args.elf tools/mktigfs.py
	python3 tools/mktigfs.py -o $@ --strip \
	  $(foreach p,$(GL_PROGS),bin/$(p).elf:user/$(p).gl.elf) \
	  shell.elf:user/shell.elf \
	  bin/shell.elf:user/shell.elf \
	  args.elf:user/args.elf \
	  bin/args.elf:user/args.elf

boot.bin: $(ARCH_DIR)/boot.asm
	$(ASM) -f bin $< -o $@

entry.o: $(ARCH_DIR)/entry.asm
	$(ASM) -f $(ASM_ELF) $< -o $@

interrupt.o: $(ARCH_DIR)/interrupt.asm
	$(ASM) -f $(ASM_ELF) $< -o $@

switch.o: $(ARCH_DIR)/switch.asm
	$(ASM) -f $(ASM_ELF) $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

interrupt_c.o: interrupt.c
	$(CC) $(CFLAGS) -c interrupt.c -o $@

kernel.elf: $(KERNEL_OBJS)
	$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJS) $(LIBGCC)

kernel.bin: kernel.elf
	$(OBJCOPY) -O binary $< $@
	@python3 -c "import os,sys; s=os.path.getsize('kernel.bin'); m=$(KERNEL_MAX_BYTES);\
print('kernel.bin',s,'bytes (max',m,') ARCH=$(ARCH)', '-- WARNING: over 80% full' if s>m*0.8 else '');\
sys.exit('ERROR: kernel.bin exceeds bootloader KERNEL_SECTS') if s>m else None"

# boot (1 sector) + kernel, pad to LBA 512, then splice pre-built TigFS image.
os-image.bin: boot.bin kernel.bin tigfs.img
	python3 tools/mkosimage.py boot.bin kernel.bin tigfs.img $@ "TigxOS V$(VERSION) ($(ARCH_LABEL))"
run: os-image.bin
	$(QEMU) -m 128 -drive file=os-image.bin,format=raw,if=ide,index=0,media=disk

run-debug: os-image.bin
	$(QEMU) -m 128 -drive file=os-image.bin,format=raw,if=ide,index=0,media=disk -serial stdio -d int,cpu_reset -D qemu.log

run-32:
	$(MAKE) ARCH=i386 run

run-64:
	$(MAKE) ARCH=x86_64 run

# Headless automated test run (isa-debug-exit). Exit status:
#   0 → all suites passed
#   1 → one or more checks failed (or QEMU crash / timeout)
test-qemu: os-image.bin
	@echo "=== test-qemu ARCH=$(ARCH) ==="
	$(QEMU) -m 128 \
	  -display none -serial stdio \
	  -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
	  -drive file=os-image.bin,format=raw,if=ide,index=0,media=disk \
	  ; rc=$$?; \
	  if [ $$rc -eq 1 ]; then echo "PASS (QEMU status 1)"; exit 0; \
	  elif [ $$rc -eq 3 ]; then echo "FAIL (QEMU status 3)"; exit 1; \
	  else echo "UNEXPECTED QEMU status $$rc"; exit 1; fi

test-qemu-32:
	$(MAKE) ARCH=i386 TESTS=1 test-qemu

test-qemu-64:
	$(MAKE) ARCH=x86_64 TESTS=1 test-qemu

# One-command release gate: both arches, full test suites
test: test-qemu-32 test-qemu-64
	@echo "=== both arches green ==="

# Size report (kernel image vs KERNEL_SECTS, shell vs TigFS max, tigfs.img)
size: kernel.bin user/shell.elf
	@python3 -c "import os; \
k=os.path.getsize('kernel.bin'); m=$(KERNEL_MAX_BYTES); \
print('kernel.bin  %7d / %7d bytes  (%5.1f%%)  ARCH=$(ARCH)' % (k,m,100.0*k/m)); \
s=os.path.getsize('user/shell.elf'); \
print('shell.elf   %7d bytes  (TigFS max ~56 KB)' % s); \
t=os.path.getsize('tigfs.img') if os.path.exists('tigfs.img') else 0; \
print('tigfs.img   %7d bytes  (programs on disk)' % t)"

clean:
	rm -f *.o *.d *.bin kernel.elf os-image.bin tigfs.img qemu.log $(ARCH_STAMP) .tests-stamp embed_hello.c user/*.o user/*.d user/*.elf user/*.gl.c

# Auto-generated header dependencies (-MMD -MP)
-include $(wildcard *.d) $(wildcard user/*.d)

# build both architectures from scratch (no QEMU needed)
check:
	$(MAKE) clean
	$(MAKE) ARCH=i386
	$(MAKE) ARCH=x86_64

.PHONY: all clean run run-debug run-32 run-64 check test-qemu test-qemu-32 test-qemu-64 test size


