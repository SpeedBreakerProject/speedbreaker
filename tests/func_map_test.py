#!/usr/bin/env python3
"""survey/func_map.py and survey/region_map.py's find_extra_functions on two
made-up images (no game data):

    python3 -I tests/func_map_test.py <repo>

The "USA" image holds a few dozen .pdata functions (address loads, calls,
returns). The "other" one is the same build moved the way a PAL build moves:
its functions shifted in pieces, one function's code changed, one dropped,
one added, the address halves of its loads relocated, and a leaf function
with a jump table (no .pdata entry, called by the changed function) of the
shape the Spanish disc's date and number formats have. Checks: every function
paired with its counterpart and the right status, the dropped one missing,
the added one reported; the leaf function and its table found, with its
bounds, base, index register, default and labels; neither a table the
translated USA tables cover nor one the USA build has too (moved) reported.
"""
import pathlib
import random
import struct
import sys
import tempfile

BASE = 0x82000000
TEXT = 0x82100000
PDATA = 0x82080000
RDATA = 0x82000600
DATA_ADDR = 0x82900000


def lis(rd, imm):
    return (15 << 26) | (rd << 21) | (imm & 0xFFFF)


def addi(rd, ra, simm):
    return (14 << 26) | (rd << 21) | (ra << 16) | (simm & 0xFFFF)


def lwz(rd, d, ra):
    return (32 << 26) | (rd << 21) | (ra << 16) | (d & 0xFFFF)


def cmplwi(cr, ra, imm):
    return (10 << 26) | (cr << 23) | (ra << 16) | imm


def bgt(cr, pc, target):
    return (16 << 26) | (12 << 21) | ((4 * cr + 1) << 16) | ((target - pc) & 0xFFFC)


def rlwinm2(ra, rs):
    return (21 << 26) | (rs << 21) | (ra << 16) | (2 << 11) | (0 << 6) | (29 << 1)


