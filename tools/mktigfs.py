#!/usr/bin/env python3
"""
mktigfs.py — build a TigFS filesystem image for TigxOS.

Layout (matches fs.c TIGFS_VERSION 6):
  LBA 0..511   : reserved (boot + kernel) — NOT written by this tool
  LBA 512      : superblock
  LBA 513      : bitmap (1 sector, up to 4096 data blocks)
  LBA 514..577 : 64 inode sectors (512 B each)
  LBA 578+     : data blocks

Usage:
  python3 tools/mktigfs.py -o tigfs.img [--strip] name:path [name:path ...]

  --strip   run `strip -s` on each ELF before embedding (requires strip in PATH)
  -o FILE   output image (raw sectors starting at LBA 512)

The resulting file is meant to be written into os-image.bin at offset
512*512 so that boot mounts a pre-populated TigFS and never needs to
re-seed every program from the kernel image.
"""
from __future__ import annotations
import argparse
import os
import struct
import subprocess
import sys
import tempfile

TIGFS_MAGIC       = 0x31534654  # 'TFS1'
TIGFS_VERSION     = 6
TIGFS_RESERVED    = 512
TIGFS_MAX_INODES  = 64
TIGFS_INODE_BYTES = 512
TIGFS_DIRECT      = 112
TIGFS_NAME_LEN    = 28
TIGFS_BITMAP_LBA  = TIGFS_RESERVED + 1
TIGFS_INODE_LBA   = TIGFS_RESERVED + 2
TIGFS_DATA_LBA    = TIGFS_RESERVED + 2 + 64   # 578
MAX_DATA_BLOCKS   = 4096                      # one bitmap sector

SECTOR = 512


def align_up(n: int, a: int) -> int:
    return (n + a - 1) // a * a


def make_super(disk_sectors: int, data_count: int) -> bytes:
    # struct tigfs_super — 12 u32 then pad[7]
    fields = [
        TIGFS_MAGIC,
        TIGFS_VERSION,
        disk_sectors,
        TIGFS_RESERVED,
        TIGFS_INODE_LBA,
        TIGFS_MAX_INODES,
        TIGFS_BITMAP_LBA,
        TIGFS_DATA_LBA,
        data_count,
    ] + [0] * 7
    blob = struct.pack("<16I", *fields)
    assert len(blob) == 64
    return blob.ljust(SECTOR, b"\0")


def make_inode(used: int, size: int, blocks: list[int], name: str) -> bytes:
    # used:u8, type:u8, pad:u16, size:u32, block[80]:u32, name[28], extra[156]
    typ = 1  # TIGFS_FILE
    buf = bytearray(TIGFS_INODE_BYTES)
    buf[0] = used & 0xFF
    buf[1] = typ
    struct.pack_into("<I", buf, 4, size)
    for i, b in enumerate(blocks[:TIGFS_DIRECT]):
        struct.pack_into("<I", buf, 8 + i * 4, b)
    nb = name.encode("ascii")[: TIGFS_NAME_LEN - 1]
    buf[8 + TIGFS_DIRECT * 4 : 8 + TIGFS_DIRECT * 4 + len(nb)] = nb
    return bytes(buf)


