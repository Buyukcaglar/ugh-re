#!/usr/bin/env python3
"""mzinfo.py - MZ header, relocations, segment refs, INT/IN/OUT usage of a DOS EXE (capstone)."""
import sys, struct, collections
from capstone import Cs, CS_ARCH_X86, CS_MODE_16

d = open(sys.argv[1], "rb").read()
(sig, cblp, cp, crlc, cparhdr, minalloc, maxalloc, ss, sp, csum, ip, cs, lfarlc, ovno) = struct.unpack_from("<2s13H", d, 0)
hdr = cparhdr * 16
size = (cp - 1) * 512 + (cblp or 512)
img = d[hdr:size]
print(f"image {len(img):#x} bytes, CS:IP {cs:04x}:{ip:04x} SS:SP {ss:04x}:{sp:04x} min {minalloc:#x} max {maxalloc:#x} relocs {crlc}")
rel = [struct.unpack_from("<HH", d, lfarlc + 4 * i) for i in range(crlc)]
segs = collections.Counter()
for off, seg in rel:
    lin = seg * 16 + off
    segs[struct.unpack_from("<H", img, lin)[0]] += 1
print("segment values referenced by relocations:", sorted((f"{k:04x}", v) for k, v in segs.items()))
print("reloc sites by segment:", sorted(collections.Counter(s for _, s in rel).items()))
if len(sys.argv) > 2:
    md = Cs(CS_ARCH_X86, CS_MODE_16)
    ints = collections.Counter(); ports = collections.Counter()
    # linear sweep (rough) for statistics only
    for ins in md.disasm(img, 0):
        if ins.mnemonic == "int": ints[ins.op_str] += 1
        if ins.mnemonic in ("in", "out"): ports[ins.op_str] += 1
    print("INT:", sorted(ints.items()))
    print("IN/OUT:", sorted(ports.items()))
