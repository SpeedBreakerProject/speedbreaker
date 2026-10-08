"""Apply hand fixes to XenonAnalyse's jump-table output.

    XenonAnalyse game/files/default.xex game/survey/switch_tables.raw.toml
    python3 survey/fix_switch_tables.py   # writes config/switch_tables.toml
"""
import re

RAW, OUT = 'game/survey/switch_tables.raw.toml', 'config/switch_tables.toml'

# XenonAnalyse misread code as tables: their "labels" are instruction words.
DROP = {0x825725B0, 0x82576FD0}

# Tables XenonAnalyse missed.
ADD = [
    # 0x825A45D0: lis/addi r12=0x82063BA8; lhzx r0 = half[r5]; r12 = 0x825A45F8 + r0;
    # bctr. No bounds check (r5 is a trusted argument); 18 entries, the next
    # halfword is ASCII ("un"). Offsets relative to 0x825A45F8.
    dict(base=0x825A45D0, r=5, labels=[
        0x825A45F8, 0x825A4618, 0x825A4644, 0x825A465C, 0x825A46C4, 0x825A46E0,
        0x825A4720, 0x825A47C4, 0x825A4848, 0x825A4898, 0x825A48F8, 0x825A492C,
        0x825A4970, 0x825A49A0, 0x825A49CC, 0x825A49F8, 0x825A4A2C, 0x825A47B4]),
]

text = open(RAW).read()
head, *blocks = text.split('[[switch]]')
kept = []
for b in blocks:
    m = re.search(r'base = 0x([0-9A-F]+)', b)
    if m and int(m.group(1), 16) in DROP:
        continue
    kept.append(b)
assert len(blocks) - len(kept) == len(DROP), 'a DROP entry no longer matches the analyzer output'

out = head + '[[switch]]'.join([''] + kept)
out = out.rstrip() + '\n\n# ---- MANUAL (survey/fix_switch_tables.py) ----\n'
for t in ADD:
    out += f"[[switch]]\nbase = 0x{t['base']:08X}\nr = {t['r']}\nlabels = [\n"
    out += ''.join(f'    0x{l:08X},\n' for l in t['labels']) + ']\n\n'
# Inline absolute tables with no bounds check (survey/find_missing_tables.py,
# run against the analyzer output + the fixes above; regenerate it after
# changing either).
out += '# ---- INLINE (config/switch_tables_inline.toml) ----\n'
out += open('config/switch_tables_inline.toml').read()
open(OUT, 'w').write(out)
print(f'{OUT}: {len(kept) + len(ADD)} tables ({len(DROP)} dropped, {len(ADD)} added)')
