#!/usr/bin/env python3
"""Draws the Speak2Mic icons: app/device icon (speaker -> waves -> microphone) and the
endpoint icons for Sound settings (speaker, microphone), all on the same gradient tile.

Large sizes show the full picture; 16-24 px use a simplified glyph (microphone + two waves)
so the icon stays readable in the taskbar and title bar.
    python3 make_icon.py [--preview preview.png]
"""
import sys
import numpy as np
from PIL import Image, ImageDraw, ImageFilter

W = 1024                     # master canvas, downscaled per icon size
SS = 1                       # coordinates below are in 1024-space


def gradient_tile(size, radius):
    """Rounded square with a diagonal violet -> cyan gradient and a soft top highlight."""
    y, x = np.mgrid[0:size, 0:size].astype(np.float32) / (size - 1)
    t = np.clip(0.65 * y + 0.35 * x, 0, 1)[..., None]
    c0 = np.array([108, 59, 255], np.float32)     # violet
    c1 = np.array([0, 194, 255], np.float32)      # cyan
    rgb = c0 * (1 - t) + c1 * t
    # glossy highlight in the upper half
    hl = np.clip(1 - y * 2.2, 0, 1)[..., None] * 38
    rgb = np.clip(rgb + hl, 0, 255).astype(np.uint8)
    img = Image.fromarray(np.dstack([rgb, np.full((size, size), 255, np.uint8)]), 'RGBA')
    mask = Image.new('L', (size, size), 0)
    m = int(size * 0.035)
    ImageDraw.Draw(mask).rounded_rectangle((m, m, size - m, size - m), radius=radius, fill=255)
    img.putalpha(mask)
    return img


def draw_mic(d, cx, top, w, h, col, stroke):
    """Capsule microphone with holder arc, stem and base. (cx, top) = capsule top center."""
    d.rounded_rectangle((cx - w / 2, top, cx + w / 2, top + h), radius=w / 2, fill=col)
    # holder: U-shaped arc around the lower part of the capsule
    pad = stroke * 1.6
    box = (cx - w / 2 - pad, top + h * 0.35, cx + w / 2 + pad, top + h + pad)
    d.arc(box, start=0, end=180, fill=col, width=int(stroke))
    stem_top = top + h + pad
    stem_bot = stem_top + h * 0.28
    d.rectangle((cx - stroke / 2, stem_top - stroke / 2, cx + stroke / 2, stem_bot), fill=col)
    d.rounded_rectangle((cx - w * 0.62, stem_bot - stroke / 2, cx + w * 0.62, stem_bot + stroke / 2),
                        radius=stroke / 2, fill=col)


def draw_grille(d, cx, top, w, h, col, stroke):
    for i in range(3):
        y = top + h * (0.26 + i * 0.14)
        d.rounded_rectangle((cx - w * 0.28, y - stroke / 2, cx + w * 0.28, y + stroke / 2),
                            radius=stroke / 2, fill=col)


def draw_waves(d, cx, cy, radii, width, alphas):
    """Right-facing arcs ")))" centered on (cx, cy)."""
    for r, a in zip(radii, alphas):
        d.arc((cx - r, cy - r, cx + r, cy + r), start=-48, end=48, fill=(255, 255, 255, a), width=int(width))


def draw_speaker(d, x, cy, s, col):
    """Speaker: small box + cone opening to the right."""
    bw, bh = s * 0.42, s * 0.62
    d.rounded_rectangle((x, cy - bh / 2, x + bw, cy + bh / 2), radius=s * 0.08, fill=col)
    d.polygon([(x + bw - 2, cy - bh / 2), (x + s, cy - s * 0.95), (x + s, cy + s * 0.95),
               (x + bw - 2, cy + bh / 2)], fill=col)


