#!/usr/bin/env python3
"""The textures of a level file (Levels\\X.3d2_42) as PNGs, for looking at the game's art (DESIGN.md
decision 13). The file has a table of 80-byte headers: width, height, Xbox format (0x0c DXT1, 0x0e DXT3,
0x0f DXT5, 0x12 linear ARGB), size with mips, ..., the size again at +32; the pixels follow, in table
order, from 24 bytes before the table's end [verified: the sizes end exactly at the file's end in
frontend_XBOX, HUD_GRAPHICS, HUD_DRIVING and frontend]. Materials name them: their records come before
the table, each with its texture's index. Mip 0 only; needs Pillow.

    tex3d2.py FILE OUTDIR
"""
import os, struct, sys
from PIL import Image

SIZES = {8, 16, 32, 64, 128, 256, 512, 1024}
FORMATS = {0x0c, 0x0e, 0x0f, 0x12}


def is_header(d, o):
    if o + 80 > len(d):
        return False
    w, h, fmt, size = struct.unpack_from('<IIII', d, o)
    return w in SIZES and h in SIZES and fmt in FORMATS and 0 < size < 1 << 24 and \
        struct.unpack_from('<I', d, o + 32)[0] == size


def rgb565(c):
    return (c >> 11 & 31) * 255 // 31, (c >> 5 & 63) * 255 // 63, (c & 31) * 255 // 31


def colours(blk, one_bit):
    c0, c1, bits = struct.unpack_from('<HHI', blk, 0)
    p0, p1 = rgb565(c0), rgb565(c1)
    if c0 > c1 or not one_bit:
        pal = [p0 + (255,), p1 + (255,), tuple((2 * a + b) // 3 for a, b in zip(p0, p1)) + (255,),
               tuple((a + 2 * b) // 3 for a, b in zip(p0, p1)) + (255,)]
    else:
        pal = [p0 + (255,), p1 + (255,), tuple((a + b) // 2 for a, b in zip(p0, p1)) + (255,), (0, 0, 0, 0)]
    return [pal[bits >> 2 * i & 3] for i in range(16)]


def decode(data, w, h, fmt):
    img = Image.new('RGBA', (w, h))
    px = img.load()
    if fmt == 0x12:
        for y in range(h):
            for x in range(w):
                b, g, r, a = data[4 * (y * w + x):4 * (y * w + x) + 4]
                px[x, y] = (r, g, b, a)
        return img
    size = 8 if fmt == 0x0c else 16
    k = 0
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            blk = data[k:k + size]
            k += size
            if fmt == 0x0c:
                cols = colours(blk, True)
            else:
                cols = colours(blk[8:], False)
                if fmt == 0x0e:
                    a = struct.unpack_from('<Q', blk, 0)[0]
                    al = [(a >> 4 * i & 15) * 17 for i in range(16)]
                else:
                    a0, a1 = blk[0], blk[1]
                    ab = int.from_bytes(blk[2:8], 'little')
                    if a0 > a1:
                        ap = [a0, a1] + [((6 - i) * a0 + (1 + i) * a1) // 7 for i in range(6)]
                    else:
                        ap = [a0, a1] + [((4 - i) * a0 + (1 + i) * a1) // 5 for i in range(4)] + [0, 255]
                    al = [ap[ab >> 3 * i & 7] for i in range(16)]
                cols = [c[:3] + (al[i],) for i, c in enumerate(cols)]
            for i in range(16):
                x, y = bx + i % 4, by + i // 4
                if x < w and y < h:
                    px[x, y] = cols[i]
    return img


def main():
    path, out = sys.argv[1:3]
    d = open(path, 'rb').read()
    os.makedirs(out, exist_ok=True)
    start = next(o for o in range(0, len(d) - 160, 2) if is_header(d, o) and is_header(d, o + 80))
    o = start
    headers = []
    while is_header(d, o):
        headers.append(struct.unpack_from('<4I', d, o))
        o += 80
    o -= 24
    for i, (w, h, fmt, size) in enumerate(headers):
        decode(d[o:o + size], w, h, fmt).save('%s/%02d_%dx%d.png' % (out, i, w, h))
        print(i, w, h, hex(fmt), size)
        o += size
    print('table at %d, %d textures, ended at %d of %d' % (start, len(headers), o, len(d)))


if __name__ == '__main__':
    main()
