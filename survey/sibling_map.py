#!/usr/bin/env python3
"""Another PAL region's tables from a sibling PAL region's committed ones,
checked against the new region's image only.

    python3 -I survey/sibling_map.py pieces <img dir> [--from pal-es]
        prints the layout pieces (sibling address range, delta) found
    python3 -I survey/sibling_map.py write  <img dir> --region pal-uk [--from pal-es]
        writes config/<region>/{nfsmw.toml,switch_tables.toml} and
        runtime/region/<stem>_{addresses.inc,names.h}
    python3 -I survey/sibling_map.py check  <img dir> --region pal-uk [--from pal-es]
        the same, compared with the committed files (exit 1 on a difference)

<img dir>: image.bin and xexsurvey.txt for the new region's default.xex
(survey/xexsurvey.cpp; from the player's own disc, never committed).

survey/region_map.py maps USA addresses and checks each one against both
images' bytes. A player with only the new disc has neither the USA nor the
sibling image, so this maps the sibling's committed addresses instead
(config/<from>/, runtime/region/<from>_addresses.inc) and checks them against
the new image alone:

  - anchors: every absolute jump table the sibling's config lists (its labels
    are absolute code addresses) is searched in the new image with each
    candidate delta; a table found with exactly one delta proves that delta
    from its base to its last label. Inline tables (the addresses right after
    the bctr) are found the same way.
  - pieces: consecutive anchors with one delta make a piece. Between two
    pieces lies the code the new build changed; an address there is only
    mapped when the new image's .pdata settles it (see `resolve`), else it is
    an error naming it.
  - checks on the result: the compiler helpers' byte patterns, every function
    start the runtime or the config names is a .pdata start (or a leaf
    function's) in the new image, the .data/.tls/.XEXID sections where the
    sibling's are (the USA's: survey/leaf_switch_bounds.py DATA), .rdata
    items by their references in the new image's code.

XenonRecomp's own jump-table analysis over the generated config is the final
check (README step 2): no "outside function" and no "no switch table entry".
"""
import bisect
import collections
import pathlib
import re
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'survey'))
sys.path.insert(0, str(ROOT / 'runtime/region'))
from region_map import BASE, HEX, Image, decode_jump_table, is_exit, bl_targets  # noqa: E402
import hook_addresses  # noqa: E402

DELTAS = range(-0x400, 0x404, 4)
# The USA's (and every PAL build's so far) data sections.
DATA_SECTIONS = {'.data': (0x828D0000, 0x82C8CE8C)}

# Anchors found by hand, for addresses between two pieces (the code the new
# build changed lies there): (sibling lo, sibling hi, delta, the evidence in
# the new image). The layout's deltas only grow (each change adds code), so
# an anchor also settles everything between it and the piece of its delta.
HAND_ANCHORS = {
    ('pal-es', 'pal-uk'): [
        # Leaf 0x822585B0 (size 0xE8): ends on its `b 0x82284938` only at +0
        # (at +0x10 its end is a li).
        (0x822585B0, 0x82258694, 0x0, 'leaf 0x822585B0 ends on its b only at +0'),
        # WorldStepTime / WorldSteps (sub_823A2348 in the USA build): the
        # hook's `fadds f0,f10,f9` is at 0x823A23D0, and r30 (the steps)
        # reaches `mr r4,r30` at 0x823A240C.
        (0x823A23C0, 0x823A23FC, 0x10, 'fadds f0,f10,f9 at 0x823A23D0'),
        # GameFrameTime: `lwz r3,80(r1); bl <the frame>` at 0x823B02CC; the
        # boot's `li r3,0; bl SetCurrentLanguage` returns to 0x823B4A68.
        (0x823B02A8, 0x823B4A40, 0x28, 'the frame call at 0x823B02D0, the boot\'s SetCurrentLanguage call at 0x823B4A64'),
        # Leaf 0x82626978 (size 0x84): 0x826269C0..0x82626A44 whole, its jump
        # table's labels inside it.
        (0x82626978, 0x826269F8, 0x48, 'leaf 0x82626978 whole at 0x826269C0, its table inside'),
        # The USA's 0x82630180 (the sibling's 0x82630370) is the .pdata
        # function 0x826303C0, and .rdata 0x820B0AB8 holds its secondary
        # entry 0x82630514 (nothing holds the +0x48 address).
        (0x82630370, 0x826304C4, 0x50, '.pdata start 0x826303C0; .rdata 0x820B0AB8 holds 0x82630514'),
    ],
}

