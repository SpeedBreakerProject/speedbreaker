"""Derive explicit function bounds for jump-table functions with no .pdata entry.

Leaf functions have no .pdata, so XenonRecomp's own analysis guesses their size,
and it stops before the switch targets. For each such table: start = just
after the last unconditional blr/b before the table (never before the gap
start); end = the first unconditional blr/b at or after the farthest target.
Accept only if the end lands on padding, a .pdata start, or a bl target.
"""
import bisect, re, struct, sys, tomllib
exec(open('survey/classify_switches.py').read().split("tables = sorted")[0])

tables = sorted(tomllib.load(open('config/switch_tables.toml', 'rb'))['switch'], key=lambda s: s['base'])
keys = [s['base'] for s in tables]
# Every table's dispatch bctr (first bctr at/after the table key). Independent
# of any recompile log, so the output is stable across runs.
def bctr_of(key):
    a = key
    while word(a) != 0x4E800420 and a - key < 0x80:
        a += 4
    return a
bad = sorted(bctr_of(k) for k in keys)

def is_uncond_exit(w):
    return w == 0x4E800020 or (w >> 26 == 18 and (w & 3) == 0)   # blr, or b (no link, relative)

# bl targets across .text
bl_targets = set()
for a in range(TEXT[0], TEXT[1], 4):
    w = word(a)
    if w >> 26 == 18 and (w & 3) == 1:
        d = w & 0x03FFFFFC
        if d & 0x02000000: d -= 0x04000000
        bl_targets.add(a + d)
pdata_starts = set(starts)

# Code addresses stored in data (vtables, function-pointer tables, callbacks).
DATA = [(0x82000600, 0x820B106C), (0x828D0000, 0x82C8CE8C)]  # .rdata, .data
data_refs = set()
for lo, hi in DATA:
    for a in range(lo, hi, 4):
        v = word(a)
        if TEXT[0] <= v < TEXT[1] and v % 4 == 0:
            data_refs.add(v)
entries = bl_targets | data_refs

out, rejected = {}, []
for bctr in bad:
    if fidx(bctr) is not None:
        continue
    s = tables[bisect.bisect_right(keys, bctr) - 1]
    if word(bctr) != 0x4E800420:
        rejected.append(('REJECTED no bctr near table', hex(s['base'])))
        continue
    if any(not (TEXT[0] <= t < TEXT[1]) for t in s['labels']):
        continue  # bogus table, handled separately
    i = bisect.bisect_right(starts, bctr) - 1
    gap_start, gap_end = funcs[i][1], funcs[i + 1][0]
    # Start = the latest possible entry at or before the table: a bl target, a
    # code address stored in data, or the first word after padding/gap start.
    a = s['base']
    while a > gap_start and a not in entries and word(a - 4) != 0:
        a -= 4
    start = a
    # Grow the extent until every internal branch target and every jump-table
    # label (of any table inside it) is covered, then end at the next exit.
    reach = max(s['labels'] + ([s['default']] if 'default' in s else []))
    while True:
        e = reach
        while e < gap_end and not is_uncond_exit(word(e)) and word(e) != 0:
            e += 4
        end = e if word(e) == 0 else e + 4
        new_reach = reach
        for a in range(start, end, 4):
            w = word(a)
            op = w >> 26
            if op in (16, 18) and (w & 3) == 0:          # bc / b, relative, no link
                d = (w & 0xFFFC) if op == 16 else (w & 0x03FFFFFC)
                sign = 0x8000 if op == 16 else 0x02000000
                if d & sign: d -= sign * 2
                t = a + d
                if start <= t < gap_end: new_reach = max(new_reach, t)
        for k in range(bisect.bisect_left(keys, start), bisect.bisect_left(keys, end)):
            new_reach = max([new_reach] + [t for t in tables[k]['labels'] + ([tables[k]['default']] if 'default' in tables[k] else []) if t < gap_end])
        if new_reach <= reach:
            break
        reach = new_reach
    follow = 'gap end' if end == gap_end else 'padding' if word(end) == 0 else 'pdata fn' if end in pdata_starts \
        else 'bl target' if end in bl_targets else f'unknown next word {word(end):08x}'
    if start < bctr < end <= gap_end:
        out[start] = max(out.get(start, 0), end - start)
        if follow.startswith('unknown'):
            rejected.append(('accepted, check', hex(bctr), hex(start), hex(end), follow))
    else:
        rejected.append(('REJECTED', hex(bctr), hex(start), hex(end), follow))

# Merge overlapping ranges: several tables in one function can each pick a
# different "start" when an internal b precedes a later table.
merged = []
for a in sorted(out):
    e = a + out[a]
    if merged and a < merged[-1][1]:
        if a in bl_targets:
            rejected.append(('merged a bl target, check', hex(a), 'into', hex(merged[-1][0])))
        merged[-1][1] = max(merged[-1][1], e)
    else:
        merged.append([a, e])
out = {a: e - a for a, e in merged}

print(f'# {len(out)} leaf jump-table functions; {len(rejected)} flagged', file=sys.stderr)
for r in rejected:
    print('#   rejected', r, file=sys.stderr)
# Hand-found bounds. Tail-call stubs that end in `b +4` into the next (.pdata)
# function: without an explicit size XenonRecomp treats the b as internal and
# swallows the neighbour, including its jump table.
MANUAL = {
    0x8218C508: 0x10,
    0x822F33D8: 0x28,
}

lines = ['functions = [']
for a in sorted(out):
    lines.append(f'    {{ address = 0x{a:08X}, size = 0x{out[a]:X} }},')
lines.append('    # manual (see MANUAL in survey/leaf_switch_bounds.py)')
for a in sorted(MANUAL):
    lines.append(f'    {{ address = 0x{a:08X}, size = 0x{MANUAL[a]:X} }},')
lines.append(']')
block = '\n'.join(lines)

if '--write' in sys.argv:
    MARKER = '# --- generated by survey/leaf_switch_bounds.py --write ---'
    cfg = 'config/nfsmw.toml'
    text = open(cfg).read().split('# --- generated by survey/leaf_switch_bounds.py')[0].rstrip()
    open(cfg, 'w').write(text + '\n\n' + MARKER + '\n'
        '# Leaf functions (no .pdata) with jump tables: start = latest entry (bl target,\n'
        '# data-referenced address, or after padding); end = branch-closure extent;\n'
        '# overlaps merged. Plus MANUAL entries. Do not edit by hand; rerun.\n' + block + '\n')
    print(f'wrote {cfg}', file=sys.stderr)
else:
    print(block)
