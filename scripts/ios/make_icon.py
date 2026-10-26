#!/usr/bin/env python3
# SpeedBreaker. GPL-3.0-or-later (see COPYING).
#
# make_icon.py [out.png] [--corner=...] [--redline=...]: SpeedBreaker's iOS
# app icon (needs Pillow), in
#   SpeedBreaker/Assets.xcassets/AppIcon.appiconset/AppIcon.png  1024x1024
#   SpeedBreaker/Assets.xcassets/AppIcon.appiconset/Contents.json
# next to this script: one size, from which Xcode makes the rest. With
# out.png, only that file (a preview, say). The look and the flags are
# scripts/macos/make_icon.py's (amber, with a burnt-orange glow); --glow
# isn't drawn here. iOS wants
# a full-bleed, opaque square and rounds the corners itself, so this is
# scripts/macos/make_icon.py's drawing without its rounded mask and with
# heavier strokes (K, below), cut to the macOS icon's 824-pixel square and
# scaled to fill the 1024, so the dial
# sits in the iOS tile as it sits in the macOS one. Drawn again rather than
# cut from scripts/macos/AppIcon.png, whose corners are transparent and
# whose square would have to be scaled up. The catalog is kept in the repo,
# so building the app needs no Pillow. A change to the drawing goes in both
# scripts.
import json, math, os, sys
from PIL import Image, ImageDraw, ImageFilter

S = 1024
SS = 4  # supersampling
W = S * SS
CATALOG = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'SpeedBreaker', 'Assets.xcassets')


def flag(name, default):
    """--name=a,b,c as a tuple of numbers (or the word itself, say none)."""
    for a in sys.argv[1:]:
        if a.startswith(f'--{name}='):
            v = a.split('=', 1)[1]
            return v if v == 'none' else tuple(float(x) for x in v.split(','))
    return default


args = [a for a in sys.argv[1:] if not a.startswith('--')]
out = args[0] if args else None
AMBER = (255, 168, 31)      # the settings menu's accent (ui.cpp kAccent)
NEEDLE = (255, 196, 92)
# Strokes 1.5x the macOS icon's: its hairline ring and ticks thin out at the
# 60-180 px an iPhone or iPad shows (compared at those sizes, 2026-10-02).
K = 1.5
RED = flag('redline', (232, 64, 40))
RED = AMBER if RED == 'none' else tuple(int(x) for x in RED)
CORNER = flag('corner', (240, 110, 30, 0.55, 0.85, 1.0, -0.22, 0.18))


def corner_glow(bg, inset, corner):
    """bg with the glow from the macOS square's lower-right corner, as on
    macOS (see there), over the whole canvas."""
    rgb, strength, rad, dark = tuple(int(v) for v in corner[:3]), *corner[3:6]
    dx, dy = corner[6:8] if len(corner) > 7 else (0.0, 0.0)
    span = W - 2 * inset
    ox, oy, reach = W - inset + dx * span, W - inset + dy * span, rad * span * 1.15
    size = round(2 * reach)
    dist = Image.new('L', (W, W), 255)  # past the reach: no glow
    dist.paste(Image.radial_gradient('L').resize((size, size), Image.BILINEAR), (round(ox - reach), round(oy - reach)))
    mask = dist.point([round(255 * strength * max(0.0, 1 - v * 181 / 255 / 128) ** 2.2) for v in range(256)])
    if dark != 1.0:
        bg = bg.point([int(v * dark) for v in range(256)] * 3 + list(range(256)))
    return Image.composite(Image.new('RGBA', (W, W), rgb + (255,)), bg, mask)


# The drawing, as on macOS: the same canvas and geometry, with the
# background over all of it instead of only the rounded square.
inset = 100 * SS  # where macOS's 824-pixel square starts
# Background: a vertical charcoal gradient, under the glow.
img = Image.new('RGBA', (W, W))
d = ImageDraw.Draw(img)
for y in range(W):
    t = y / W
    c = (int(38 - 22 * t), int(40 - 23 * t), int(46 - 26 * t), 255)
    d.line([(0, y), (W, y)], fill=c)
