#!/usr/bin/env python3
"""Pack a game's RomFS directory tree into one file the host reads directly (dm/host/hle_pak.c).

The IPA used to carry the RomFS as 600-odd files inside the app bundle: every install moved the whole game again, even when only the host's code had changed. The packed file is
one entry to copy (to the app's Documents, once per game version) and one mmap in the host, so a program-only update is a small IPA and the data stays where it is.

Format (little endian), all offsets absolute:

  0x00  char  magic[8]      "DMPAK1\\n\\0"
  0x08  u32   version       1
  0x0c  u32   flags         0
  0x10  u64   n_entries     entries in the table
  0x18  u64   toc_off       offset of the entry table (88)
  0x20  u64   names_off     offset of the name pool
  0x28  u64   names_len
  0x30  u64   data_off      offset of the first file's bytes
  0x38  u64   data_len
  0x40  u64   total_len     the file's size
  0x48  u64   reserved[2]

Each entry is 48 bytes, in depth-first order (a directory comes before its children):

  0x00  u32 name_off    into the name pool
  0x04  u16 name_len
  0x06  u8  type        0 directory, 1 file
  0x07  u8  pad
  0x08  u64 data_off    the file's bytes
  0x10  u32 data_len
  0x14  u32 first_child index, or 0xffffffff
  0x18  u32 next_sibling index, or 0xffffffff
  0x1c  u32 parent      index, or 0xffffffff for the root
  0x20  u64 pad[2]

Entry 0 is the root directory (empty name, parent 0xffffffff), so a game path "rom:/a/b" is the chain of names a, b below it. File data is 16-byte aligned and stored in the
order the entries appear, so the host reads whole files with one memcpy from the mapping. Names keep the case the game itself uses; the host falls back to a case-insensitive
comparison for a component that does not match exactly, as Horizon paths are case-sensitive but games do not always agree with their own file names.

Usage:
  romfs_pack.py <RomFS dir> <out.dmpak>            pack
  romfs_pack.py --list <file.dmpak>                list the entries
  romfs_pack.py --verify <RomFS dir> <file.dmpak>  compare a packed file with a directory (sizes and contents)
  romfs_pack.py --unpack <file.dmpak> <dir>        write the tree back out (for inspection)

Run from a terminal or Spyder (F5) as the other scripts in scripts/ are: the SETTINGS block below supplies what the command line does not.
"""
import argparse
import os
import struct
import sys

# ---- Settings (Spyder F5 uses these; command-line arguments override them) ----
SRC = ''          # RomFS directory to pack
OUT = ''          # the .dmpak to write
LIST = ''         # --list: a .dmpak to list
VERIFY_SRC = ''   # --verify: the directory
VERIFY_PAK = ''   # --verify: the .dmpak
UNPACK_SRC = ''   # --unpack: the .dmpak
UNPACK_DIR = ''   # --unpack: the output directory
# ------------------------------------------------------------------------------

MAGIC = b"DMPAK1\n\0"
VERSION = 1
HEADER = struct.Struct('<8sII9Q')           # 8 + 4 + 4 + 9*8 = 88 bytes
ENTRY = struct.Struct('<IHBBQIIIIQQ')       # 48 bytes
ENTRY_SIZE = ENTRY.size
ROOT = 0xFFFFFFFF
ALIGN = 16

assert HEADER.size == 88 and ENTRY_SIZE == 48


def walk(src):
    """Entries in depth-first order: (path parts, is_dir, host path, size)."""
    out = []

    def rec(parts, path):
        try:
            names = sorted(os.listdir(path))
        except OSError as e:
            sys.exit(f'romfs_pack: cannot list {path}: {e}')
        for name in names:
            p = os.path.join(path, name)
            if os.path.isdir(p):
                out.append((parts + [name], True, p, 0))
                rec(parts + [name], p)
            elif os.path.isfile(p):
                out.append((parts + [name], False, p, os.path.getsize(p)))
            # anything else (a device node, a broken link) is not part of a game's data and is skipped

    rec([], src)
    return out


