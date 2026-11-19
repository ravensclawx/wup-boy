#!/usr/bin/env python3
"""WUP Boy's icon (64x64): a handheld drawn from plain shapes (a body, its screen, a cross and two buttons).
    python3 mkicon.py     -> icon.png"""
import os
from PIL import Image, ImageDraw
HERE = os.path.dirname(os.path.abspath(__file__)); S = 4
top, bottom = (0x8e, 0x9a, 0x3c), (0x55, 0x66, 0x22)
im = Image.new('RGB', (64 * S, 64 * S)); d = ImageDraw.Draw(im)
for y in range(64 * S):
    t = y / (64 * S - 1); d.line([(0, y), (64 * S, y)], fill=tuple(int(a + (b - a) * t) for a, b in zip(top, bottom)))
w = (255, 255, 255); mid = tuple((a + b) // 2 for a, b in zip(top, bottom)); r = lambda *v: [x * S for x in v]
d.rounded_rectangle(r(18, 9, 46, 55), radius=4 * S, fill=w)                     # the body
d.rounded_rectangle(r(22, 14, 42, 31), radius=2 * S, fill=mid)                   # the screen
d.rectangle(r(23, 40, 31, 42.5), fill=mid); d.rectangle(r(25.75, 37.25, 28.25, 45.25), fill=mid)   # the cross
d.ellipse(r(34, 41.5, 38, 45.5), fill=mid); d.ellipse(r(39, 37.5, 43, 41.5), fill=mid)             # B and A
im.resize((64, 64), Image.LANCZOS).save(os.path.join(HERE, 'icon.png'))