# What each region is called in the generated files' comments, and its
# default.xex's SHA-256.
REGION_INFO = {
    'pal-es': ('PAL (Spanish)', 'Europe/PAL, Spanish', hook_addresses.PAL_ES_XEX_SHA256),
    'pal-de': ('PAL (German)', 'Europe/PAL, German', hook_addresses.PAL_DE_XEX_SHA256),
    'pal-uk': ('PAL (English, UK)', 'Europe/PAL, English (UK)', getattr(hook_addresses, 'PAL_UK_XEX_SHA256', None)),
}


def parse_switches(text):
    """[(line index of 'base =', base, default, labels, section)]."""
    out, section = [], None
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        line = lines[i]
        if line.startswith('# ----'):
            section = line.strip('# -')
        if line == '[[switch]]':
            j = i + 1
            fields, labels = {}, []
            while j < len(lines) and lines[j] and not lines[j].startswith('[['):
                m = re.match(r'(\w+) = (0x[0-9A-Fa-f]+|\d+)', lines[j])
                if m:
                    fields[m.group(1)] = int(m.group(2), 0)
                labels += [int(x, 16) for x in HEX.findall(lines[j].partition('#')[0])] if lines[j].startswith('    ') else []
                if lines[j] == ']':
                    break
                j += 1
            out.append((i, fields.get('base'), fields.get('default'), labels, section))
            i = j
        i += 1
    return out


