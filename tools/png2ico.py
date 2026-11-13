"""png2ico.py SRC.png OUT.ico [MASTER.png] - builds the program icon from a square RGBA picture.

Every size is resampled from the source with Lanczos and stored as a PNG entry (Windows Vista
and later read those).  With MASTER, a 512 px copy of the source is saved too (kept in the
repository so the icon can be rebuilt)."""
import io
import struct
import sys

from PIL import Image

SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]


def main():
    src = Image.open(sys.argv[1]).convert("RGBA")
    if src.width != src.height:
        side = max(src.size)
        sq = Image.new("RGBA", (side, side), (0, 0, 0, 0))
        sq.paste(src, ((side - src.width) // 2, (side - src.height) // 2))
        src = sq
    blobs = []
    for s in SIZES:
        buf = io.BytesIO()
        src.resize((s, s), Image.LANCZOS).save(buf, "PNG", optimize=True)
        blobs.append(buf.getvalue())
    out = bytearray(struct.pack("<HHH", 0, 1, len(SIZES)))
    off = 6 + 16 * len(SIZES)
    for s, b in zip(SIZES, blobs):
        out += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(b), off)
        off += len(b)
    for b in blobs:
        out += b
    with open(sys.argv[2], "wb") as f:
        f.write(out)
    if len(sys.argv) > 3:
        src.resize((512, 512), Image.LANCZOS).save(sys.argv[3], "PNG", optimize=True)
    print(sys.argv[2], len(out), "bytes,", len(SIZES), "sizes")


if __name__ == "__main__":
    main()
