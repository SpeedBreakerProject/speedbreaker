#!/usr/bin/env python3
"""The function map survey/region_map.py takes: every USA .pdata function and
its counterpart in another region's default.xex.

    python3 -I survey/func_map.py <usa dir> <other dir> > <other dir>/funcmap.tsv

<dir>: image.bin and xexsurvey.txt, survey/xexsurvey.cpp's output for each
disc's default.xex (decrypted from the player's own discs; never committed).
Writes one line per USA .pdata function, tab-separated:

    usa_addr  other_addr  usa_len  other_len  status

lengths in bytes; status exact (the same bytes), reloc-equal (the same code
once branch displacements and address halves are masked: region_map.norm),
changed (paired, other code) or missing (no counterpart: other_addr and
other_len are '-'). A summary goes to stderr: how many of each, the
functions only the other build has, and how many pieces the layout moved in
(the Spanish disc: 22,679 functions, all but 19 the same code, 17 pieces).

The builds come from one source tree, so their .pdata functions keep their
order: the two lists are aligned on each function's masked code and length
(difflib). Between two aligned runs, functions are paired in order when both
sides have as many, and otherwise by how alike their code is (a pair needs
half its masked instructions in common). region_map.py checks the code at
every address it maps, so a wrong pair here is an error there, not a wrong
address.
"""
import difflib
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from region_map import Image, norm  # noqa: E402

PAIR_RATIO = 0.5


def load(img):
    """[(start, length, raw words, masked words)] for the .pdata functions."""
    out = []
    for start, length in img.functions():
        ws = img.words(start, length // 4)
        if ws is None:
            continue
        out.append((start, length, ws, norm(ws)))
    return out


def ratio(a, b):
    return difflib.SequenceMatcher(None, a, b, autojunk=False).ratio()


def pair_block(us, ot):
    """Pairs (i, j) between two runs of unaligned functions, in order."""
    n, m = len(us), len(ot)
    if n == m:
        return [(i, i) for i in range(n)]
    if n * m > 4096:
        # Too many to compare all: in order, each with the most alike of the
        # next few.
        pairs, j = [], 0
        for i in range(n):
            best = max(((ratio(us[i][3], ot[k][3]), k) for k in range(j, min(m, j + 8))), default=(0, None))
            if best[1] is not None and best[0] >= PAIR_RATIO:
                pairs.append((i, best[1]))
                j = best[1] + 1
        return pairs
    # The order-keeping pairing with the most code in common.
    score = [[ratio(us[i][3], ot[j][3]) for j in range(m)] for i in range(n)]
    best = [[0.0] * (m + 1) for _ in range(n + 1)]
    for i in range(n - 1, -1, -1):
        for j in range(m - 1, -1, -1):
            take = score[i][j] + best[i + 1][j + 1] if score[i][j] >= PAIR_RATIO else -1
            best[i][j] = max(take, best[i + 1][j], best[i][j + 1])
    pairs, i, j = [], 0, 0
    while i < n and j < m:
        if score[i][j] >= PAIR_RATIO and best[i][j] == score[i][j] + best[i + 1][j + 1]:
            pairs.append((i, j))
            i, j = i + 1, j + 1
        elif best[i][j] == best[i + 1][j]:
            i += 1
        else:
            j += 1
    return pairs


def func_map(usa_img, other_img):
    """[(usa start, usa length, other start or None, other length or None,
    status)] for every USA function, and the other build's unpaired ones."""
    us, ot = load(usa_img), load(other_img)
    ka = [(f[1], hash(f[3])) for f in us]
    kb = [(f[1], hash(f[3])) for f in ot]
    paired = {}
    for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(None, ka, kb, autojunk=False).get_opcodes():
        if tag == 'equal':
            for k in range(i2 - i1):
                paired[i1 + k] = j1 + k
        elif tag == 'replace':
            for i, j in pair_block(us[i1:i2], ot[j1:j2]):
                paired[i1 + i] = j1 + j
    rows = []
    for i, (u, ul, uw, un) in enumerate(us):
        j = paired.get(i)
        if j is None:
            rows.append((u, ul, None, None, 'missing'))
            continue
        p, pl, pw, pn = ot[j]
        status = 'exact' if uw == pw else 'reloc-equal' if un == pn else 'changed'
        rows.append((u, ul, p, pl, status))
    used = set(paired.values())
    only = [(ot[j][0], ot[j][1]) for j in range(len(ot)) if j not in used]
    return rows, only


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    rows, only = func_map(Image(sys.argv[1]), Image(sys.argv[2]))
    out = ['# usa_addr\tother_addr\tusa_len\tother_len\tstatus (survey/func_map.py)']
    for u, ul, p, pl, st in rows:
        out.append(f'0x{u:08X}\t' + (f'0x{p:08X}' if p is not None else '-') + f'\t{ul}\t' +
                   (str(pl) if pl is not None else '-') + f'\t{st}')
    sys.stdout.write('\n'.join(out) + '\n')

    counts = {}
    for *_, st in rows:
        counts[st] = counts.get(st, 0) + 1
    pieces, last = [], None
    for u, ul, p, pl, st in rows:
        if p is not None and p - u != last:
            last = p - u
            pieces.append(last)
    print(f'{len(rows)} USA functions: ' + ', '.join(f'{counts.get(k, 0)} {k}' for k in ('exact', 'reloc-equal', 'changed', 'missing'))
          + f'; {len(only)} only in the other build', file=sys.stderr)
    if pieces:
        print(f'{len(pieces)} piece(s) of unchanged layout, moved by {min(pieces):+#x}..{max(pieces):+#x}', file=sys.stderr)
    for u, ul, p, pl, st in rows:
        if st == 'changed':
            print(f'  changed: 0x{u:08X} ({ul} bytes) -> 0x{p:08X} ({pl} bytes)', file=sys.stderr)
    for p, pl in only[:50]:
        print(f'  only in the other build: 0x{p:08X} ({pl} bytes)', file=sys.stderr)


if __name__ == '__main__':
    main()