class SiblingMap:
    def __init__(self, img_dir, sibling, analyse=None, region=None):
        self.img = Image(img_dir)
        self.analyse = pathlib.Path(analyse) if analyse else None
        self.hand = HAND_ANCHORS.get((sibling, region), [])
        self.sibling = sibling
        self.sib_toml = (ROOT / f'config/{sibling}/nfsmw.toml').read_text()
        self.sib_sw = (ROOT / f'config/{sibling}/switch_tables.toml').read_text()
        self.funcs = self.img.functions()
        self.fstarts = [f[0] for f in self.funcs]
        self.fset = set(self.fstarts)
        code = [(lo, hi) for _, lo, hi, kind in self.img.sections if kind == 'code']
        self.code_lo, self.code_hi = min(c[0] for c in code), max(c[1] for c in code)
        # Every code address stored in the image, by value: where it is.
        self.where = collections.defaultdict(list)
        data = self.img.data
        for o in range(0, len(data) - 3, 4):
            w = struct.unpack_from('>I', data, o)[0]
            if self.code_lo <= w < self.code_hi:
                self.where[w].append(BASE + o)
        self._anchors()

    # --- anchors and pieces ------------------------------------------------
    def find_table(self, labels, d):
        """Where the new image holds `labels` + d in a row, or None."""
        want = [l + d for l in labels]
        hits = []
        for at in self.where.get(want[0], ()):
            if list(self.img.words(at, len(want)) or ()) == want:
                hits.append(at)
        return hits

    def _anchors(self):
        anchors, multi, self.conflicts = [], [], []
        for _, base, default, labels, section in parse_switches(self.sib_sw):
            if section not in ('ABSOLUTE JUMPTABLE', 'INLINE (config/switch_tables_inline.toml)', 'MANUAL (survey/fix_switch_tables.py)'):
                continue
            uniq = sorted(set(labels))
            if len(uniq) < 2:
                continue
            ds = [d for d in DELTAS if self.find_table(labels, d)]
            # default = 0 is "none" in some generated entries.
            span = [x for x in [base, default] + labels if x is not None and x >= BASE]
            if len(ds) == 1:
                anchors.append((min(span), max(span), ds[0], f'jump table at sibling {base:#010x}'))
            elif len(ds) > 1:
                # The same switch code more than once in the image (one
                # template, several functions): its delta is its unique
                # neighbours', when they agree and it is one of the hits.
                multi.append((min(span), max(span), ds, base))
            else:
                self.conflicts.append(f'sibling table {base:#010x}: not found in the new image with any delta')
        # XenonAnalyse's tables for the new image (every kind: absolute,
        # computed, offset), when given: a sibling table whose base and
        # labels, moved by one delta, are one of them.
        if self.analyse:
            auto = {}
            for _, base, default, labels, _ in parse_switches(self.analyse.read_text()):
                if base is not None:
                    auto[base] = labels
            have = {a[3] for a in anchors}
            for _, base, default, labels, section in parse_switches(self.sib_sw):
                if base is None or not labels or f'jump table at sibling {base:#010x}' in have:
                    continue
                ds = [d for d in DELTAS if auto.get(base + d) == [l + d for l in labels]]
                span = [x for x in [base, default] + labels if x is not None and x >= BASE]
                if len(ds) == 1:
                    anchors.append((min(span), max(span), ds[0], f'jump table at sibling {base:#010x} (XenonAnalyse\'s)'))
        anchors.sort()
        starts = [a[0] for a in anchors]
        for lo, hi, ds, base in multi:
            k = bisect.bisect_right(starts, lo)
            near = {anchors[j][2] for j in (k - 1, k) if 0 <= j < len(anchors)}
            pick = [d for d in ds if d in near]
            if len(near) == 1 and len(pick) == 1:
                anchors.append((lo, hi, pick[0], f'jump table at sibling {base:#010x} (repeated code; its neighbours\' delta)'))
            else:
                self.conflicts.append(f'sibling table {base:#010x}: found with deltas {", ".join(hex(d) for d in ds)}, '
                                      f'neighbours {", ".join(hex(d) for d in sorted(near))}: not used')
        anchors += [(lo, hi, d, f'by hand: {how}') for lo, hi, d, how in self.hand]
        anchors.sort()
        # The deltas only grow along the image (each change adds code): a
        # smaller delta after a larger one is a wrong anchor.
        for a, b in zip(anchors, anchors[1:]):
            if b[2] < a[2]:
                self.conflicts.append(f'{b[3]} ({b[2]:+#x}) after {a[3]} ({a[2]:+#x}): the delta shrinks')
        # Overlapping anchors must agree.
        for a, b in zip(anchors, anchors[1:]):
            if b[0] <= a[1] and a[2] != b[2]:
                self.conflicts.append(f'{a[3]} ({a[2]:+#x}) overlaps {b[3]} ({b[2]:+#x})')
        pieces = []
        for lo, hi, d, how in anchors:
            if pieces and pieces[-1][2] == d:
                pieces[-1][1] = max(pieces[-1][1], hi)
                pieces[-1][3] += 1
            else:
                pieces.append([lo, hi, d, 1])
        self.pieces = pieces
        self.piece_lo = [p[0] for p in pieces]

    def piece_of(self, x):
        """(index, inside): the piece holding x, or the one before it."""
        k = bisect.bisect_right(self.piece_lo, x) - 1
        return k, k >= 0 and x <= self.pieces[k][1]

    def ufunc(self, a):
        """The new image's .pdata function holding a, or None."""
        k = bisect.bisect_right(self.fstarts, a) - 1
        if k >= 0 and self.funcs[k][0] <= a < self.funcs[k][0] + self.funcs[k][1]:
            return self.funcs[k]
        return None

    def resolve(self, x, start=False):
        """(new address, how) for the sibling's code address x. Raises
        ValueError with the reason when the pieces don't settle it."""
        k, inside = self.piece_of(x)
        if inside:
            d = self.pieces[k][2]
            return self._checked(x, d, start, f'piece {k} ({d:+#x})')
        before = self.pieces[k] if k >= 0 else None
        after = self.pieces[k + 1] if k + 1 < len(self.pieces) else None
        cands = []
        if before:
            cands.append(('before', before[2], before))
        if after and (not before or after[2] != before[2]):
            cands.append(('after', after[2], after))
        # The piece's edge extends to the end (start) of the new image's
        # function holding its last (first) anchor: one function is changed
        # or not as a whole.
        for side, d, p in cands:
            edge = p[1] if side == 'before' else p[0]
            f = self.ufunc(edge + d)
            if f and f[0] <= x + d < f[0] + f[1]:
                return self._checked(x, d, start, f'beside piece ({d:+#x}), in the function holding its {"last" if side == "before" else "first"} anchor')
        # A function start: the delta under which it is one.
        if start:
            ok = [(d, side) for side, d, _ in cands if x + d in self.fset]
            if len(ok) == 1:
                return x + ok[0][0], f'between pieces; a .pdata start only with the {ok[0][1]} piece\'s delta {ok[0][0]:+#x}'
        gap = f'{before[1]:#010x}..{after[0]:#010x}' if before and after else 'an edge of the layout'
        raise ValueError(f'{x:#010x}: between pieces ({gap}, deltas ' + ', '.join(f'{d:+#x}' for _, d, _ in cands) + '): check by hand')

    def _checked(self, x, d, start, how):
        y = x + d
        if start and y not in self.fset:
            # A leaf function (no .pdata): it must follow an exit or padding.
            # A leaf function (no .pdata): it follows an exit or padding, or
            # a call (survey/leaf_switch_bounds.py starts a leaf at its latest
            # entry, which may be a bl's return).
            prev = self.img.words(y - 4, 1)
            if prev and (is_exit(prev[0]) or prev[0] == 0):
                how += ', leaf start'
            elif prev and prev[0] >> 26 == 18 and prev[0] & 1:
                how += ', leaf start after a bl'
            else:
                raise ValueError(f'{x:#010x} -> {y:#010x} ({how}): not a function start in the new image')
        elif start:
            how += ', .pdata start'
        return y, how

    # --- the sibling's addresses ------------------------------------------
    def code(self, x, start=False):
        return self.resolve(x, start)


