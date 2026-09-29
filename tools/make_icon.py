#!/usr/bin/env python3
"""
make_icon.py - Generate the mouse-shaped .ico used by PreoMG14Driver.exe.

Kept in the repository so the icon is reproducible from source and nothing
binary has to be trusted. Run:  python3 tools/make_icon.py src/preo.ico
"""
import math
import struct
import sys

W = H = 32

OUT   = (0x24, 0x26, 0x30, 0xFF)   # outline
BODY  = (0xEC, 0xEE, 0xF4, 0xFF)   # shell
SHELL = (0xD6, 0xDA, 0xE6, 0xFF)   # lower shell
ACC   = (0x2F, 0x7A, 0xE0, 0xFF)   # scroll wheel
GLOSS = (0xFF, 0xFF, 0xFF, 0xFF)
CLEAR = (0, 0, 0, 0)

px = [[CLEAR] * W for _ in range(H)]

# Mouse silhouette: an ellipse, which is what actually reads as a mouse once
# you add the button split line and the scroll wheel.
CX, CY = 15.5, 15.5
RX, RY = 8.6, 13.2
INSET = 0.75          # pull the fill in so the outline shows


def inside(x, y, scale=1.0):
    dx = (x + 0.5 - CX) / (RX * scale)
    dy = (y + 0.5 - CY) / (RY * scale)
    return dx * dx + dy * dy <= 1.0


for y in range(H):
    for x in range(W):
        if inside(x, y):
            px[y][x] = BODY
        if inside(x, y, INSET / 1.0):
            px[y][x] = BODY

# Outline: any transparent pixel orthogonally adjacent to a filled one
for y in range(H):
    for x in range(W):
        if px[y][x] == CLEAR:
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                nx, ny = x + dx, y + dy
                if 0 <= nx < W and 0 <= ny < H and px[ny][nx] != CLEAR:
                    px[y][x] = OUT
                    break

# Lower shell: everything below the button split is a slightly darker plastic
for y in range(H):
    for x in range(W):
        if px[y][x] is BODY or px[y][x] == BODY:
            if y > 16:
                px[y][x] = SHELL

# Button split line across the upper half
for x in range(W):
    for y in (15, 16):
        if px[y][x] in (BODY, SHELL):
            px[y][x] = OUT

# Scroll wheel
for y in range(8, 14):
    for x in (15, 16):
        if px[y][x] in (BODY, SHELL, OUT):
            px[y][x] = ACC
for y in (7, 14):
    for x in (15, 16):
        if px[y][x] in (BODY, SHELL):
            px[y][x] = OUT

# Glossy highlight on the upper-left of the shell
for y in range(6, 10):
    for x in range(10, 13):
        if px[y][x] == BODY:
            px[y][x] = GLOSS

# ---- encode as a 32-bit BGRA .ico ----------------------------------------
rows = []
for y in range(H - 1, -1, -1):          # BMP rows are bottom-up
    row = bytearray()
    for x in range(W):
        r, g, b, a = px[y][x]
        row += bytes((b, g, r, a))
    rows.append(bytes(row))
xor = b"".join(rows)
and_mask = b"\x00" * (4 * H)            # 32-bit alpha carries transparency

header = struct.pack("<IiiHHIIiiII", 40, W, H * 2, 1, 32, 0,
                     len(xor) + len(and_mask), 0, 0, 0, 0)
image = header + xor + and_mask

ico = struct.pack("<HHH", 0, 1, 1)
ico += struct.pack("<BBBBHHII", W, H, 0, 0, 1, 32, len(image), 6 + 16)
ico += image

out = sys.argv[1] if len(sys.argv) > 1 else "preo.ico"
with open(out, "wb") as f:
    f.write(ico)
print(f"wrote {out} ({len(ico)} bytes)")