if CORNER != 'none':
    img = corner_glow(img, inset, CORNER)
    d = ImageDraw.Draw(img)

cx, cy = W // 2, int(W * 0.535)
r = int(W * 0.30)
start, end = 135, 405  # the dial: 270 degrees, open at the bottom
# Outer ring and the arc.
ring = (18 + 6 * (K - 1)) * SS
d.arc([cx - r - ring, cy - r - ring, cx + r + ring, cy + r + ring], start, end, fill=(70, 72, 80, 255), width=int(6 * K * SS))
d.arc([cx - r, cy - r, cx + r, cy + r], start, end - 60, fill=AMBER + (255,), width=int(26 * K * SS))
d.arc([cx - r, cy - r, cx + r, cy + r], end - 60, end, fill=RED + (255,), width=int(26 * K * SS))
# Ticks.
for i in range(11):
    a = math.radians(start + i * 27)
    inner = 44 + 13 * (K - 1)  # clear of the wider arc
    r0, r1 = r - inner * SS, r - (inner + (26 if i % 2 == 0 else 14) * (1 + 0.25 * (K - 1))) * SS
    d.line([(cx + r0 * math.cos(a), cy + r0 * math.sin(a)), (cx + r1 * math.cos(a), cy + r1 * math.sin(a))],
           fill=(235, 235, 240, 255), width=int((10 if i % 2 == 0 else 6) * K * SS))
# The needle, past the redline, with a soft glow.
a = math.radians(end - 22)
tip = (cx + (r - 30 * SS) * math.cos(a), cy + (r - 30 * SS) * math.sin(a))
perp = a + math.pi / 2
base_w = 22 * K * SS
p1 = (cx + base_w * math.cos(perp), cy + base_w * math.sin(perp))
p2 = (cx - base_w * math.cos(perp), cy - base_w * math.sin(perp))
glow = Image.new('RGBA', (W, W), (0, 0, 0, 0))
ImageDraw.Draw(glow).polygon([p1, tip, p2], fill=AMBER + (200,))
glow = glow.filter(ImageFilter.GaussianBlur(14 * SS))
img = Image.alpha_composite(img, glow)
d = ImageDraw.Draw(img)
d.polygon([p1, tip, p2], fill=NEEDLE + (255,))
hub = int(40 * (1 + 0.15 * (K - 1)) * SS)
d.ellipse([cx - hub, cy - hub, cx + hub, cy + hub], fill=(28, 29, 34, 255), outline=AMBER + (255,), width=int(8 * K * SS))

# The macOS square, cut from the supersampled canvas so nothing is scaled
# up. No alpha channel: an app icon with one is rejected on upload.
img = img.crop((inset, inset, W - inset, W - inset)).resize((S, S), Image.LANCZOS).convert('RGB')
if out:
    img.save(out)
    print(out)
    sys.exit()

def write_json(path, value):
    # Xcode's own layout, so opening the catalog in Xcode changes nothing.
    with open(path, 'w') as f:
        f.write(json.dumps(value, indent=2, separators=(',', ' : '), sort_keys=True) + '\n')

info = {'author': 'xcode', 'version': 1}
iconset = os.path.join(CATALOG, 'AppIcon.appiconset')
os.makedirs(iconset, exist_ok=True)
png = os.path.join(iconset, 'AppIcon.png')
img.save(png)
write_json(os.path.join(CATALOG, 'Contents.json'), {'info': info})
write_json(os.path.join(iconset, 'Contents.json'), {
    'images': [{'filename': 'AppIcon.png', 'idiom': 'universal', 'platform': 'ios', 'size': '1024x1024'}],
    'info': info,
})
print(png)
