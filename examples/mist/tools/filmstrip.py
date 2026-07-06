#!/usr/bin/env python3
"""Montage mist ECS_FILMSTRIP frames into one timestamped image.

Usage: filmstrip.py [frames_dir] [out.png] [every_seconds]
       (defaults /tmp/strip, /tmp/filmstrip.png, 2.5)

Needs Pillow (`python3 -m pip install Pillow`).
"""
import glob
import sys

from PIL import Image, ImageDraw

d = sys.argv[1] if len(sys.argv) > 1 else "/tmp/strip"
out = sys.argv[2] if len(sys.argv) > 2 else "/tmp/filmstrip.png"
every = float(sys.argv[3]) if len(sys.argv) > 3 else 2.5

files = sorted(glob.glob(d + "/*.bmp"))
if not files:
    sys.exit("no frames in " + d)
imgs = [Image.open(f).convert("RGB") for f in files]

cols = 4
rows = (len(imgs) + cols - 1) // cols
w = 300
h = int(w * imgs[0].height / imgs[0].width)
sheet = Image.new("RGB", (cols * w, rows * h), "white")
draw = ImageDraw.Draw(sheet)
for i, im in enumerate(imgs):
    x, y = (i % cols) * w, (i // cols) * h
    sheet.paste(im.resize((w, h)), (x, y))
    draw.text((x + 4, y + 4), f"t={i * every:.1f}s", fill=(200, 0, 0))
sheet.save(out)
print("saved", out, sheet.size)