def pack(src, out):
    if not os.path.isdir(src):
        sys.exit(f'romfs_pack: {src} is not a directory')
    items = walk(src)
    names = bytearray()
    # entries: [name_off, name_len, type, data_off, data_len, first_child, next_sibling, parent]; entry 0 is the root
    entries = [[0, 0, 0, 0, 0, ROOT, ROOT, ROOT]]
    last_child = {0: ROOT}
    stack = [(0, 0)]                       # (entry index, depth) of the open directories
    for parts, is_dir, path, size in items:
        depth = len(parts)
        while stack and stack[-1][1] >= depth:
            stack.pop()
        parent = stack[-1][0]
        name = parts[-1].encode()
        idx = len(entries)
        entries.append([len(names), len(name), 0 if is_dir else 1, 0, 0 if is_dir else size, ROOT, ROOT, parent])
        names += name + b'\0'
        if last_child[parent] != ROOT:
            entries[last_child[parent]][6] = idx
        else:
            entries[parent][5] = idx
        last_child[parent] = idx
        last_child[idx] = ROOT
        if is_dir:
            stack.append((idx, depth))
    # layout: header, entry table, names, then the file data 16-byte aligned in entry order
    toc_bytes = len(entries) * ENTRY_SIZE
    names_off = HEADER.size + toc_bytes
    data_off = (names_off + len(names) + ALIGN - 1) // ALIGN * ALIGN
    off = data_off
    for i, (parts, is_dir, path, size) in enumerate(items, start=1):
        if is_dir:
            continue
        entries[i][3] = off
        off = (off + size + ALIGN - 1) // ALIGN * ALIGN
    total = off
    with open(out, 'wb') as f:
        f.write(HEADER.pack(MAGIC, VERSION, 0, len(entries), HEADER.size, names_off, len(names), data_off, total - data_off, total, 0, 0))
        for e in entries:
            f.write(ENTRY.pack(e[0], e[1], e[2], 0, e[3], e[4], e[5], e[6], e[7], 0, 0))
        f.write(bytes(names))
        f.write(b'\0' * (data_off - names_off - len(names)))
        for i, (parts, is_dir, path, size) in enumerate(items, start=1):
            if is_dir or not size:
                continue
            f.seek(entries[i][3])
            with open(path, 'rb') as g:
                left = size
                while left:
                    chunk = g.read(min(left, 1 << 20))
                    if not chunk:
                        sys.exit(f'romfs_pack: {path} changed while packing')
                    f.write(chunk)
                    left -= len(chunk)
        f.truncate(total)
    print(f'{out}: {len(entries) - 1} entries, {sum(1 for _, d, _, _ in items if not d)} files, {total} bytes ({total / 1048576:.1f} MiB)')
    return 0


def read_header(f):
    h = f.read(HEADER.size)
    if len(h) != HEADER.size:
        sys.exit('romfs_pack: short file')
    magic, ver, flags, n, toc_off, names_off, names_len, data_off, data_len, total, _r0, _r1 = HEADER.unpack(h)
    if magic != MAGIC or ver != VERSION:
        sys.exit(f'romfs_pack: not a version {VERSION} DMPAK file (magic {magic!r}, version {ver})')
    return dict(n=n, toc_off=toc_off, names_off=names_off, names_len=names_len, data_off=data_off, data_len=data_len, total=total)


def read_entries(f, h):
    f.seek(h['toc_off'])
    raw = f.read(h['n'] * ENTRY_SIZE)
    if len(raw) != h['n'] * ENTRY_SIZE:
        sys.exit('romfs_pack: truncated entry table')
    f.seek(h['names_off'])
    names = f.read(h['names_len'])
    out = []
    for i in range(h['n']):
        name_off, name_len, typ, _pad, data_off, data_len, first_child, next_sibling, parent, _p0, _p1 = ENTRY.unpack_from(raw, i * ENTRY_SIZE)
        out.append(dict(name=names[name_off:name_off + name_len].decode('utf-8', 'surrogateescape'), type='dir' if typ == 0 else 'file',
                        data_off=data_off, data_len=data_len, first_child=first_child, next_sibling=next_sibling, parent=parent))
    return out


