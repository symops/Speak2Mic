#!/usr/bin/env python3
"""Draws the small toolbar icons of the control panel (dark glyphs on transparent):
reset.ico (circular arrow), export.ico (arrow up out of a tray), import.ico (arrow down into a tray),
mixer.ico (three sliders: the Windows volume mixer).
    python3 make_toolicons.py [--preview preview.png]
"""
import sys
from PIL import Image, ImageDraw

W = 256
COL = (55, 60, 75, 255)
SIZES = [16, 20, 24, 32, 40, 48]


def tray(d):
    w = 26
    d.line([(40, 150), (40, 216), (216, 216), (216, 150)], fill=COL, width=w, joint='curve')
    for x, y in ((40, 150), (40, 216), (216, 216), (216, 150)):
        d.ellipse((x - w / 2, y - w / 2, x + w / 2, y + w / 2), fill=COL)


def arrow(d, up):
    w = 26
    if up:
        d.rectangle((128 - w / 2, 70, 128 + w / 2, 170), fill=COL)
        d.polygon([(72, 100), (184, 100), (128, 28)], fill=COL)
    else:
        d.rectangle((128 - w / 2, 28, 128 + w / 2, 128), fill=COL)
        d.polygon([(72, 100), (184, 100), (128, 172)], fill=COL)


def reset(d):
    w = 28
    box = (44, 44, 212, 212)
    d.arc(box, start=-40, end=250, fill=COL, width=w)
    # arrowhead at the arc's open end (upper right), pointing clockwise
    d.polygon([(150, 18), (218, 60), (150, 104)], fill=COL)


def mixer(d):
    # three sliders: vertical tracks with knobs at different heights
    for x, knob in ((64, 150), (128, 90), (192, 170)):
        d.rounded_rectangle((x - 9, 28, x + 9, 228), radius=9, fill=COL)
        d.rounded_rectangle((x - 30, knob - 18, x + 30, knob + 18), radius=10, fill=COL)


def trash(d):
    # waste bin: lid with handle, body with ribs
    d.rounded_rectangle((40, 52, 216, 76), radius=10, fill=COL)
    d.rounded_rectangle((100, 26, 156, 58), radius=10, outline=COL, width=18)
    d.polygon([(58, 88), (198, 88), (184, 232), (72, 232)], fill=COL)
    for x in (100, 128, 156):
        d.rounded_rectangle((x - 7, 112, x + 7, 208), radius=7, fill=(0, 0, 0, 0))


def make(fn):
    img = Image.new('RGBA', (W, W), (0, 0, 0, 0))
    fn(ImageDraw.Draw(img))
    return img


def save(path, img):
    frames = [img.resize((s, s), Image.LANCZOS) for s in SIZES]
    frames[-1].save(path, format='ICO', sizes=[(s, s) for s in SIZES], append_images=frames[:-1])
    print('written:', path)


def main():
    icons = {
        'reset.ico': make(reset),
        'export.ico': make(lambda d: (tray(d), arrow(d, True))),
        'import.ico': make(lambda d: (tray(d), arrow(d, False))),
        'mixer.ico': make(mixer),
        'clear.ico': make(trash),
    }
    for path, img in icons.items():
        save(path, img)
    if '--preview' in sys.argv:
        out = sys.argv[sys.argv.index('--preview') + 1]
        sheet = Image.new('RGBA', (5 * 120 + 40, 90), (240, 240, 240, 255))
        x = 20
        for img in icons.values():
            for s in (48, 24, 16):
                sheet.alpha_composite(img.resize((s, s), Image.LANCZOS), (x, 20 + (48 - s)))
                x += s + 8
            x += 12
        sheet.convert('RGB').save(out)
        print('preview:', out)


if __name__ == '__main__':
    main()
