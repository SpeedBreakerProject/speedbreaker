#!/usr/bin/env python3
# make_icon.py [out.png]: SpeedBreaker's app icon, 1024x1024 (needs Pillow):
# an amber speedometer, its needle past the redline, on a dark rounded
# square in macOS's icon grid (everything clipped to it). No game artwork. scripts/macos/AppIcon.png is
# its output, kept in the repo so make_app.sh needs no Pillow.
import math, sys
from PIL import Image, ImageDraw, ImageFilter

S = 1024
SS = 4  # supersampling
W = S * SS
out = sys.argv[1] if len(sys.argv) > 1 else 'AppIcon.png'
AMBER = (255, 168, 31)      # the settings menu's accent (ui.cpp kAccent)
RED = (232, 64, 40)

def rounded_mask(size, inset, radius):
    m = Image.new('L', (size, size), 0)
    ImageDraw.Draw(m).rounded_rectangle([inset, inset, size - inset, size - inset], radius, fill=255)
    return m

# macOS's grid: an 824-pixel rounded square (radius ~185) centred in 1024.
inset, radius = 100 * SS, 185 * SS
img = Image.new('RGBA', (W, W), (0, 0, 0, 0))
# Background: a vertical charcoal gradient.
bg = Image.new('RGBA', (W, W))
d = ImageDraw.Draw(bg)
for y in range(W):
    t = y / W
    c = (int(38 - 22 * t), int(40 - 23 * t), int(46 - 26 * t), 255)
    d.line([(0, y), (W, y)], fill=c)
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
d.polygon([p1, tip, p2], fill=(255, 196, 92, 255))
hub = 40 * SS
d.ellipse([cx - hub, cy - hub, cx + hub, cy + hub], fill=(28, 29, 34, 255), outline=AMBER + (255,), width=8 * SS)
# Nothing outside the rounded square.
clip = Image.new('RGBA', (W, W), (0, 0, 0, 0))
clip.paste(img, (0, 0), rounded_mask(W, inset, radius))
img = clip
img = img.resize((S, S), Image.LANCZOS)
img.save(out)
print(out)
