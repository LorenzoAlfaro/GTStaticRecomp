#!/usr/bin/env python3
"""Extract the code the recompiler needs from a Gran Turismo (USA, SCUS-94194) disc image.

Usage: python tools/extract.py <disc.cue|disc.bin> [outdir=data]

GT's code is five PS-X executables that all load at 0x80010000 and replace each other:
SCUS_941.94 (boot), GTOS.EXE, GTMENU.EXE, GTMAIN.EXE (race) and GTEND.EXE. Each one is
packed: a stub at the entry point decompresses the image in place and jumps to it.

Writes:
  <outdir>/exe/<NAME>.bin     packed executable as on the disc (PS-X EXE)
  <outdir>/exe/<NAME>.img     unpacked memory image, loaded at <base>
  <outdir>/exe/exes.json      name -> {base, size, entry, gp, packed_entry}
  <outdir>/disc.json          ISO directory (name -> lba, size)
"""
import json
import os
import struct
import sys

RAW = 2352
EXES = ["SCUS_941.94", "GTOS.EXE", "GTMENU.EXE", "GTMAIN.EXE", "GTEND.EXE"]


def resolve_bin(path):
    if path.lower().endswith(".cue"):
        with open(path) as f:
            for line in f:
                if line.strip().upper().startswith("FILE"):
                    return os.path.join(os.path.dirname(path), line.split('"')[1])
        raise SystemExit("no FILE in cue")
    return path


class Disc:
    def __init__(self, path):
        self.f = open(path, "rb")

    def sector(self, lba):
        self.f.seek(lba * RAW)
        return self.f.read(RAW)[24:24 + 2048]

    def read(self, lba, size):
        return b"".join(self.sector(lba + i) for i in range((size + 2047) // 2048))[:size]

    def walk(self, lba, size, path=""):
        buf = self.read(lba, size)
        i = 0
        while i < len(buf):
            n = buf[i]
            if n == 0:
                i = (i // 2048 + 1) * 2048
                continue
            rec = buf[i:i + n]
            elba, esize = struct.unpack("<I", rec[2:6])[0], struct.unpack("<I", rec[10:14])[0]
            name = rec[33:33 + rec[32]]
            if name not in (b"\0", b"\1"):
                name = name.decode().split(";")[0]
                full = f"{path}/{name}" if path else name
                if rec[25] & 2:
                    yield from self.walk(elba, esize, full)
                else:
                    yield full, elba, esize
            i += n


def unpack(exe):
    """Run the packer's decompressor. The stub at the entry point is
         lui a0, hi ; ori a0, a0, lo     -> a0 = header
    and the header is
         +0  'PSLZ'
         +4  last byte of the unpacked image (decompression runs backwards)
         +8  unpacked size
         +12 last byte of the packed stream
         +16 where the stub copies its decompressor, +20 its size
         +24 entry point of the unpacked program
    Returns (base, image, entry)."""
    pc, gp, taddr, tsize = struct.unpack_from("<4I", exe, 0x10)
    text = bytearray(exe[0x800:0x800 + tsize])
    w0, w1 = struct.unpack_from("<2I", text, pc - taddr)
    assert w0 >> 16 == 0x3C04 and w1 >> 16 == 0x3484, "unknown packer stub"
    hdr = ((w0 & 0xFFFF) << 16) | (w1 & 0xFFFF)
    magic, dst_end, size, src_end, _, _, entry = struct.unpack_from("<4s6I", text, hdr - taddr)
    assert magic == b"PSLZ", magic
    base = dst_end + 1 - size
    lo = min(base, taddr)
    mem = bytearray(dst_end + 1 - lo if dst_end + 1 > taddr + tsize else taddr + tsize - lo)
    mem[taddr - lo:taddr - lo + tsize] = text
    a0, a1 = src_end - lo, dst_end - lo  # src / dst cursors (inclusive, moving down)
    while True:
        flags = mem[a0]
        a0 -= 1
        done = False
        for _ in range(8):
            if not a0 < a1:
                done = True
                break
            v1 = mem[a0]
            a0 -= 1
            if not flags & 1:
                mem[a1] = v1
                a1 -= 1
            else:
                n = v1 + 3
                d = mem[a0]
                a0 -= 1
                if d & 0x80:
                    d = ((d & 0x7F) << 8) | mem[a0]
                    a0 -= 1
                src = a1 + d + 1
                for _ in range(n):
                    mem[a1] = mem[src]
                    a1 -= 1
                    src -= 1
            flags >>= 1
        if done or not a0 < a1:
            break
    return base, bytes(mem[base - lo:base - lo + size]), entry


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    out = sys.argv[2] if len(sys.argv) > 2 else "data"
    disc = Disc(resolve_bin(sys.argv[1]))
    pvd = disc.sector(16)
    assert pvd[1:6] == b"CD001", "not an ISO9660 image"
    root = pvd[156:190]
    files = {n: (l, s) for n, l, s in disc.walk(struct.unpack("<I", root[2:6])[0],
                                               struct.unpack("<I", root[10:14])[0])}
    cnf = disc.read(*files["SYSTEM.CNF"])
    assert b"SCUS_941.94" in cnf, "expected Gran Turismo USA (SCUS-94194)"
    os.makedirs(os.path.join(out, "exe"), exist_ok=True)
    with open(os.path.join(out, "disc.json"), "w") as f:
        json.dump(files, f, indent=1)

    meta = {}
    for name in EXES:
        exe = disc.read(*files[name])
        assert exe[:8] == b"PS-X EXE", name
        with open(os.path.join(out, "exe", name + ".bin"), "wb") as f:
            f.write(exe)
        base, img, entry = unpack(exe)
        with open(os.path.join(out, "exe", name + ".img"), "wb") as f:
            f.write(img)
        pc, gp = struct.unpack_from("<2I", exe, 0x10)
        meta[name] = {"base": base, "size": len(img), "entry": entry, "gp": gp, "packed_entry": pc}
        print(f"{name}: {len(exe):#x} packed -> {len(img):#x} bytes @ {base:#010x}, entry {entry:#010x}")
    with open(os.path.join(out, "exe", "exes.json"), "w") as f:
        json.dump(meta, f, indent=1)
    print("ok")


if __name__ == "__main__":
    main()
