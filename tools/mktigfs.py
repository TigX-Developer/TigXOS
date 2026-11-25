#!/usr/bin/env python3
"""
mktigfs.py — build a hierarchical TigFS v6 image for TigxOS 1.3.8.

Layout:
  LBA 512      : superblock
  LBA 513      : bitmap
  LBA 514..577 : 64 inode sectors
  LBA 578+     : data blocks

Inode 0 is always the root directory. Directory data holds 32-byte dentries
{name[28], ino:u16, type:u16}. Paths like bin/shell.elf create dir "bin"
and file "shell.elf" inside it.

Usage:
  python3 tools/mktigfs.py -o tigfs.img [--strip] name:path [name:path ...]
"""
from __future__ import annotations
import argparse
import os
import struct
import subprocess
import sys
import tempfile

TIGFS_MAGIC       = 0x31534654
TIGFS_VERSION     = 6
TIGFS_RESERVED    = 512
TIGFS_MAX_INODES  = 64
TIGFS_INODE_BYTES = 512
TIGFS_DIRECT      = 112
TIGFS_NAME_LEN    = 28
TIGFS_BITMAP_LBA  = TIGFS_RESERVED + 1
TIGFS_INODE_LBA   = TIGFS_RESERVED + 2
TIGFS_DATA_LBA    = TIGFS_RESERVED + 2 + 64
MAX_DATA_BLOCKS   = 4096
FREE_SLACK_BLOCKS = 1024
SECTOR = 512

TIGFS_FILE = 1
TIGFS_DIR  = 3


def make_super(disk_sectors: int, data_count: int) -> bytes:
    fields = [
        TIGFS_MAGIC, TIGFS_VERSION, disk_sectors, TIGFS_RESERVED,
        TIGFS_INODE_LBA, TIGFS_MAX_INODES, TIGFS_BITMAP_LBA, TIGFS_DATA_LBA,
        data_count,
    ] + [0] * 7
    return struct.pack("<16I", *fields).ljust(SECTOR, b"\0")


def make_inode(used: int, typ: int, size: int, blocks: list[int], name: str) -> bytes:
    # used:u8 type:u8 nlink:u16 size:u32 block[112] name[28] extra[26]
    buf = bytearray(TIGFS_INODE_BYTES)
    buf[0] = used & 0xFF
    buf[1] = typ & 0xFF
    struct.pack_into("<H", buf, 2, 1)  # nlink
    struct.pack_into("<I", buf, 4, size)
    for i, b in enumerate(blocks[:TIGFS_DIRECT]):
        struct.pack_into("<I", buf, 8 + i * 4, b)
    name_off = 8 + TIGFS_DIRECT * 4  # 456
    nb = name.encode("ascii")[: TIGFS_NAME_LEN - 1]
    buf[name_off : name_off + len(nb)] = nb
    return bytes(buf)


def make_dentry(name: str, ino: int, typ: int) -> bytes:
    buf = bytearray(32)
    nb = name.encode("ascii")[: TIGFS_NAME_LEN - 1]
    buf[0 : len(nb)] = nb
    struct.pack_into("<HH", buf, 28, ino & 0xFFFF, typ & 0xFFFF)
    return bytes(buf)


