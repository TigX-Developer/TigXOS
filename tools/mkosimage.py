#!/usr/bin/env python3
"""Assemble os-image.bin: boot + kernel (LBA 0..511) + tigfs.img (LBA 512+)."""
import sys
import os

def main():
    if len(sys.argv) < 5:
        print("usage: mkosimage.py boot.bin kernel.bin tigfs.img os-image.bin [label]",
              file=sys.stderr)
        return 1
    boot_path, kern_path, tig_path, out_path = sys.argv[1:5]
    label = sys.argv[5] if len(sys.argv) > 5 else ""

    boot = open(boot_path, "rb").read()
    kern = open(kern_path, "rb").read()
    tig  = open(tig_path, "rb").read()
    if len(boot) != 512:
        print(f"error: boot.bin must be 512 bytes, got {len(boot)}", file=sys.stderr)
        return 1

    reserved = 512 * 512
    if 512 + len(kern) > reserved:
        print("error: kernel exceeds reserved region", file=sys.stderr)
        return 1

    big = 512 * 1024 * 1024
    with open(out_path, "wb") as f:
        f.write(boot)
        f.write(kern)
        pad = reserved - 512 - len(kern)
        if pad > 0:
            f.write(b"\0" * pad)
        f.write(tig)
        try:
            f.truncate(big)
        except OSError:
            pass

    print(f"Built {out_path} (512 MB sparse){(' — ' + label) if label else ''}")
    print(f"  kernel {len(kern)} bytes; TigFS {len(tig)} bytes at LBA 512")
    return 0

if __name__ == "__main__":
    sys.exit(main())
