"""Find computed jumps (mtctr + bctr) that no switch table covers, and decode
the table-less-bounds-check pattern XenonAnalyse misses:

    lis rT,hi ; addi rT,rT,lo      table address (absolute entries)
    rlwinm r0,rI,2,0,29            index * 4
    lwzx r0,rT,r0 ; mtctr r0 ; bctr

Table length = consecutive entries that are code addresses inside the
function. Writes TOML [[switch]] entries to stdout; reports the rest.
"""
import bisect, re, struct, sys, tomllib
exec(open('survey/classify_switches.py').read().split("tables = sorted")[0])

# Tables from the analyzer + hand fixes only (not a previous run's inline
# section), so the output is always the complete set.
_text = open('config/switch_tables.toml').read().split('# ---- INLINE')[0]
tables = tomllib.loads(_text)['switch']
keys = sorted(t['base'] for t in tables)

def bctr_of(key):
    a = key
    while word(a) != 0x4E800420 and a - key < 0x80:
        a += 4
    return a
covered = set(bctr_of(k) for k in keys)

fnstarts = sorted(int(x, 16) for x in re.findall(r'\{ 0x([0-9A-F]+),', open('ppc/ppc_func_mapping.cpp').read()))

def function_bounds(a):
    """The .pdata function containing `a`, or for leaf code (no .pdata) the
    gap between .pdata functions: XenonRecomp's own guesses split leaf
    functions at their inline tables, so they can't be trusted here."""
    i = bisect.bisect_right(starts, a) - 1
    if i >= 0 and funcs[i][0] <= a < funcs[i][1]:
        return funcs[i]
    return (funcs[i][1], funcs[i + 1][0])

found, unknown = [], []
for bctr in range(TEXT[0], TEXT[1], 4):
    if word(bctr) != 0x4E800420 or bctr in covered:
        continue
    fb, fe = function_bounds(bctr)
    # Walk back up to 12 instructions for the pattern.
    mtctr = word(bctr - 4)
    if mtctr >> 26 != 31 or ((mtctr >> 1) & 0x3FF) != 467:
        unknown.append((bctr, 'no mtctr before bctr'))
        continue
    rs = (mtctr >> 21) & 31
    lwzx = None
    for a in range(bctr - 8, bctr - 48, -4):
        w = word(a)
        if w >> 26 == 31 and ((w >> 1) & 0x3FF) == 23 and (w >> 21) & 31 == rs:  # lwzx rs,rA,rB
            lwzx = (a, (w >> 16) & 31, (w >> 11) & 31)
            break
    if lwzx is None:
        unknown.append((bctr, 'target not loaded with lwzx (computed or offset table)'))
        continue
    la, ra, rb = lwzx
    base = None
    index_reg = None
    for a in range(la - 4, la - 48, -4):
        w = word(a)
        op = w >> 26
        if op == 14 and (w >> 21) & 31 == ra and base is None:            # addi rA,rX,lo
            lo = w & 0xFFFF
            lo = lo - 0x10000 if lo & 0x8000 else lo
            src = (w >> 16) & 31
            for b in range(a - 4, a - 40, -4):
                x = word(b)
                if x >> 26 == 15 and (x >> 21) & 31 == src and (x >> 16) & 31 == 0:  # lis src,hi
                    base = (((x & 0xFFFF) << 16) + lo) & 0xFFFFFFFF
                    break
        if op == 21 and (w >> 16) & 31 == rb and index_reg is None:        # rlwinm rb,rI,2,...
            if (w >> 11) & 31 == 2:
                index_reg = (w >> 21) & 31
    if base is None or index_reg is None:
        unknown.append((bctr, f'lwzx found but base/index not decoded (base={base}, index={index_reg})'))
        continue
    labels = []
    a = base
    while True:
        v = word(a)
        if not (fb <= v < fe and v % 4 == 0):
            break
        labels.append(v)
        a += 4
    if not labels:
        unknown.append((bctr, f'table at {base:#x} has no in-function entries'))
        continue
    # Key: the lis that starts the dispatch (XenonAnalyse convention).
    key = min(x for x in range(la - 40, la, 4) if word(x) >> 26 == 15 and (word(x) >> 16) & 31 == 0 and word(x) & 0xFFFF == (base >> 16) + (1 if base & 0x8000 else 0) & 0xFFFF) if False else la - 8
    found.append((bctr, base, index_reg, labels, fb))

print(f'# uncovered bctr: {len(found) + len(unknown)}; decoded absolute tables: {len(found)}', file=sys.stderr)
for b, why in unknown:
    print(f'#   {b:#x}: {why}', file=sys.stderr)
for bctr, base, reg, labels, fb in found:
    print(f'# bctr {bctr:#x} in {fb:#x}: absolute table at {base:#x}, index r{reg}, {len(labels)} entries (no bounds check)')
    print(f'[[switch]]\nbase = 0x{bctr - 12:08X}\nr = {reg}\nlabels = [')
    for l in labels:
        print(f'    0x{l:08X},')
    print(']\n')