def strip_elf(path: str) -> bytes:
    tmp_path = None
    try:
        with tempfile.NamedTemporaryFile(delete=False, suffix=".elf") as tmp:
            tmp_path = tmp.name
        subprocess.check_call(
            ["strip", "-s", "-o", tmp_path, path],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        return open(tmp_path, "rb").read()
    except Exception:
        return open(path, "rb").read()
    finally:
        if tmp_path and os.path.exists(tmp_path):
            try:
                os.unlink(tmp_path)
            except OSError:
                pass


def main() -> int:
    ap = argparse.ArgumentParser(description="Build hierarchical TigFS v6 image")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--strip", action="store_true")
    ap.add_argument("--disk-sectors", type=int, default=0)
    ap.add_argument("entries", nargs="+", help="name:path (name may contain /)")
    args = ap.parse_args()

    files: list[tuple[str, bytes]] = []
    for spec in args.entries:
        if ":" not in spec:
            print(f"error: need name:path, got {spec}", file=sys.stderr)
            return 1
        name, path = spec.split(":", 1)
        if not os.path.isfile(path):
            print(f"error: missing {path}", file=sys.stderr)
            return 1
        if len(name) >= 64:
            print(f"error: name too long: {name}", file=sys.stderr)
            return 1
        data = strip_elf(path) if args.strip else open(path, "rb").read()
        if len(data) > TIGFS_DIRECT * SECTOR:
            print(f"error: {name} too large", file=sys.stderr)
            return 1
        files.append((name.lstrip("/"), data))

    # --- allocate inodes: 0=root, then dirs, then files ---
    inodes = [make_inode(0, 0, 0, [], "")] * TIGFS_MAX_INODES
    inodes[0] = make_inode(1, TIGFS_DIR, 0, [], "/")

    dir_children: dict[int, list[tuple[str, int, int]]] = {0: []}  # ino -> [(name, child_ino, type)]
    path_to_ino: dict[str, int] = {"": 0, "/": 0}
    next_ino = 1
    bitmap = bytearray(SECTOR)
    next_block = 0
    data_payload: list[tuple[int, bytes]] = []

    def alloc_blocks(n: int) -> list[int]:
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

    def ensure_dir(rel: str) -> int:
        nonlocal next_ino
        rel = rel.strip("/")
        if not rel:
            return 0
        if rel in path_to_ino:
            return path_to_ino[rel]
        parts = rel.split("/")
        parent_rel = ""
        parent_ino = 0
        for part in parts:
            cur = f"{parent_rel}/{part}" if parent_rel else part
            if cur not in path_to_ino:
                if next_ino >= TIGFS_MAX_INODES:
                    raise SystemExit("out of inodes")
                ino = next_ino
                next_ino += 1
                inodes[ino] = make_inode(1, TIGFS_DIR, 0, [], part)
                path_to_ino[cur] = ino
                dir_children.setdefault(parent_ino, []).append((part, ino, TIGFS_DIR))
                dir_children.setdefault(ino, [])
            parent_ino = path_to_ino[cur]
            parent_rel = cur
        return parent_ino

    for name, data in files:
        if "/" in name:
            parent_rel, base = name.rsplit("/", 1)
            parent_ino = ensure_dir(parent_rel)
        else:
            parent_ino = 0
            base = name
        if next_ino >= TIGFS_MAX_INODES:
            raise SystemExit("out of inodes")
        ino = next_ino
        next_ino += 1
        nblocks = (len(data) + SECTOR - 1) // SECTOR if data else 0
        blocks = alloc_blocks(nblocks) if nblocks else []
        abs_blocks = [TIGFS_DATA_LBA + b for b in blocks]
        padded = data.ljust(nblocks * SECTOR, b"\0") if nblocks else b""
        for bi, blk in enumerate(blocks):
            data_payload.append((blk, padded[bi * SECTOR : (bi + 1) * SECTOR]))
        inodes[ino] = make_inode(1, TIGFS_FILE, len(data), abs_blocks, base)
        dir_children.setdefault(parent_ino, []).append((base, ino, TIGFS_FILE))
        path_to_ino[name] = ino

    # Write directory data for each dir that has children
    for dino, children in dir_children.items():
        if not children:
            continue
        blob = b"".join(make_dentry(n, c, t) for n, c, t in children)
        nblocks = (len(blob) + SECTOR - 1) // SECTOR
        blocks = alloc_blocks(nblocks)
        abs_blocks = [TIGFS_DATA_LBA + b for b in blocks]
        padded = blob.ljust(nblocks * SECTOR, b"\0")
        for bi, blk in enumerate(blocks):
            data_payload.append((blk, padded[bi * SECTOR : (bi + 1) * SECTOR]))
        name = "/" if dino == 0 else inodes[dino][456:456 + TIGFS_NAME_LEN].split(b"\0")[0].decode("ascii", "ignore")
        inodes[dino] = make_inode(1, TIGFS_DIR, len(blob), abs_blocks, name if dino else "/")

    data_count = max(next_block, 1) + FREE_SLACK_BLOCKS
    if data_count > MAX_DATA_BLOCKS:
        data_count = MAX_DATA_BLOCKS
    disk_sectors = args.disk_sectors
    if disk_sectors <= TIGFS_DATA_LBA + data_count:
        disk_sectors = TIGFS_DATA_LBA + data_count + 64

    n_sectors = 1 + 1 + 64 + data_count
    img = bytearray(n_sectors * SECTOR)

    def put(rel: int, data: bytes):
        off = rel * SECTOR
        img[off : off + len(data)] = data

    put(0, make_super(disk_sectors, data_count))
    put(1, bytes(bitmap))
    for i, ino in enumerate(inodes):
        put(2 + i, ino)
    for blk, sec in data_payload:
        put(66 + blk, sec)

    with open(args.output, "wb") as f:
        f.write(img)

    print(f"mktigfs: wrote {args.output} ({len(img)} bytes, "
          f"{len(files)} files, hierarchical, {data_count} blocks)")
    for name, data in files:
        print(f"  {name:20s} {len(data):7d} B")
    return 0


if __name__ == "__main__":
    sys.exit(main())