def glyph_layer(simple):
    g = Image.new('RGBA', (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(g)
    white = (255, 255, 255, 255)
    if simple:
        # Microphone centered-right, two thick waves on the left.
        draw_mic(d, cx=700, top=165, w=230, h=420, col=white, stroke=66)
        draw_waves(d, cx=170, cy=380, radii=[120, 240], width=74, alphas=[255, 255])
    else:
        draw_speaker(d, x=150, cy=450, s=190, col=white)
        draw_waves(d, cx=290, cy=450, radii=[105, 175, 245], width=40, alphas=[255, 210, 160])
        draw_mic(d, cx=750, top=235, w=200, h=340, col=white, stroke=40)
        draw_grille(d, cx=750, top=235, w=200, h=340, col=(108, 70, 235, 255), stroke=22)
    return g


def render(simple):
    tile = gradient_tile(W, radius=int(W * 0.22))
    g = glyph_layer(simple)
    # soft drop shadow under the white glyphs
    shadow = Image.new('RGBA', (W, W), (0, 0, 0, 0))
    alpha = g.split()[3].point(lambda v: v * 0.45)
    shadow.putalpha(alpha)
    shadow = shadow.filter(ImageFilter.GaussianBlur(18))
    out = tile.copy()
    out.alpha_composite(shadow, (0, 14))
    out.alpha_composite(g)
    # keep everything inside the rounded tile
    out.putalpha(Image.fromarray(np.minimum(np.array(out.split()[3]), np.array(tile.split()[3]))))
    return out


def glyph_endpoint(kind):
    """Single-purpose glyphs for the sound endpoints: 'speaker' or 'mic'."""
    g = Image.new('RGBA', (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(g)
    white = (255, 255, 255, 255)
    if kind == 'speaker':
        draw_speaker(d, x=257, cy=512, s=250, col=white)
        draw_waves(d, cx=462, cy=512, radii=[125, 215, 305], width=58, alphas=[255, 225, 180])
    else:
        draw_mic(d, cx=512, top=185, w=270, h=440, col=white, stroke=58)
        draw_grille(d, cx=512, top=185, w=270, h=440, col=(108, 70, 235, 255), stroke=30)
    return g


def render_glyph(g):
    tile = gradient_tile(W, radius=int(W * 0.22))
    shadow = Image.new('RGBA', (W, W), (0, 0, 0, 0))
    shadow.putalpha(g.split()[3].point(lambda v: v * 0.45))
    shadow = shadow.filter(ImageFilter.GaussianBlur(18))
    out = tile.copy()
    out.alpha_composite(shadow, (0, 14))
    out.alpha_composite(g)
    out.putalpha(Image.fromarray(np.minimum(np.array(out.split()[3]), np.array(tile.split()[3]))))
    return out


def add_setup_badge(img):
    """Installer variant: the same icon with a green round badge and a white download arrow in the lower-right
    corner (as Show2Cam's installer), so Speak2Mic-Setup.exe and the control panel are told apart."""
    out = img.copy()
    d = ImageDraw.Draw(out)
    d.ellipse((610, 610, 960, 960), fill=(30, 200, 90, 255), outline=(255, 255, 255, 255), width=28)
    d.rectangle((755, 680, 815, 820), fill=(255, 255, 255, 255))
    d.polygon([(700, 800), (870, 800), (785, 890)], fill=(255, 255, 255, 255))
    return out


SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]


def save_ico(paths, big, small=None):
    frames = [((small or big) if s <= 24 else big).resize((s, s), Image.LANCZOS) for s in SIZES]
    for path in paths:
        frames[-1].save(path, format='ICO', sizes=[(s, s) for s in SIZES], append_images=frames[:-1])
    print('written:', ', '.join(paths))


def main():
    full, simple = render(False), render(True)
    speaker = render_glyph(glyph_endpoint('speaker'))
    mic = render_glyph(glyph_endpoint('mic'))

    # Application and driver/device icon; endpoint icons for Sound settings (driver resources 101/102).
    save_ico(['s2mpanel.ico', '../driver/Speak2Mic.ico'], full, simple)
    save_ico(['s2msetup.ico'], add_setup_badge(full), add_setup_badge(simple))
    save_ico(['../driver/Speaker.ico'], speaker)
    save_ico(['../driver/Mic.ico'], mic)

    if '--preview' in sys.argv:
        path = sys.argv[sys.argv.index('--preview') + 1]
        show = [128, 48, 32, 24, 16]
        sets = [(full, simple), (add_setup_badge(full), add_setup_badge(simple)), (speaker, speaker), (mic, mic)]
        pw = (sum(show) + 20 * len(show)) * len(sets) + 40
        for bg, name in (((243, 243, 243), ''), ((32, 32, 32), '_dark')):
            sheet = Image.new('RGBA', (pw, 168), bg + (255,))
            x = 20
            for big, small in sets:
                for s in show:
                    f = (small if s <= 24 else big).resize((s, s), Image.LANCZOS)
                    sheet.alpha_composite(f, (x, 20 + (128 - s)))
                    x += s + 20
                x += 20
            sheet.convert('RGB').save(path.replace('.png', name + '.png'))
        print('preview:', path)


if __name__ == '__main__':
    main()
