#!/usr/bin/env python3
# Reads what tests/report_zip_test.cpp wrote with independent code: every
# deflate stream through zlib, the archive through zipfile (and its entries
# against the originals), the PNG decoded by hand (chunks, CRCs, zlib,
# filters) and compared pixel for pixel.
#   python3 tests/report_zip_check.py build/report_zip_out
import os
import struct
import sys
import zipfile
import zlib

out = sys.argv[1]
failures = 0


def fail(message):
    global failures
    failures += 1
    print("  FAIL", message)


i = 0
while os.path.exists(os.path.join(out, f"raw_{i}.bin")):
    raw = open(os.path.join(out, f"raw_{i}.bin"), "rb").read()
    packed = open(os.path.join(out, f"deflate_{i}.bin"), "rb").read()
    try:
        d = zlib.decompressobj(-15)
        got = d.decompress(packed) + d.flush()
        if got != raw:
            fail(f"case {i}: inflated {len(got)} bytes differ from the {len(raw)} put in")
        if d.unused_data:
            fail(f"case {i}: {len(d.unused_data)} bytes after the final block")
    except zlib.error as e:
        fail(f"case {i}: zlib: {e}")
    i += 1
print(f"deflate: {i} cases inflated")

with zipfile.ZipFile(os.path.join(out, "test.zip")) as z:
    bad = z.testzip()
    if bad:
        fail(f"zip: bad CRC in {bad}")
    names = z.namelist()
    ref = os.path.join(out, "zipref")
    for name in names:
        want = open(os.path.join(ref, name), "rb").read()
        if z.read(name) != want:
            fail(f"zip: {name} differs")
        info = z.getinfo(name)
        if (info.external_attr >> 16) & 0o777 != 0o644:
            fail(f"zip: {name} permissions {oct(info.external_attr >> 16)}")
    print(f"zip: {len(names)} entries match ({', '.join(f'{n}:{z.getinfo(n).compress_type}' for n in names)})")

png = open(os.path.join(out, "test.png"), "rb").read()
w, h = map(int, open(os.path.join(out, "test.size")).read().split())
rgb = open(os.path.join(out, "test.rgb"), "rb").read()
if png[:8] != b"\x89PNG\r\n\x1a\n":
    fail("png: signature")
pos, idat, header = 8, b"", None
while pos < len(png):
    length, kind = struct.unpack(">I4s", png[pos:pos + 8])
    data = png[pos + 8:pos + 8 + length]
    crc = struct.unpack(">I", png[pos + 8 + length:pos + 12 + length])[0]
    if zlib.crc32(kind + data) != crc:
        fail(f"png: {kind} CRC")
    if kind == b"IHDR":
        header = struct.unpack(">IIBBBBB", data)
    elif kind == b"IDAT":
        idat += data
    pos += 12 + length
if header != (w, h, 8, 2, 0, 0, 0):
    fail(f"png: IHDR {header}")
raw = zlib.decompress(idat)  # checks the zlib header and Adler-32 too
stride = w * 3
pixels = bytearray()
prev = bytearray(stride)
for y in range(h):
    kind = raw[y * (stride + 1)]
    line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
    for x in range(stride):
        a = line[x - 3] if x >= 3 else 0
        b = prev[x]
        c = prev[x - 3] if x >= 3 else 0
        if kind == 1:
            p = a
        elif kind == 2:
            p = b
        elif kind == 3:
            p = (a + b) // 2
        elif kind == 4:
            pp = a + b - c
            pa, pb, pc = abs(pp - a), abs(pp - b), abs(pp - c)
            p = a if pa <= pb and pa <= pc else b if pb <= pc else c
        else:
            p = 0
        line[x] = (line[x] + p) & 0xFF
    pixels += line
    prev = line
if bytes(pixels) != rgb:
    fail("png: pixels differ")
print(f"png: {w}x{h} decoded, pixels match")

print("ok" if failures == 0 else f"FAILED ({failures})")
sys.exit(1 if failures else 0)