def paths(entries):
    out = {}

    def rec(i, prefix):
        e = entries[i]
        p = prefix if not e['name'] else (prefix + '/' + e['name'] if prefix else e['name'])
        out[i] = p
        c = e['first_child']
        while c != ROOT:
            rec(c, p)
            c = entries[c]['next_sibling']

    rec(0, '')
    return out


def do_list(path):
    with open(path, 'rb') as f:
        h = read_header(f)
        entries = read_entries(f, h)
        ps = paths(entries)
    for i, e in enumerate(entries):
        kind = e['type']
        print(f'{"d" if kind == "dir" else "f"} {e["data_len"]:>12}  /{ps[i]}')
    print(f'{len(entries) - 1} entries, {h["total"]} bytes')
    return 0


def do_verify(src, path):
    with open(path, 'rb') as f:
        h = read_header(f)
        entries = read_entries(f, h)
        ps = paths(entries)
        bad = 0
        have = set()
        for i, e in enumerate(entries):
            p = ps[i]
            if not p:
                continue
            have.add(p)
            hp = os.path.join(src, *p.split('/'))
            if e['type'] == 'dir':
                if not os.path.isdir(hp):
                    print(f'✗ missing directory {p}')
                    bad += 1
                continue
            if not os.path.isfile(hp):
                print(f'✗ missing file {p}')
                bad += 1
                continue
            if os.path.getsize(hp) != e['data_len']:
                print(f'✗ size differs {p}: directory {os.path.getsize(hp)}, pack {e["data_len"]}')
                bad += 1
                continue
            f.seek(e['data_off'])
            left = e['data_len']
            with open(hp, 'rb') as g:
                while left:
                    a = f.read(min(left, 1 << 20))
                    b = g.read(len(a))
                    if a != b:
                        print(f'✗ content differs {p}')
                        bad += 1
                        break
                    left -= len(a)
        extra = set()
        for parts, is_dir, hp, _s in walk(src):
            extra.add('/'.join(parts))
        for p in sorted(extra - have):
            print(f'✗ not in the pack: {p}')
            bad += 1
    if bad:
        print(f'{bad} differences')
        return 1
    print(f'same: {len(entries) - 1} entries and every byte matches {src}')
    return 0


def do_unpack(path, out):
    with open(path, 'rb') as f:
        h = read_header(f)
        entries = read_entries(f, h)
        ps = paths(entries)
        for i, e in enumerate(entries):
            p = ps[i]
            if not p:
                continue
            hp = os.path.join(out, *p.split('/'))
            if e['type'] == 'dir':
                os.makedirs(hp, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(hp), exist_ok=True)
            f.seek(e['data_off'])
            left = e['data_len']
            with open(hp, 'wb') as g:
                while left:
                    chunk = f.read(min(left, 1 << 20))
                    if not chunk:
                        sys.exit('romfs_pack: truncated data')
                    g.write(chunk)
                    left -= len(chunk)
    print(f'{out}: {len(entries) - 1} entries written')
    return 0


def main():
    ap = argparse.ArgumentParser(description='Pack a RomFS directory into one file the host reads (dm/host/hle_pak.c).')
    ap.add_argument('src', nargs='?', default=SRC, help='the RomFS directory to pack (or with --list/--unpack: the .dmpak)')
    ap.add_argument('out', nargs='?', default=OUT, help='the .dmpak to write (or with --unpack: the directory)')
    ap.add_argument('--list', nargs='?', const=LIST or True, default=None, metavar='DMPAK', help='list a packed file')
    ap.add_argument('--verify', nargs=2, default=None, metavar=('DIR', 'DMPAK'), help='compare a packed file with a directory')
    ap.add_argument('--unpack', nargs=2, default=None, metavar=('DMPAK', 'DIR'), help='write a packed file back out as a tree')
    a = ap.parse_args()
    if a.verify:
        return do_verify(*a.verify)
    if a.unpack:
        return do_unpack(*a.unpack)
    if a.list:
        return do_list(a.list if isinstance(a.list, str) and a.list not in ('True',) else (VERIFY_PAK or LIST or a.src))
    if not a.src or not a.out:
        ap.error('needs a RomFS directory and an output .dmpak (or --list / --verify / --unpack)')
    return pack(a.src, a.out)


if __name__ == '__main__':
    sys.exit(main())
