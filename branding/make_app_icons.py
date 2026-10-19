#!/usr/bin/env python3
"""Per-app icons: the logo with its mark in each app's own colour.

Reads the apps and their accent colours from apps/common/apps.cpp and writes,
for each app (not share), into branding/apps/:
  <id>.svg       linux start-menu icon (scalable)
  <id>.png       256 px, for the AppImage and anything that wants a bitmap
  <id>.ico       windows, built into wetwaresuite-<id>.exe
  <id>.icns      macOS, inside wetwaresuite <id>.app

  python3 branding/make_app_icons.py      (needs Pillow; only run when the logo or a colour changes)
"""
import io
import re
import struct
from pathlib import Path

from PIL import Image

root = Path(__file__).resolve().parent.parent
brand = root / "branding"
out = brand / "apps"
INK = (14, 17, 22)
MINT_G, INK_G = 227, 17  # the mark is mint #19e3b1 on ink #0e1116: green says how much is mark


def accents():
    text = (root / "apps/common/apps.cpp").read_text()
    found = re.findall(r'\{"([a-z]+)", "[^"]*",\s*\{[^}]*\},\s*0x([0-9a-fA-F]{6})', text)
    return [(i, tuple(int(h[k:k + 2], 16) for k in (0, 2, 4))) for i, h in found if i != "share"]


def recolour(img, colour):
    img = img.convert("RGBA")
    px = img.load()
    for y in range(img.height):
        for x in range(img.width):
            r, g, b, a = px[x, y]
            t = min(1.0, max(0.0, (g - INK_G) / (MINT_G - INK_G)))
            px[x, y] = tuple(round(INK[c] + (colour[c] - INK[c]) * t) for c in range(3)) + (a,)
    return img


def png_bytes(img):
    buf = io.BytesIO()
    img.save(buf, "PNG", optimize=True)
    return buf.getvalue()


def icns(images):
    # PNG payloads by size: what macOS 11+ reads
    kinds = {16: b"icp4", 32: b"icp5", 64: b"icp6", 128: b"ic07", 256: b"ic08", 512: b"ic09", 1024: b"ic10"}
    body = b"".join(kinds[s] + struct.pack(">I", len(d) + 8) + d for s, d in ((s, png_bytes(images[s])) for s in sorted(kinds)))
    return b"icns" + struct.pack(">I", len(body) + 8) + body


def main():
    out.mkdir(exist_ok=True)
    svg = (brand / "icon.svg").read_text()
    sizes = [16, 32, 48, 64, 128, 256, 512, 1024]
    base = {s: Image.open(brand / f"png/icon-{s}.png") for s in sizes}
    for app, colour in accents():
        hexcol = "#%02x%02x%02x" % colour
        (out / f"{app}.svg").write_text(svg.replace("#19e3b1", hexcol))
        imgs = {s: recolour(base[s], colour) for s in sizes}
        imgs[256].save(out / f"{app}.png", optimize=True)
        imgs[256].save(out / f"{app}.ico", sizes=[(s, s) for s in (16, 32, 48, 64, 128, 256)])
        (out / f"{app}.icns").write_bytes(icns(imgs))
        print(app, hexcol)


if __name__ == "__main__":
    main()
