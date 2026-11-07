#!/usr/bin/env python3
"""Draws the app's icons as crisp 1-bit-alpha PNGs, in the plain geometric style of the
PebbleOS system icons.

Run with any Python that has Pillow (pebble-tool's own Python has it):
    ~/.local/share/uv/tools/pebble-tool/bin/python tools/make_icons.py
"""

import math
import os

from PIL import Image, ImageDraw

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'resources', 'images')
SS = 8  # supersampling factor

WHITE = (255, 255, 255, 255)
BLACK = (0, 0, 0, 255)


def canvas(size):
    img = Image.new('L', (size * SS, size * SS), 0)
    return img, ImageDraw.Draw(img)


def s(v):
    return int(round(v * SS))


def save(mask, size, name, color):
    small = mask.resize((size, size), Image.LANCZOS)
    alpha = small.point(lambda a: 255 if a >= 110 else 0)
    out = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    solid = Image.new('RGBA', (size, size), color)
    out.paste(solid, (0, 0), alpha)
    out.save(os.path.join(OUT, name + '.png'), optimize=True)


def circle(d, cx, cy, r, fill=255):
    d.ellipse([s(cx - r), s(cy - r), s(cx + r), s(cy + r)], fill=fill)


def rect(d, x0, y0, x1, y1, fill=255, radius=0):
    if radius:
        d.rounded_rectangle([s(x0), s(y0), s(x1), s(y1)], radius=s(radius), fill=fill)
    else:
        d.rectangle([s(x0), s(y0), s(x1), s(y1)], fill=fill)


def poly(d, pts, fill=255):
    d.polygon([(s(x), s(y)) for x, y in pts], fill=fill)


def line(d, pts, width, fill=255):
    d.line([(s(x), s(y)) for x, y in pts], fill=fill, width=s(width), joint='curve')
    for x, y in (pts[0], pts[-1]):
        circle(d, x, y, width / 2.0, fill)


# --- Action bar icons: 18 x 18, ~15 px visual core ---------------------------------------------

def ab_record(d):
    circle(d, 9, 9, 7.2)


def ab_pause(d):
    rect(d, 3.5, 2.5, 7.3, 15.5, radius=0.8)
    rect(d, 10.7, 2.5, 14.5, 15.5, radius=0.8)


def ab_stop(d):
    rect(d, 3, 3, 15, 15, radius=1.6)


def ab_play(d):
    poly(d, [(4.5, 2.2), (15.8, 9), (4.5, 15.8)])


def ab_rewind(d):
    poly(d, [(9, 3.5), (1.5, 9), (9, 14.5)])
    poly(d, [(16.5, 3.5), (9, 9), (16.5, 14.5)])


def ab_forward(d):
    poly(d, [(9, 3.5), (16.5, 9), (9, 14.5)])
    poly(d, [(1.5, 3.5), (9, 9), (1.5, 14.5)])


def ab_trash(d):
    rect(d, 6.5, 1.2, 11.5, 3.5, radius=0.8)
    rect(d, 2.2, 3, 15.8, 5.2, radius=0.6)
    poly(d, [(3.6, 6.4), (14.4, 6.4), (13.4, 16.8), (4.6, 16.8)])
    for x in (6.9, 9.0, 11.1):
        rect(d, x - 0.55, 8.3, x + 0.55, 14.9, fill=0)


def ab_list(d):
    for y in (4, 9, 14):
        circle(d, 3, y, 1.7)
        rect(d, 6.5, y - 1.2, 16, y + 1.2, radius=0.8)


def ab_settings(d):
    cx = cy = 9
    teeth = 8
    pts = []
    for i in range(teeth * 4):
        a = (i / (teeth * 4.0)) * 2 * math.pi
        r = 8.2 if (i % 4) in (0, 1) else 6.1
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    poly(d, pts)
    circle(d, cx, cy, 6.3)
    circle(d, cx, cy, 2.7, fill=0)


def speaker(d):
    rect(d, 1.2, 6.3, 4.6, 11.7, radius=0.6)
    poly(d, [(4, 6.3), (9.2, 2.2), (9.2, 15.8), (4, 11.7)])


def ab_vol_up(d):
    speaker(d)
    rect(d, 10.6, 8, 17, 10, radius=0.5)
    rect(d, 12.8, 5.8, 14.8, 12.2, radius=0.5)


def ab_vol_down(d):
    speaker(d)
    rect(d, 10.6, 8, 17, 10, radius=0.5)


def ab_check(d):
    line(d, [(2.8, 9.6), (7, 13.8), (15.4, 4.4)], 2.8)


def ab_cross(d):
    line(d, [(3.5, 3.5), (14.5, 14.5)], 2.8)
    line(d, [(14.5, 3.5), (3.5, 14.5)], 2.8)


