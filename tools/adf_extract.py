#!/usr/bin/env python3
"""adf_extract.py ADF OUTDIR - extracts every file of an AmigaDOS OFS/FFS floppy image."""
import os, struct, sys

BS = 512


def blk(d, n):
    return d[n * BS:(n + 1) * BS]


def name_of(b):
    n = b[BS - 80]
    return b[BS - 79:BS - 79 + n].decode('latin-1')


def read_file(d, hdr, ffs):
    b = blk(d, hdr)
    size = struct.unpack('>I', b[BS - 188:BS - 184])[0]
    out = bytearray()
    while True:
        cnt = struct.unpack('>I', b[8:12])[0]
        ptrs = [struct.unpack('>I', b[24 + 4 * i:28 + 4 * i])[0] for i in range(72)]
        for p in reversed(ptrs[72 - cnt:]):
            db = blk(d, p)
            if ffs:
                out += db
            else:
                n = struct.unpack('>I', db[12:16])[0]
                out += db[24:24 + n]
        ext = struct.unpack('>I', b[BS - 8:BS - 4])[0]
        if ext == 0:
            break
        b = blk(d, ext)
    return bytes(out[:size])


def walk(d, n, path, ffs, out, listing):
    b = blk(d, n)
    for i in range(72):
        h = struct.unpack('>I', b[24 + 4 * i:28 + 4 * i])[0]
        while h:
            e = blk(d, h)
            nm = name_of(e)
            sec = struct.unpack('>i', e[BS - 4:])[0]
            p = os.path.join(path, nm)
            if sec == 2:
                os.makedirs(os.path.join(out, p), exist_ok=True)
                walk(d, h, p, ffs, out, listing)
            else:
                data = read_file(d, h, ffs)
                with open(os.path.join(out, p), 'wb') as f:
                    f.write(data)
                listing.append((p, len(data)))
            h = struct.unpack('>I', e[BS - 16:BS - 12])[0]


def main():
    d = open(sys.argv[1], 'rb').read()
    out = sys.argv[2]
    os.makedirs(out, exist_ok=True)
    ffs = d[3] & 1
    listing = []
    walk(d, 880, '', ffs, out, listing)
    for p, n in listing:
        print(f'{n:8d}  {p}')


if __name__ == '__main__':
    main()
