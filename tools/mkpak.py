"""mkpak.py GAMEDIR [AMIGA.ADF] OUT.pak - packs the game's files into one file for src/hal/pack.c.

Used by `build.cmd private` for a personal build that carries the game inside UGH64.exe (and,
when given, the Amiga disk image for the Amiga sound, stored as AMIGA.ADF).
Such a build contains the game's data: it must not be published or shared."""
import os
import struct
import sys

GAME_FILES = ("UGH.EXE", "UGH!.HI")


def main():
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    src, out = sys.argv[1], sys.argv[-1]
    adf = sys.argv[2] if len(sys.argv) == 4 else None
    have = {n.upper(): n for n in os.listdir(src) if os.path.isfile(os.path.join(src, n))}
    if "UGH.EXE" not in have:
        sys.exit(f"UGH.EXE not found in {src}")
    entries = [(n, open(os.path.join(src, have[n]), "rb").read()) for n in GAME_FILES if n in have]
    if adf:
        entries.append(("AMIGA.ADF", open(adf, "rb").read()))
    head = bytearray(b"P2PK" + struct.pack("<I", len(entries)))
    off = 8 + 24 * len(entries)
    for n, b in entries:
        nm = n.encode("ascii")
        if len(nm) > 15:
            sys.exit(f"name too long: {n}")
        head += nm.ljust(16, b"\0") + struct.pack("<II", off, len(b))
        off += len(b)
    with open(out, "wb") as f:
        f.write(head)
        for _, b in entries:
            f.write(b)
    print(f"{out}: {len(entries)} files, {off} bytes")


if __name__ == "__main__":
    main()
