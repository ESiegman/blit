#!/usr/bin/env python3
# Draws blit's icon, the start screen's name in half-cell blocks on the
# blit-dark theme's colours, at every size macOS asks for, and makes
# res/blit.icns from them with iconutil. Run it again after changing it;
# the .icns is checked in, so a build needs neither Python nor iconutil.
import os
import struct
import subprocess
import sys
import tempfile
import zlib

# The start screen's logo (src/appstart.cpp), as pixels: each character is
# one pixel wide and two high (its top and bottom halves).
LOGO = ["█   █ ▀ ▄█▄",
        "█▀▄ █ █  █ ",
        "█▄▀ █ █  █▄"]
HALVES = {"█": (1, 1), "▀": (1, 0), "▄": (0, 1), " ": (0, 0)}
PIXELS = []
for row in LOGO:
    top = [HALVES[c][0] for c in row]
    bottom = [HALVES[c][1] for c in row]
    PIXELS += [top, bottom]
W, H = len(PIXELS[0]), len(PIXELS)

BACK_TOP = (0x2a, 0x2c, 0x33)
BACK_BOTTOM = (0x1c, 0x1d, 0x21)
EDGE = (0x41, 0x44, 0x4d)
INK = (0x61, 0xaf, 0xef)
CURSOR = (0xe5, 0xc0, 0x7b)


def mix(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def draw(size):
    # Apple's grid: the shape is 824 of 1024, centred, with corners of 185.
    k = size / 1024.0
    x0 = y0 = 100 * k
    side = 824 * k
    r = 185 * k
    edge = max(1.0, 6 * k)
    cell = side * 0.66 / W  # the logo is two thirds of the shape's width
    lx = x0 + (side - cell * (W + 1.1)) / 2
    ly = y0 + (side - cell * H) / 2
    cursor = (lx + cell * (W + 0.7), ly - cell * 0.25, lx + cell * (W + 1.1), ly + cell * (H + 0.25))  # (a thin bar: the cursor)
    ss = 4 if size <= 256 else 2
    rows = []
    for py in range(size):
        line = bytearray()
        for px in range(size):
            acc = [0.0, 0.0, 0.0, 0.0]
            for sy in range(ss):
                for sx in range(ss):
                    x = px + (sx + 0.5) / ss
                    y = py + (sy + 0.5) / ss
                    # Inside the rounded square?
                    dx = max(x0 + r - x, 0, x - (x0 + side - r))
                    dy = max(y0 + r - y, 0, y - (y0 + side - r))
                    d = (dx * dx + dy * dy) ** 0.5
                    if d > r:
                        continue
                    t = (y - y0) / side
                    c = mix(BACK_TOP, BACK_BOTTOM, t)
                    if d > r - edge or x < x0 + edge or x > x0 + side - edge or y < y0 + edge or y > y0 + side - edge:
                        c = EDGE
                    gx = int((x - lx) // cell)
                    gy = int((y - ly) // cell)
                    if 0 <= gx < W and 0 <= gy < H and x >= lx and y >= ly and PIXELS[gy][gx]:
                        c = INK
                    elif cursor[0] <= x < cursor[2] and cursor[1] <= y < cursor[3]:
                        c = CURSOR
                    acc[0] += c[0]
                    acc[1] += c[1]
                    acc[2] += c[2]
                    acc[3] += 255
            n = ss * ss
            a = acc[3] / n
            if a > 0:
                line += bytes((round(acc[0] / n * 255 / a), round(acc[1] / n * 255 / a), round(acc[2] / n * 255 / a), round(a)))
            else:
                line += b"\0\0\0\0"
        rows.append(bytes(line))
    return rows


def png(path, size, rows):
    raw = b"".join(b"\0" + r for r in rows)
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    with tempfile.TemporaryDirectory() as tmp:
        iconset = os.path.join(tmp, "blit.iconset")
        os.mkdir(iconset)
        drawn = {}
        for base in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                size = base * scale
                if size not in drawn:
                    drawn[size] = draw(size)
                name = f"icon_{base}x{base}{'@2x' if scale == 2 else ''}.png"
                png(os.path.join(iconset, name), size, drawn[size])
        png(os.path.join(here, "blit.png"), 1024, drawn[1024])
        subprocess.run(["iconutil", "-c", "icns", iconset, "-o", os.path.join(here, "blit.icns")], check=True)
    print("res/blit.icns and res/blit.png made")


if __name__ == "__main__":
    sys.exit(main())
