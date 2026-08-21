#!/usr/bin/env python3
"""Minimal reader for UE4 .pak archives, legacy index (versions 3-9).

Tasomachi ships a version 9 pak: unencrypted, index not frozen, Zlib in the
compression table. repak does not read v9 (FrozenIndex), so this exists instead.
Only what the mod needs: list, and extract by prefix or by exact path.

    python tools/pak.py list   <pak> [substring ...]
    python tools/pak.py unpack <pak> -o <dir> [-i <path prefix> ...]
"""
import argparse
import os
import struct
import sys
import zlib

MAGIC = b"\xE1\x12\x6F\x5A"
FOOTER_V9 = 222


class Reader:
    def __init__(self, buf, pos=0):
        self.b = buf
        self.p = pos

    def take(self, n):
        out = self.b[self.p:self.p + n]
        self.p += n
        return out

    def i32(self):
        return struct.unpack_from("<i", self.b, self._bump(4))[0]

    def u32(self):
        return struct.unpack_from("<I", self.b, self._bump(4))[0]

    def i64(self):
        return struct.unpack_from("<q", self.b, self._bump(8))[0]

    def u8(self):
        return self.b[self._bump(1)]

    def _bump(self, n):
        p = self.p
        self.p += n
        return p

    def string(self):
        n = self.i32()
        if n == 0:
            return ""
        if n < 0:  # UTF-16
            return self.take(-n * 2)[:-2].decode("utf-16-le")
        return self.take(n)[:-1].decode("utf-8", "replace")


class Entry:
    __slots__ = ("path", "offset", "size", "usize", "method", "blocks", "flags", "block_size")


def read_footer(f, size):
    f.seek(size - FOOTER_V9)
    b = f.read(FOOTER_V9)
    if b[17:21] != MAGIC:
        raise SystemExit("not a version 8/9 pak (footer magic not where expected)")
    version, = struct.unpack_from("<I", b, 21)
    idx_off, idx_size = struct.unpack_from("<QQ", b, 25)
    encrypted = b[16]
    frozen = b[61]
    methods = [b[62 + i * 32:62 + (i + 1) * 32].rstrip(b"\0").decode("latin1") for i in range(5)]
    if encrypted:
        raise SystemExit("encrypted index - an AES key would be needed")
    if frozen:
        raise SystemExit("frozen index - not supported")
    return version, idx_off, idx_size, methods


def read_entry(r, version):
    e = Entry()
    e.offset = r.i64()
    e.size = r.i64()
    e.usize = r.i64()
    e.method = r.i32()          # index into the footer's compression method table
    r.take(20)                  # sha1
    e.blocks = []
    if e.method != 0:
        for _ in range(r.i32()):
            e.blocks.append((r.i64(), r.i64()))
    e.flags = r.u8()
    e.block_size = r.u32()
    return e


def read_index(path):
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        version, idx_off, idx_size, methods = read_footer(f, size)
        f.seek(idx_off)
        r = Reader(f.read(idx_size))
        mount = r.string()
        count = r.i32()
        entries = []
        for _ in range(count):
            name = r.string()
            e = read_entry(r, version)
            e.path = mount.replace("../../../", "") + name
            entries.append(e)
    return version, methods, mount, entries


def extract(f, e, methods):
    """The payload is preceded by a duplicate FPakEntry header, whose size varies
    with the block count, so seek past it by reparsing rather than by a constant."""
    f.seek(e.offset)
    head = f.read(1024)
    hr = Reader(head)
    read_entry(hr, 9)
    data_at = e.offset + hr.p

    if e.method == 0:
        f.seek(data_at)
        return f.read(e.size)

    name = methods[e.method - 1]
    if name != "Zlib":
        raise SystemExit(f"unsupported compression '{name}' for {e.path}")
    out = bytearray()
    for start, end in e.blocks:
        # PakFile_Version_RelativeChunkOffsets (6) and later: block offsets are
        # relative to the start of the entry, header included - not to the payload.
        f.seek(e.offset + start)
        out += zlib.decompress(f.read(end - start))
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("action", choices=["list", "unpack", "info"])
    ap.add_argument("pak")
    ap.add_argument("filter", nargs="*")
    ap.add_argument("-o", "--out", default="extracted")
    ap.add_argument("-i", "--include", action="append", default=[])
    args = ap.parse_args()

    version, methods, mount, entries = read_index(args.pak)

    if args.action == "info":
        print(f"version={version} mount={mount!r} entries={len(entries)} methods={methods}")
        return

    if args.action == "list":
        for e in entries:
            if all(s.lower() in e.path.lower() for s in args.filter):
                print(f"{e.usize:>10}  {'z' if e.method else '-'}  {e.path}")
        return

    wanted = [e for e in entries
              if not args.include or any(e.path.startswith(i) or i.lower() in e.path.lower()
                                         for i in args.include)]
    print(f"{len(wanted)} file(s) -> {args.out}")
    with open(args.pak, "rb") as f:
        for e in wanted:
            dst = os.path.join(args.out, e.path.replace("/", os.sep))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "wb") as o:
                o.write(extract(f, e, methods))


if __name__ == "__main__":
    main()
