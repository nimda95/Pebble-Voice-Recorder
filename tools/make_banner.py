"""Draws the 720x320 appstore banner for one platform from its emulator screenshots.

  python tools/make_banner.py PLATFORM FONT_DIR OUT.png

FONT_DIR must hold BarlowCondensed-Bold.ttf and Barlow-Medium.ttf (Google Fonts, OFL).
Screens come from screenshots/<platform>/ and are drawn at whole-pixel scales so the watch fonts
stay crisp.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

W, H = 720, 320
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RED = (227, 84, 98)  # GColorRed as the screenshots render it
BEZEL = (24, 24, 27)

# Per platform: screens (file, scale, x, y), left to right.
LAYOUTS = {
    'emery': [('02-recording', 1, 336, 40), ('09-transcript', 1, 572, 40)],
    'flint': [('02-recording', 2, 372, 14)],
    'gabbro': [('02-recording', 1, 392, 18)],
}


def gradient():
    top, bottom = (236, 100, 112), (198, 62, 78)
    im = Image.new('RGB', (W, H))
    px = im.load()
    for y in range(H):
        for x in range(W):
            t = min(1.0, max(0.0, (x / W) * 0.45 + (y / H) * 0.55))
            px[x, y] = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3))
    return im


def screen(platform, name, scale):
    im = Image.open(os.path.join(ROOT, 'screenshots', platform, name + '.png')).convert('RGBA')
    if scale != 1:
        im = im.resize((im.width * scale, im.height * scale), Image.NEAREST)
    return im


def framed(im, round_screen, pad):
    """The screen inside a plain dark bezel, plus its alpha mask."""
    w, h = im.width + 2 * pad, im.height + 2 * pad
    out = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(out)
    if round_screen:
        d.ellipse((0, 0, w - 1, h - 1), fill=BEZEL + (255,))
        mask = Image.new('L', im.size, 0)
        ImageDraw.Draw(mask).ellipse((0, 0, im.width - 1, im.height - 1), fill=255)
        out.paste(im, (pad, pad), mask)
    else:
        d.rounded_rectangle((0, 0, w - 1, h - 1), radius=pad + 8, fill=BEZEL + (255,))
        out.paste(im, (pad, pad), im)
    return out


def shadow(size, round_screen, blur=14, alpha=90):
    w, h = size
    m = 30
    sh = Image.new('RGBA', (w + 2 * m, h + 2 * m), (0, 0, 0, 0))
    d = ImageDraw.Draw(sh)
    box = (m, m, m + w, m + h)
    if round_screen:
        d.ellipse(box, fill=(60, 0, 10, alpha))
    else:
        d.rounded_rectangle(box, radius=20, fill=(60, 0, 10, alpha))
    return sh.filter(ImageFilter.GaussianBlur(blur)), m


def waves(canvas, cx, cy):
    """Faint rings spreading from behind the watch, like sound."""
    layer = Image.new('RGBA', canvas.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    for i, r in enumerate(range(150, 520, 62)):
        a = max(0, 34 - i * 5)
        d.ellipse((cx - r, cy - r, cx + r, cy + r), outline=(255, 255, 255, a), width=2)
    canvas.alpha_composite(layer)


def mic_badge(canvas, x, y, size):
    d = ImageDraw.Draw(canvas)
    d.ellipse((x, y, x + size - 1, y + size - 1), fill=(255, 255, 255, 255))
    glyph = Image.open(os.path.join(ROOT, 'resources', 'images', 'dlg_mic~bw.png')).convert('RGBA')
    tint = Image.new('RGBA', glyph.size, RED + (255,))
    tint.putalpha(glyph.getchannel('A'))
    canvas.alpha_composite(tint, (x + (size - glyph.width) // 2, y + (size - glyph.height) // 2))


def main():
    platform, font_dir, out = sys.argv[1], sys.argv[2], sys.argv[3]
    title_font = ImageFont.truetype(os.path.join(font_dir, 'BarlowCondensed-Bold.ttf'), 70)
    tag_font = ImageFont.truetype(os.path.join(font_dir, 'Barlow-Medium.ttf'), 21)
    round_screen = platform == 'gabbro'

    canvas = gradient().convert('RGBA')
    shots = LAYOUTS[platform]

    # Rings centred on the first (front) screen.
    name, scale, sx, sy = shots[0]
    first = screen(platform, name, scale)
    pad = 12 if round_screen else (10 if scale == 1 else 14)
    waves(canvas, sx + first.width // 2 + pad, sy + first.height // 2 + pad)

    # Back to front, so the first screen sits on top.
    for name, scale, sx, sy in reversed(shots):
        im = screen(platform, name, scale)
        f = framed(im, round_screen, pad)
        sh, m = shadow(f.size, round_screen)
        canvas.alpha_composite(sh, (sx - m, sy - m + 10))
        canvas.alpha_composite(f, (sx, sy))

    # Text column
    x = 44
    mic_badge(canvas, x, 40, 60)
    d = ImageDraw.Draw(canvas)
    d.text((x - 3, 104), 'Voice', font=title_font, fill='white')
    d.text((x - 3, 166), 'Recorder', font=title_font, fill='white')
    for i, line in enumerate(('Voice notes on your wrist,', 'transcribed with timestamps.')):
        d.text((x, 254 + i * 27), line, font=tag_font, fill=(255, 255, 255, 228))

    canvas.convert('RGB').save(out)


if __name__ == '__main__':
    main()
