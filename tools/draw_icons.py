#!/usr/bin/env python3
"""Draws the Underglow Kit's garage icons into icons/ (then tools/gen_icons.py packs them into src/icon_data.h).

  python tools/draw_icons.py

  icons/kit.png, kit_LG.png           the Underglow Kit itself: the LED strip picture (tools/icon_src/led_strip.png)
  icons/<kit>_LG.png                   one per kit, its large picture: a car seen from above, glowing under the parts
                                       the kit has (front, back, sides, front_back, front_sides, back_sides, all);
                                       the kits' tiles are the game's own Forza logo

Small icons are 200x200, large ones 916x458 (see tools/icontools/FORMAT.md). The car is drawn in the game's own
style for part icons: light gray, see-through, dark gray edges. Needs pillow and numpy.
"""
import colorsys, os
from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'icons')
SRC = os.path.join(ROOT, 'tools', 'icon_src', 'led_strip.png')
SS = 4   # supersampling

KITS = {'front': 'F', 'back': 'B', 'sides': 'S', 'front_back': 'FB', 'front_sides': 'FS', 'back_sides': 'BS',
        'all': 'FBS'}


def rainbow(t):
    """The strip's colors along it: magenta, red, orange, green, cyan, blue, violet."""
    r, g, b = colorsys.hsv_to_rgb((0.85 + 0.9 * t) % 1.0, 0.85, 1.0)
    return int(r * 255), int(g * 255), int(b * 255)


def strip_line(draw, p0, p1, width, t0, t1, alpha=255, steps=60):
    for i in range(steps):
        a, b = i / steps, (i + 1) / steps
        x0, y0 = p0[0] + (p1[0] - p0[0]) * a, p0[1] + (p1[1] - p0[1]) * a
        x1, y1 = p0[0] + (p1[0] - p0[0]) * b, p0[1] + (p1[1] - p0[1]) * b
        draw.line([(x0, y0), (x1, y1)], fill=rainbow(t0 + (t1 - t0) * a) + (alpha,), width=width)


def car(w, h, parts):
    """A car from above, front to the right, in a w x h picture (already supersampled)."""
    L = min(w * 0.64, h * 1.62)       # length (room for the glow at both ends)
    W = L * 0.43                       # width
    cx, cy = w / 2, h / 2
    x0, x1, y0, y1 = cx - L / 2, cx + L / 2, cy - W / 2, cy + W / 2
    img = Image.new('RGBA', (w, h), (0, 0, 0, 0))

    # The strips' edges, just outside the body (the order runs round the car, for the colors).
    m = W * 0.10
    edges = []
    if 'F' in parts: edges.append(((x1 + m, y0 + W * 0.18), (x1 + m, y1 - W * 0.18), 0.00, 0.20))
    if 'S' in parts:
        edges.append(((x0 + L * 0.27, y1 + m), (x1 - L * 0.27, y1 + m), 0.25, 0.45))
        edges.append(((x0 + L * 0.27, y0 - m), (x1 - L * 0.27, y0 - m), 0.75, 0.95))
    if 'B' in parts: edges.append(((x0 - m, y0 + W * 0.18), (x0 - m, y1 - W * 0.18), 0.50, 0.70))

    # The glow on the ground, under everything.
    glow = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    for p0, p1, t0, t1 in edges: strip_line(gd, p0, p1, int(W * 0.26), t0, t1, 255)
    glow = glow.filter(ImageFilter.GaussianBlur(W * 0.11))
    for _ in range(3): img.alpha_composite(glow)   # three times: brighter

    d = ImageDraw.Draw(img)
    edge, lw = (70, 70, 70, 255), max(2, int(W * 0.022))
    # Wheels.
    tw, th = L * 0.16, W * 0.13
    for ax in (x0 + L * 0.20, x1 - L * 0.21):
        for yy in (y0 - th * 0.35, y1 - th * 0.65):
            d.rounded_rectangle([ax - tw / 2, yy, ax + tw / 2, yy + th], radius=th * 0.35, fill=(60, 60, 60, 255))
    # Body.
    d.rounded_rectangle([x0, y0, x1, y1], radius=W * 0.30, fill=(222, 222, 222, 215), outline=edge, width=lw)
    # Windshield, roof, rear window.
    ws = [(cx + L * 0.16, y0 + W * 0.12), (cx + L * 0.03, y0 + W * 0.20), (cx + L * 0.03, y1 - W * 0.20),
          (cx + L * 0.16, y1 - W * 0.12)]
    rw = [(cx - L * 0.25, y0 + W * 0.15), (cx - L * 0.17, y0 + W * 0.21), (cx - L * 0.17, y1 - W * 0.21),
          (cx - L * 0.25, y1 - W * 0.15)]
    d.polygon(ws, fill=(120, 120, 120, 230), outline=edge)
    d.polygon(rw, fill=(120, 120, 120, 230), outline=edge)
    d.rectangle([cx - L * 0.17, y0 + W * 0.21, cx + L * 0.03, y1 - W * 0.21], fill=(205, 205, 205, 235), outline=edge,
                width=max(1, lw // 2))
    # Hood lines and headlights / tail lights.
    d.line([(cx + L * 0.19, y0 + W * 0.22), (x1 - L * 0.05, y0 + W * 0.28)], fill=edge, width=max(1, lw // 2))
    d.line([(cx + L * 0.19, y1 - W * 0.22), (x1 - L * 0.05, y1 - W * 0.28)], fill=edge, width=max(1, lw // 2))
    for yy in (y0 + W * 0.08, y1 - W * 0.20):
        d.rounded_rectangle([x1 - L * 0.045, yy, x1 - L * 0.012, yy + W * 0.12], radius=W * 0.03, fill=(250, 250, 250, 255),
                            outline=edge)
        d.rounded_rectangle([x0 + L * 0.012, yy, x0 + L * 0.040, yy + W * 0.12], radius=W * 0.03, fill=(140, 140, 140, 255),
                            outline=edge)
    # The strips themselves, crisp, over the glow.
    for p0, p1, t0, t1 in edges: strip_line(d, p0, p1, max(3, int(W * 0.045)), t0, t1)
    return img


def save(img, size, name):
    img.resize(size, Image.LANCZOS).save(os.path.join(OUT, name))


def strip_icon(size, margin):
    src = Image.open(SRC).convert('RGBA')
    w, h = size
    pic = src.copy()
    pic.thumbnail((w - 2 * margin, h - 2 * margin), Image.LANCZOS)
    img = Image.new('RGBA', size, (0, 0, 0, 0))
    img.alpha_composite(pic, ((w - pic.width) // 2, (h - pic.height) // 2))
    return img


def main():
    os.makedirs(OUT, exist_ok=True)
    strip_icon((200, 200), 10).save(os.path.join(OUT, 'kit.png'))
    strip_icon((916, 458), 20).save(os.path.join(OUT, 'kit_LG.png'))
    for name, parts in KITS.items():
        save(car(916 * SS, 458 * SS, parts), (916, 458), name + '_LG.png')
    print('icons drawn in', OUT)


if __name__ == '__main__':
    main()