def strip_elf(path: str) -> bytes:
    """Return stripped ELF bytes (or original on failure / missing strip)."""
    try:
        with tempfile.NamedTemporaryFile(delete=False, suffix=".elf") as tmp:
            tmp_path = tmp.name
        subprocess.check_call(["strip", "-s", "-o", tmp_path, path],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        data = open(tmp_path, "rb").read()
        os.unlink(tmp_path)
        return data
    except Exception:
        return open(path, "rb").read()


def main() -> int:
    ap = argparse.ArgumentParser(description="Build a TigFS image for TigxOS")
    ap.add_argument("-o", "--output", required=True, help="output tigfs.img")
    ap.add_argument("--strip", action="store_true", help="strip ELFs before packing")
    ap.add_argument("--disk-sectors", type=int, default=512 * 1024 * 1024 // 512,
                    help="total disk sectors (default 512 MB)")
    ap.add_argument("files", nargs="+", help="name:path pairs (fs name : host path)")
    args = ap.parse_args()

    entries: list[tuple[str, bytes]] = []
    for spec in args.files:
        if ":" not in spec:
            print(f"error: need name:path, got {spec}", file=sys.stderr)
            return 1
        name, path = spec.split(":", 1)
        if not os.path.isfile(path):
            print(f"error: missing {path}", file=sys.stderr)
            return 1
        if len(name) >= TIGFS_NAME_LEN:
            print(f"error: name too long: {name}", file=sys.stderr)
            return 1
        data = strip_elf(path) if args.strip else open(path, "rb").read()
        if len(data) > TIGFS_DIRECT * SECTOR:
            print(f"error: {name} too large ({len(data)} > {TIGFS_DIRECT*SECTOR})",
                  file=sys.stderr)
            return 1
        entries.append((name, data))

    if len(entries) > TIGFS_MAX_INODES:
        print("error: too many files", file=sys.stderr)
        return 1

    # Allocate data blocks
    bitmap = bytearray(SECTOR)  # 4096 bits
    next_block = 0

    def alloc(n: int) -> list[int]:
        nonlocal next_block
        out = []
        for _ in range(n):
            if next_block >= MAX_DATA_BLOCKS:
                raise SystemExit("out of data blocks")
            byte, bit = next_block // 8, next_block % 8
            bitmap[byte] |= (1 << bit)
            out.append(next_block)
            next_block += 1
        return out

    inodes = [make_inode(0, 0, [], "")] * TIGFS_MAX_INODES
    data_payload: list[tuple[int, bytes]] = []  # (block_index, sector_bytes)

    for idx, (name, data) in enumerate(entries):
        nblocks = (len(data) + SECTOR - 1) // SECTOR if data else 0
        blocks = alloc(nblocks) if nblocks else []
        # pad last sector
        padded = data.ljust(nblocks * SECTOR, b"\0") if nblocks else b""
        # Kernel stores absolute LBAs in inode.block[] (fs_write does the same)
        abs_blocks = [TIGFS_DATA_LBA + b for b in blocks]
        for bi, blk in enumerate(blocks):
            sector_data = padded[bi * SECTOR : (bi + 1) * SECTOR]
            data_payload.append((blk, sector_data))  # payload still relative in image
        inodes[idx] = make_inode(1, len(data), abs_blocks, name)

    data_count = max(next_block, 1)
    # Cap like the kernel
    if data_count > MAX_DATA_BLOCKS:
        data_count = MAX_DATA_BLOCKS

    disk_sectors = args.disk_sectors
    if disk_sectors <= TIGFS_DATA_LBA + data_count:
        disk_sectors = TIGFS_DATA_LBA + data_count + 64

    # Build image: sectors from LBA 512 onward
    # We write: super, bitmap, 64 inode sectors, then data blocks
    n_sectors = 1 + 1 + 64 + data_count
    img = bytearray(n_sectors * SECTOR)

    def put(lba_from_512: int, data: bytes):
        off = lba_from_512 * SECTOR
        img[off : off + len(data)] = data

    put(0, make_super(disk_sectors, data_count))          # LBA 512
    put(1, bytes(bitmap))                                 # LBA 513
    for i, ino in enumerate(inodes):
        put(2 + i, ino)                                   # LBA 514+
    for blk, sec in data_payload:
        put(66 + blk, sec)                                # data starts at relative 66

    with open(args.output, "wb") as f:
        f.write(img)

    total = sum(len(d) for _, d in entries)
    print(f"mktigfs: wrote {args.output} ({len(img)} bytes, "
          f"{len(entries)} files, {total} data bytes, {data_count} blocks)")
    for name, data in entries:
        print(f"  {name:20s} {len(data):7d} B")
    return 0


if __name__ == "__main__":
    sys.exit(main())
