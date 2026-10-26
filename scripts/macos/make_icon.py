#!/usr/bin/env python3
# make_icon.py [out.png] [--corner=...] [--redline=...] [--glow=...]:
# SpeedBreaker's app icon, 1024x1024 (needs Pillow): an amber speedometer,
# its needle past the red redline, on a dark rounded square in macOS's icon
# grid (everything clipped to it), lit by a burnt-orange glow bleeding in
# from the lower right. No game artwork. scripts/macos/AppIcon.png is its
# output, kept in the repo so make_app.py needs no Pillow. The flags only
# try other looks:
#   --corner=R,G,B,STRENGTH,RADIUS,DARK[,DX,DY]  the glow: its colour, STRENGTH
#       0-1 at its origin, RADIUS the fraction of the icon it fades over, DARK
#       scaling the charcoal under it, DX,DY moving its origin from the
#       square's lower-right corner (fractions of the square: + right, + down);
#       --corner=none: no glow
#   --redline=R,G,B  the redline's colour, or none (the arc keeps its accent)
#   --glow=R,G,B,STRENGTH,HEIGHT  with --corner=none, a glow rising from the
#       bottom edge instead, over HEIGHT of the icon
# scripts/ios/make_icon.py draws the same icon for iOS; a change to the
# drawing goes in both scripts.
import math, sys
from PIL import Image, ImageDraw, ImageFilter

S = 1024
SS = 4  # supersampling
W = S * SS


def flag(name, default):
    """--name=a,b,c as a tuple of numbers (or the word itself, say none)."""
    for a in sys.argv[1:]:
        if a.startswith(f'--{name}='):
            v = a.split('=', 1)[1]
            return v if v == 'none' else tuple(float(x) for x in v.split(','))
    return default


args = [a for a in sys.argv[1:] if not a.startswith('--')]
out = args[0] if args else 'AppIcon.png'
AMBER = (255, 168, 31)      # the settings menu's accent (ui.cpp kAccent)
NEEDLE = (255, 196, 92)
RED = flag('redline', (232, 64, 40))
RED = AMBER if RED == 'none' else tuple(int(x) for x in RED)
CORNER = flag('corner', (240, 110, 30, 0.55, 0.85, 1.0, -0.22, 0.18))
GLOW = flag('glow', None)


def rounded_mask(size, inset, radius):
    m = Image.new('L', (size, size), 0)
    ImageDraw.Draw(m).rounded_rectangle([inset, inset, size - inset, size - inset], radius, fill=255)
    return m


def corner_glow(bg, inset, corner):
    """bg with the glow from its lower-right corner, strongest at its origin
    and fading as (1 - distance/reach)^2.2: Pillow's radial gradient (a
    256-pixel square, each pixel its distance from the centre, 255 at the
    corners' 181) scaled so its inscribed circle is the reach, through that
    curve, as the mask of the glow's colour over the (darkened) charcoal."""
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


# macOS's grid: an 824-pixel rounded square (radius ~185) centred in 1024.
inset, radius = 100 * SS, 185 * SS
img = Image.new('RGBA', (W, W), (0, 0, 0, 0))
# Background: a vertical charcoal gradient, under the glow.
bg = Image.new('RGBA', (W, W))
d = ImageDraw.Draw(bg)
for y in range(W):
    t = y / W
    c = (int(38 - 22 * t), int(40 - 23 * t), int(46 - 26 * t), 255)
    if CORNER == 'none' and GLOW and t > 1 - GLOW[4]:  # --glow: rising from the bottom edge
        k = GLOW[3] * ((t - (1 - GLOW[4])) / GLOW[4]) ** 2
        c = tuple(int(c[i] + (GLOW[i] - c[i]) * k) for i in range(3)) + (255,)
    d.line([(0, y), (W, y)], fill=c)
if CORNER != 'none':
    bg = corner_glow(bg, inset, CORNER)
img.paste(bg, (0, 0), rounded_mask(W, inset, radius))
d = ImageDraw.Draw(img)

cx, cy = W // 2, int(W * 0.535)
r = int(W * 0.30)
start, end = 135, 405  # the dial: 270 degrees, open at the bottom
# Outer ring and the arc.
d.arc([cx - r - 18 * SS, cy - r - 18 * SS, cx + r + 18 * SS, cy + r + 18 * SS], start, end, fill=(70, 72, 80, 255), width=6 * SS)
d.arc([cx - r, cy - r, cx + r, cy + r], start, end - 60, fill=AMBER + (255,), width=26 * SS)
d.arc([cx - r, cy - r, cx + r, cy + r], end - 60, end, fill=RED + (255,), width=26 * SS)
# Ticks.
for i in range(11):
    a = math.radians(start + i * 27)
    r0, r1 = r - 44 * SS, r - (70 if i % 2 == 0 else 58) * SS
    d.line([(cx + r0 * math.cos(a), cy + r0 * math.sin(a)), (cx + r1 * math.cos(a), cy + r1 * math.sin(a))],
           fill=(235, 235, 240, 255), width=(10 if i % 2 == 0 else 6) * SS)
# The needle, past the redline, with a soft glow.
a = math.radians(end - 22)
tip = (cx + (r - 30 * SS) * math.cos(a), cy + (r - 30 * SS) * math.sin(a))
perp = a + math.pi / 2
base_w = 22 * SS
p1 = (cx + base_w * math.cos(perp), cy + base_w * math.sin(perp))
p2 = (cx - base_w * math.cos(perp), cy - base_w * math.sin(perp))
glow = Image.new('RGBA', (W, W), (0, 0, 0, 0))
ImageDraw.Draw(glow).polygon([p1, tip, p2], fill=AMBER + (200,))
glow = glow.filter(ImageFilter.GaussianBlur(14 * SS))
img = Image.alpha_composite(img, glow)
d = ImageDraw.Draw(img)
d.polygon([p1, tip, p2], fill=NEEDLE + (255,))
hub = 40 * SS
d.ellipse([cx - hub, cy - hub, cx + hub, cy + hub], fill=(28, 29, 34, 255), outline=AMBER + (255,), width=8 * SS)
# Nothing outside the rounded square.
clip = Image.new('RGBA', (W, W), (0, 0, 0, 0))
clip.paste(img, (0, 0), rounded_mask(W, inset, radius))
img = clip
img = img.resize((S, S), Image.LANCZOS)
img.save(out)
print(out)
