"""Classify XenonRecomp 'switch case outside function' errors against .pdata."""
import bisect, collections, re, struct, sys, tomllib

IMG, BASE = 'game/survey/image.bin', 0x82000000
TEXT = (0x820E0000, 0x828C8BA0)
img = open(IMG, 'rb').read()

def word(a):
    return struct.unpack('>I', img[a - BASE:a - BASE + 4])[0]

funcs = []
for a in range(0x820B1200, 0x820DD6B8, 8):
    b, f = struct.unpack('>II', img[a - BASE:a - BASE + 8])
    if b:
        funcs.append((b, b + ((f >> 8) & 0x3FFFFF) * 4))
funcs.sort()
starts = [f[0] for f in funcs]

def fidx(a):
    i = bisect.bisect_right(starts, a) - 1
    return i if i >= 0 and funcs[i][0] <= a < funcs[i][1] else None

tables = sorted(tomllib.load(open('config/switch_tables.toml', 'rb'))['switch'], key=lambda s: s['base'])
keys = [s['base'] for s in tables]
log = open(sys.argv[1] if len(sys.argv) > 1 else 'game/survey/recomp.log', 'rb').read().decode('latin1')
bad = sorted(set(int(m, 16) for m in re.findall(r'Switch case at ([0-9A-F]+) is trying', log)))

kinds = collections.defaultdict(list)
for bctr in bad:
    s = tables[bisect.bisect_right(keys, bctr) - 1]
    i = fidx(bctr)
    labels = s['labels']
    if any(not (TEXT[0] <= t < TEXT[1]) for t in labels):
        kinds['bogus table (labels are not code addresses)'].append((bctr, s['base']))
        continue
    if i is None:
        kinds['bctr not covered by pdata'].append((bctr, s['base']))
        continue
    outside = [t for t in labels if not (funcs[i][0] <= t < funcs[i][1])]
    if not outside:
        kinds['labels inside pdata fn (recompiler used a smaller boundary)'].append((bctr, funcs[i]))
        continue
    tj = sorted(set(fidx(t) for t in outside), key=lambda x: -1 if x is None else x)
    if None in tj:
        kinds['targets in a pdata gap'].append((bctr, funcs[i], outside[:3]))
    elif all(j > i for j in tj):
        last = max(tj)
        contiguous = all(funcs[k][1] == funcs[k + 1][0] for k in range(i, last))
        kinds['targets in following chunk(s)' + ('' if contiguous else ' (not contiguous)')].append(
            (bctr, i, last))
    else:
        kinds['targets in earlier chunk'].append((bctr, funcs[i], outside[:3]))

for k, v in kinds.items():
    print(f'{len(v):4d}  {k}')
    for e in v[:3]:
        print('      ', [hex(x) if isinstance(x, int) and x > 0x1000000 else x for x in e])