def function_starts(toml_text):
    """Sibling addresses its config names as function starts."""
    out = set()
    for m in re.finditer(r'\{ address = (0x[0-9A-F]+), size = (0x[0-9A-F]+) \}', toml_text):
        out.add(int(m.group(1), 16))
    for key in ('longjmp_address', 'setjmp_address') + tuple(f'{a}{b}_{n}_address' for a in ('rest', 'save') for b, n in (('gprlr', 14), ('fpr', 14), ('vmx', 14), ('vmx', 64))):
        m = re.search(rf'^{key} = (0x[0-9A-F]+)', toml_text, re.M)
        if m:
            out.add(int(m.group(1), 16))
    return out


def main():
    args = sys.argv[1:]
    opt = {}
    for k in ('--region', '--from', '--analyse'):
        if k in args:
            i = args.index(k)
            opt[k] = args[i + 1]
            del args[i:i + 2]
    if len(args) != 2 or args[0] not in ('pieces', 'write', 'check'):
        sys.exit(__doc__)
    sm = SiblingMap(args[1], opt.get('--from', 'pal-es'), opt.get('--analyse'), opt.get('--region'))
    print(f'{len(sm.pieces)} piece(s):')
    for i, (lo, hi, d, n) in enumerate(sm.pieces):
        print(f'  {i:2}: {lo:#010x}..{hi:#010x} {d:+#06x} ({n} anchors)')
    for c in sm.conflicts:
        print('  conflict:', c)
    if args[0] == 'pieces':
        return
    import sibling_write
    sys.exit(0 if sibling_write.write(sm, opt['--region'], args[0] == 'check') else 1)


if __name__ == '__main__':
    main()
