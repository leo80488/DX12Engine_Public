#!/usr/bin/env python3
"""Build a .ipak bundle from a list of files.

Binary format (little-endian):
    [0..4)  magic "IPAK"
    [4..8)  version u32 (=1)
    [8..12) entry count u32
    [12..)  index: per entry
                u32 path_len
                UTF-8 path (path_len bytes, no null terminator)
                u64 offset  (absolute byte offset into this .ipak)
                u64 size
    [blob]  raw concatenated file bytes

Usage:
    python tools/pack_assets.py --out game.ipak --root . --list cook_manifest.txt
    python tools/pack_assets.py --out game.ipak --root . --include-dir asset
    python tools/pack_assets.py --out game.ipak --root . --list manifest.txt --include-dir shaders
"""

import argparse
import os
import struct
import sys
from pathlib import Path


MAGIC = b"IPAK"
VERSION = 1


def normalize(p: str) -> str:
    """Match AssetFS::Normalize — forward slashes, strip leading ./"""
    p = p.replace("\\", "/")
    while p.startswith("./"):
        p = p[2:]
    return p


def gather_from_list(list_path: Path, root: Path) -> list:
    """Read a text file with one path per line (relative to root). Blank
       lines and lines starting with '#' are skipped."""
    out = []
    for raw in list_path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        p = root / line
        if not p.is_file():
            print(f"[warn] listed path missing on disk: {line}")
            continue
        out.append(normalize(line))
    return out


def gather_from_dir(dir_path: Path, root: Path) -> list:
    """Recursively collect every file under dir_path, returning root-relative
       forward-slash paths."""
    out = []
    if not dir_path.is_dir():
        return out
    for f in dir_path.rglob("*"):
        if f.is_file():
            rel = f.relative_to(root)
            out.append(normalize(str(rel)))
    return out


def build_pak(root: Path, rel_paths: list, out_path: Path) -> None:
    # Dedup while preserving order.
    seen = set()
    paths = []
    for p in rel_paths:
        if p not in seen:
            seen.add(p)
            paths.append(p)

    # Pre-compute: header + index take a fixed amount of space; entries' blob
    # offsets follow sequentially after that.
    header_size = 12
    index_size = 0
    for p in paths:
        index_size += 4 + len(p.encode("utf-8")) + 8 + 8

    cursor = header_size + index_size

    # Collect sizes by reading file stats (we re-read data later when writing;
    # fine for a build step).
    entries = []  # (path, offset, size)
    for p in paths:
        disk = root / p
        size = disk.stat().st_size
        entries.append((p, cursor, size))
        cursor += size

    total_size = cursor

    print(f"[pak] {len(entries)} entries, {total_size / (1024*1024):.1f} MiB -> {out_path}")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "wb") as f:
        # Header
        f.write(MAGIC)
        f.write(struct.pack("<II", VERSION, len(entries)))

        # Index
        for p, off, size in entries:
            path_bytes = p.encode("utf-8")
            f.write(struct.pack("<I", len(path_bytes)))
            f.write(path_bytes)
            f.write(struct.pack("<QQ", off, size))

        # Blob — copy each file's bytes. Buffered I/O: read in 1 MiB chunks so
        # huge assets (textures) don't balloon memory.
        for p, off, size in entries:
            assert f.tell() == off, f"offset mismatch for {p}: wrote {f.tell()}, expected {off}"
            with open(root / p, "rb") as src:
                remaining = size
                while remaining > 0:
                    chunk = src.read(min(remaining, 1 << 20))
                    if not chunk:
                        raise RuntimeError(f"unexpected EOF reading {p}")
                    f.write(chunk)
                    remaining -= len(chunk)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out",  required=True, help="Output .ipak file path")
    ap.add_argument("--root", default=".", help="Root directory; paths stored relative to this (default: cwd)")
    ap.add_argument("--list", default=None,
                    help="Text file with one root-relative path per line")
    ap.add_argument("--include-dir", action="append", default=[],
                    help="Recursively include all files under this directory (repeatable)")
    args = ap.parse_args()

    root = Path(args.root).resolve()

    paths = []
    if args.list:
        paths.extend(gather_from_list(Path(args.list), root))
    for d in args.include_dir:
        paths.extend(gather_from_dir(root / d, root))

    if not paths:
        sys.exit("No input files specified. Provide --list and/or --include-dir.")

    build_pak(root, paths, Path(args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