def ab_upload(d):
    poly(d, [(9, 1.5), (15, 8), (11, 8), (11, 12.5), (7, 12.5), (7, 8), (3, 8)])
    rect(d, 2.5, 14.3, 15.5, 16.5, radius=0.8)


def ab_text(d):
    for i, (y, w) in enumerate(((3.5, 15), (7.5, 15), (11.5, 15), (15.5, 9.5))):
        rect(d, 2, y - 1.1, 2 + w - 1.5, y + 1.1, radius=0.7)


# --- Dialog icons: 40 x 40 ---------------------------------------------------------------------

def dlg_trash(d):
    rect(d, 15, 3, 25, 7.5, radius=1.5)
    rect(d, 6, 6.5, 34, 11, radius=1.2)
    poly(d, [(8.5, 13.5), (31.5, 13.5), (29.5, 37), (10.5, 37)])
    for x in (15, 20, 25):
        rect(d, x - 1.2, 17.5, x + 1.2, 32.5, fill=0)


def dlg_check(d):
    circle(d, 20, 20, 18.5)
    line(d, [(10.5, 20.5), (17, 27), (29.5, 13.5)], 4.2, fill=0)


def dlg_warning(d):
    d.polygon([(s(20), s(2.5)), (s(38.5), s(36.5)), (s(1.5), s(36.5))], fill=255)
    rect(d, 18.2, 13, 21.8, 26.5, fill=0, radius=1.6)
    circle(d, 20, 31, 2.2, fill=0)


def dlg_phone(d):
    rect(d, 10, 2, 30, 38, radius=3.5)
    rect(d, 12.5, 7, 27.5, 31, fill=0)
    circle(d, 20, 34.6, 1.8, fill=0)
    line(d, [(4, 6), (36, 36)], 3.6, fill=0)
    line(d, [(4, 3.2), (36, 33.2)], 2.6, fill=255)


def dlg_mic(d):
    rect(d, 13, 2, 27, 25, radius=7)
    d.arc([s(7.5), s(10), s(32.5), s(31)], start=0, end=180, fill=255, width=s(3))
    rect(d, 18.5, 30, 21.5, 35.5)
    rect(d, 12, 34.5, 28, 37.5, radius=1.2)


# --- Menu icons --------------------------------------------------------------------------------

def menu_check(d):
    line(d, [(2.2, 7.6), (5.6, 11), (11.8, 3.6)], 2.2)


def app_icon(d):
    # 25 x 25 launcher icon: a microphone
    rect(d, 8.5, 1.5, 16.5, 15, radius=4)
    d.arc([s(5), s(6.5), s(20), s(19)], start=0, end=180, fill=255, width=s(2))
    rect(d, 11.6, 18.5, 13.4, 22)
    rect(d, 8, 21.5, 17, 23.3, radius=0.8)


ICONS = [
    # (name, size, drawer, variants: [(suffix, color)])
    ('ab_record', 18, ab_record, [('', WHITE)]),
    ('ab_pause', 18, ab_pause, [('', WHITE)]),
    ('ab_stop', 18, ab_stop, [('', WHITE)]),
    ('ab_play', 18, ab_play, [('', WHITE)]),
    ('ab_rewind', 18, ab_rewind, [('', WHITE)]),
    ('ab_forward', 18, ab_forward, [('', WHITE)]),
    ('ab_trash', 18, ab_trash, [('', WHITE)]),
    ('ab_list', 18, ab_list, [('', WHITE)]),
    ('ab_settings', 18, ab_settings, [('', WHITE)]),
    ('ab_vol_up', 18, ab_vol_up, [('', WHITE)]),
    ('ab_vol_down', 18, ab_vol_down, [('', WHITE)]),
    ('ab_check', 18, ab_check, [('', WHITE)]),
    ('ab_cross', 18, ab_cross, [('', WHITE)]),
    ('dlg_trash', 40, dlg_trash, [('~color', WHITE), ('~bw', BLACK)]),
    ('dlg_check', 40, dlg_check, [('~color', WHITE), ('~bw', BLACK)]),
    ('dlg_warning', 40, dlg_warning, [('~color', WHITE), ('~bw', BLACK)]),
    ('dlg_phone', 40, dlg_phone, [('~color', WHITE), ('~bw', BLACK)]),
    ('dlg_mic', 40, dlg_mic, [('~color', WHITE), ('~bw', BLACK)]),
    ('menu_check_black', 14, menu_check, [('', BLACK)]),
    ('menu_check_white', 14, menu_check, [('', WHITE)]),
    ('app_icon', 25, app_icon, [('', BLACK)]),
]


def main():
    os.makedirs(OUT, exist_ok=True)
    for name, size, drawer, variants in ICONS:
        mask, d = canvas(size)
        drawer(d)
        for suffix, color in variants:
            save(mask, size, name + suffix, color)
    print('Wrote %d icons to %s' % (sum(len(v) for *_, v in ICONS), os.path.normpath(OUT)))


if __name__ == '__main__':
    main()
