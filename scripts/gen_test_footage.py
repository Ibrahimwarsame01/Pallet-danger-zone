#!/usr/bin/env python3
"""Generate synthetic 320x240 24-bit BMP footage for PalletGuard integration test.

Frames 0-14 : static scene, upright dark 'pallet' at right.
Frames 15-24: pallet falls over (big pixel change in ROI -> motion + occupancy).
Frames 20-39: bright 'person' blob walks into the danger zone.
"""
import os
import struct
import sys

W, H = 320, 240
OUT = sys.argv[1] if len(sys.argv) > 1 else "footage"


def write_bmp(path, pix):  # pix[y][x] = (b, g, r), top-down
    rowbytes = (W * 3 + 3) & ~3
    pad = b"\x00" * (rowbytes - W * 3)
    with open(path, "wb") as f:
        f.write(b"BM")
        f.write(struct.pack("<IHHI", 54 + rowbytes * H, 0, 0, 54))
        f.write(struct.pack("<IiiHHIIiiII", 40, W, H, 1, 24, 0,
                            rowbytes * H, 2835, 2835, 0, 0))
        for y in range(H - 1, -1, -1):  # bottom-up
            f.write(b"".join(bytes(p) for p in pix[y]) + pad)


def rect(pix, x0, y0, x1, y1, c):
    for y in range(max(0, y0), min(H, y1)):
        for x in range(max(0, x0), min(W, x1)):
            pix[y][x] = c


os.makedirs(OUT, exist_ok=True)
for i in range(40):
    pix = [[(128, 128, 128)] * W for _ in range(H)]
    # floor line
    rect(pix, 0, 150, W, 240, (110, 110, 110))
    dark = (40, 40, 40)
    if i < 15:                       # upright pallet: tall & narrow
        rect(pix, 220, 90, 260, 150, dark)
    else:                            # falling: interpolate to lying wide & flat
        t = min(1.0, (i - 15) / 5.0)
        w = int(40 + 60 * t)         # 40 -> 100 wide
        h = int(60 - 40 * t)         # 60 -> 20 tall
        rect(pix, 240 - w // 2 - int(20 * t), 150 - h, 240 + w // 2, 150, dark)
    if i >= 20:                      # person walks in from the left
        px = min(140, 20 + (i - 20) * 8)
        rect(pix, px - 8, 170, px + 8, 220, (230, 230, 230))
    write_bmp(os.path.join(OUT, "frame_%05d.bmp" % i), pix)
print("wrote 40 frames to", OUT)