def lwzx(rd, ra, rb):
    return (31 << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (23 << 1)


def mtctr(rs):
    return 0x7C0903A6 | (rs << 21)


def bl(pc, target):
    return (18 << 26) | ((target - pc) & 0x03FFFFFC) | 1


BCTR, BLR = 0x4E800420, 0x4E800020


def body(seed, n):
    """A made-up function body: arithmetic, a load from .data, a return."""
    rnd = random.Random(seed)
    ws = [addi(rnd.randrange(3, 12), rnd.randrange(3, 12), rnd.randrange(-500, 500)) for _ in range(n)]
    return ws


def build(functions, data_shift, leaves=(), layout_gap=None):
    """functions: [(key, words-maker)] in order -> (image bytes, {key: addr},
    {key: length}, sections). Each maker(addr, where, shift) returns the
    words; `where` maps keys to addresses (for calls). leaves: [(key, maker(addr))],
    leaf functions after them, with no .pdata entry. layout_gap: {index:
    bytes} of padding before that function."""
    where, addr = {}, TEXT
    sizes = {}
    for idx, (key, maker) in enumerate(functions):
        addr += (layout_gap or {}).get(idx, 0)
        n = len(maker(addr, {k: TEXT for k, _ in list(functions) + list(leaves)}, data_shift))
        where[key] = addr
        sizes[key] = 4 * n
        addr += 4 * n + 8  # 2 words of padding (zeros) between functions
    end = addr
    for key, leaf in leaves:
        where[key] = end
        end += 4 * len(leaf(end)) + 8
    img = bytearray(0x00A00000)
    for key, maker in functions:
        ws = maker(where[key], where, data_shift)
        struct.pack_into(f'>{len(ws)}I', img, where[key] - BASE, *ws)
    for key, leaf in leaves:
        ws = leaf(where[key])
        struct.pack_into(f'>{len(ws)}I', img, where[key] - BASE, *ws)
    # .pdata: every function but the leaf one
    for i, (key, _) in enumerate(functions):
        struct.pack_into('>II', img, PDATA - BASE + 8 * i, where[key], (sizes[key] // 4) << 8)
    text_end = (end + 0xFFF) & ~0xFFF
    sections = [('.rdata', RDATA, PDATA, 'data'), ('.pdata', PDATA, PDATA + 8 * len(functions), 'data'),
                ('.text', TEXT, text_end, 'code'), ('.data', DATA_ADDR, DATA_ADDR + 0x1000, 'data')]
    return bytes(img), where, sizes, sections


def write(d, img, sections):
    d.mkdir(parents=True, exist_ok=True)
    (d / 'image.bin').write_bytes(img)
    lines = ['## Image', '', '| Section | Start | End | Size | Kind |', '|---|---|---|---|---|']
    for name, lo, hi, kind in sections:
        lines.append(f'| {name} | 0x{lo:08X} | 0x{hi:08X} | 0x{hi - lo:X} | {kind} |')
    (d / 'xexsurvey.txt').write_text('\n'.join(lines) + '\n')


def leaf_function(addr, lang=0x828FF0C8, n=14):
    """The Spanish disc's extra leaf functions' shape: load the language,
    bounds check, jump through a table right after the bctr, each case
    returns a string address; past the last, 0."""
    ws = [lis(11, (lang + 0x8000) >> 16), lwz(11, lang & 0xFFFF, 11), cmplwi(6, 11, n - 1), None,
          None, None, rlwinm2(0, 11), lwzx(0, 12, 0), mtctr(0), BCTR]
    table = addr + 4 * len(ws)
    cases = table + 4 * n
    labels = [cases + 12 * i for i in range(n)]
    default = cases + 12 * n
    ws[3] = bgt(6, addr + 12, default)
    ws[4] = lis(12, (table + 0x8000) >> 16)
    ws[5] = addi(12, 12, table & 0xFFFF)
    ws += labels
    for i in range(n):
        s = 0x82001000 + 0x10 * i
        ws += [lis(3, (s + 0x8000) >> 16), addi(3, 3, s & 0xFFFF), BLR]
    ws += [addi(3, 0, 0), BLR]
    return ws, {'base': addr + 16, 'r': 11, 'default': default, 'labels': labels, 'size': 4 * len(ws)}


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    root = pathlib.Path(sys.argv[1]).resolve()
    sys.path.insert(0, str(root / 'survey'))
    import func_map
    import region_map

    def plain(seed, n):
        def maker(addr, where, shift):
            # a .data load, whose low half the other build relocates
            return body(seed, n) + [lis(9, DATA_ADDR >> 16), lwz(3, 0x10 + shift, 9), BLR]
        return maker

    def caller(seed, n, callee):
        def maker(addr, where, shift):
            ws = body(seed, n)
            ws.append(bl(addr + 4 * len(ws), where[callee]))
            return ws + [BLR]
        return maker

    def changed_caller(seed, n, callee):
        # the other build's version: different code, and it calls the leaf
        def maker(addr, where, shift):
            ws = body(seed + 1000, n + 3)
            ws.append(bl(addr + 4 * len(ws), where.get('leaf', addr)))
            return ws + [BLR]
        return maker

    def shared_leaf(addr):
        # a leaf function with a table both builds have (moved in the other)
        return leaf_function(addr, lang=0x828FF0D0, n=5)[0]

    usa = [(f'f{i}', plain(i, 3 + i % 7)) for i in range(40)]
    usa[5] = ('f5', caller(5, 6, 'f30'))
    usa[12] = ('f12', caller(12, 4, 'f2'))
    other = list(usa)
    other[12] = ('f12', changed_caller(12, 4, 'f2'))       # changed
    del other[20]                                          # f20: only in the USA build
    other.insert(33, ('new', plain(999, 9)))                # only in the other build
    leaf_info = {}

    def leaf(addr):
        ws, info = leaf_function(addr)
        leaf_info.update(info)
        return ws

    fails = []
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        img_u, where_u, sizes_u, sec_u = build(usa, 0, leaves=[('shared', shared_leaf)])
        img_o, where_o, sizes_o, sec_o = build(other, 0x40, leaves=[('shared', shared_leaf), ('leaf', leaf)],
                                               layout_gap={8: 0x20, 25: 0x100})
        write(tmp / 'usa', img_u, sec_u)
        write(tmp / 'other', img_o, sec_o)
        rows, only = func_map.func_map(region_map.Image(tmp / 'usa'), region_map.Image(tmp / 'other'))
        by = {u: (p, st) for u, ul, p, pl, st in rows}
        for key, _ in usa:
            p, st = by[where_u[key]]
            if key == 'f20':
                if p is not None or st != 'missing':
                    fails.append(f'f20: {st} at {p}, expected missing')
                continue
            want = 'changed' if key == 'f12' else 'reloc-equal' if key not in ('f5',) else None
            if p != where_o[key]:
                fails.append(f'{key}: paired with {p and hex(p)}, expected {where_o[key]:#x}')
            if key == 'f12' and st != 'changed':
                fails.append(f'f12: {st}, expected changed')
            if key not in ('f12', 'f5') and st != 'reloc-equal':
                fails.append(f'{key}: {st}, expected reloc-equal (its data address moved)')
            if key == 'f5' and st != 'reloc-equal':
                fails.append(f'f5: {st}, expected reloc-equal (its call moved)')
        if only != [(where_o['new'], sizes_o['new'])]:
            fails.append(f'only in the other build: {only}, expected the added function')

        # The function map as region_map.py reads it.
        tsv = tmp / 'other' / 'funcmap.tsv'
        lines = ['# header'] + [f'0x{u:08X}\t' + (f'0x{p:08X}' if p is not None else '-') + f'\t{ul}\t'
                                + (str(pl) if pl is not None else '-') + f'\t{st}' for u, ul, p, pl, st in rows]
        tsv.write_text('\n'.join(lines) + '\n')
        rm = region_map.RegionMap(tmp / 'usa', tmp / 'other', tsv)
        f7 = where_u['f7']
        try:
            p, how = rm.code(f7 + 4)
            if p != where_o['f7'] + 4:
                fails.append(f'region_map.code(f7+4): {p:#x}, expected {where_o["f7"] + 4:#x}')
        except ValueError as e:
            fails.append(f'region_map.code(f7+4): {e}')

        # The leaf function only the other build has, and its table (not the
        # one both have, which the USA config doesn't list here either).
        found = region_map.find_extra_functions(rm, '')
        leafs = [f for f in found if f[0] is not None]
        if len(leafs) != 1:
            fails.append(f'find_extra_functions: {len(leafs)} leaf functions, expected 1 ({found})')
        else:
            a, z, base, r, default, labels = leafs[0]
            want = (where_o['leaf'], leaf_info['size'], leaf_info['base'], 11, leaf_info['default'], leaf_info['labels'])
            if (a, z, base, r, default, labels) != want:
                fails.append(f'find_extra_functions: {(a, z, base, r, default)}, expected {want[:5]}')
        # Covered by a (translated) table: not reported.
        found = region_map.find_extra_functions(rm, f'base = 0x{leaf_info["base"]:08X}\n')
        if found:
            fails.append(f'find_extra_functions with the table covered: {found}')

    for f in fails:
        print('FAIL', f)
    print('func_map_test:', 'FAILED' if fails else 'all checks passed')
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
