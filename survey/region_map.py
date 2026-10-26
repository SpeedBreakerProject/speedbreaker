#!/usr/bin/env python3
"""USA -> another region's guest addresses, for Most Wanted's PAL (Spanish) disc.

The PAL default.xex is the USA build's source tree built for Europe: 22,679
functions, aligned 1:1 with the USA's (all but 19 the same code modulo the
addresses in it), laid out in 17 pieces shifted by 0..+0x200; .data and .bss
keep their addresses, .rdata items shift by 0..+0xD0. This maps any USA
address the recompiler config or the runtime names to the PAL one, and checks
the PAL bytes there are the USA's (instructions compared with branch
displacements and address halves masked).

    python3 survey/region_map.py toml   <usa dir> <pal dir> <funcmap.tsv>
        writes config/pal-es/{nfsmw.toml,switch_tables.toml} from config/
    python3 survey/region_map.py table  <usa dir> <pal dir> <funcmap.tsv>
        writes runtime/region/pal_es_addresses.inc: every guest address the
        runtime's sources name (runtime/region/hook_addresses.py finds them)
    python3 survey/region_map.py check  <usa dir> <pal dir> <funcmap.tsv>
        both, compared with the committed files (exit 1 on a difference)

<usa dir>/<pal dir>: a decrypted, decompressed image of each default.xex
(image.bin, base 0x82000000; survey/xexsurvey.cpp writes it, with the section
table in xexsurvey.txt). funcmap.tsv: usa_addr pal_addr usa_len pal_len status
for every USA .pdata function (status exact, reloc-equal or changed). All are
derived from the player's own discs and never committed: only the addresses
this writes are. Run with python3 -I.
"""
import bisect
import collections
import pathlib
import re
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
BASE = 0x82000000
sys.path.insert(0, str(ROOT / 'runtime/region'))
import hook_addresses  # noqa: E402


class Image:
    def __init__(self, d):
        d = pathlib.Path(d)
        self.data = (d / 'image.bin').read_bytes()
        self.sections = []
        for line in open(d / 'xexsurvey.txt', errors='replace'):
            m = re.match(r'\| (.+?) \| 0x([0-9A-F]+) \| 0x([0-9A-F]+) \| 0x([0-9A-F]+) \| (\w+) \|', line)
            if m:
                self.sections.append((m.group(1), int(m.group(2), 16), int(m.group(3), 16), m.group(5)))

    def section(self, addr):
        for s in self.sections:
            if s[1] <= addr < s[2]:
                return s
        return None

    def named(self, name):
        return next(s for s in self.sections if s[0] == name)

    def words(self, addr, n):
        o = addr - BASE
        if o < 0 or o + 4 * n > len(self.data):
            return None
        return struct.unpack_from(f'>{n}I', self.data, o)

    def bytes(self, addr, n):
        o = addr - BASE
        return self.data[o:o + n] if 0 <= o and o + n <= len(self.data) else None


def norm(ws):
    """Instructions with what moves with the layout masked: branch
    displacements, lis/addis high halves, and the low halves of addi/ori/loads/
    stores not off the stack pointer."""
    out = []
    for w in ws:
        op, ra = w >> 26, (w >> 16) & 31
        if op in (16, 18):                 # bc, b/bl: relative targets
            w &= 0xFC000003 if op == 18 else 0xFFFF0003
        elif op == 15:
            w &= 0xFFFF0000
        elif op in (14, 24, 25) or 32 <= op <= 55 or op in (58, 62):
            if ra != 1:
                w &= 0xFFFF0000
        out.append(w)
    return tuple(out)


class RegionMap:
    def __init__(self, usa_dir, pal_dir, funcmap):
        self.usa, self.pal = Image(usa_dir), Image(pal_dir)
        self.funcs = []   # (usa, pal, usa_len, pal_len, status), sorted by usa
        for line in open(funcmap):
            f = line.rstrip('\n').split('\t')
            if not f[0].startswith('0x'):
                continue
            self.funcs.append((int(f[0], 16), int(f[1], 16) if f[1] != '-' else None, int(f[2]),
                               int(f[3]) if f[3] != '-' else None, f[4]))
        self.funcs.sort()
        self.starts = [f[0] for f in self.funcs]
        self._data = None

    # --- code --------------------------------------------------------------
    def function_of(self, addr):
        i = bisect.bisect_right(self.starts, addr) - 1
        if i >= 0 and self.funcs[i][0] <= addr < self.funcs[i][0] + self.funcs[i][2]:
            return i
        return None

    def same_code(self, usa, pal, n=4):
        """'exact', 'reloc-equal' or None: up to n instructions from each,
        stopping after an unconditional exit (a jump table may follow a
        bctr: data, not code)."""
        a, b = self.usa.words(usa, n), self.pal.words(pal, n)
        if a is None or b is None:
            return None
        for i, w in enumerate(a):
            if w in (0x4E800420, 0x4E800020) or (w >> 26 == 18 and w & 1 == 0):
                a, b = a[:i + 1], b[:i + 1]
                break
        if a == b:
            return 'exact'
        return 'reloc-equal' if norm(a) == norm(b) else None

    def code(self, addr, window=4):
        """(pal, how): `how` names the evidence. Raises ValueError."""
        i = self.function_of(addr)
        if i is not None:
            u, p, ul, pl, st = self.funcs[i]
            if p is None:
                raise ValueError(f'{addr:#010x}: in USA-only function {u:#010x}')
            if st == 'changed':
                # One of the 19 the PAL build changed: the instruction at the
                # same place in the aligned code, if it is in an unchanged run.
                pal = self.aligned(i).get(addr)
                if pal is None:
                    raise ValueError(f'{addr:#010x}: in {u:#010x} (changed in the PAL build), in code the PAL one changed')
                eq = self.same_code(addr, pal, 1)
                if not eq:
                    raise ValueError(f'{addr:#010x} -> {pal:#010x}: the code differs')
                return pal, f'in sub_{u:08X} (changed; aligned), {eq}'
            if ul != pl:
                raise ValueError(f'{addr:#010x}: in {u:#010x}, whose length differs')
            pal = p + (addr - u)
            n = max(1, min(window, (u + ul - addr) // 4))
            eq = self.same_code(addr, pal, n)
            if not eq:
                raise ValueError(f'{addr:#010x} -> {pal:#010x}: the code differs')
            return pal, f'in sub_{u:08X} ({st}), {eq}'
        # Code outside .pdata (leaf functions the config lists): the delta of
        # a neighbouring function, if the code there is the same.
        k = bisect.bisect_right(self.starts, addr)
        deltas = []
        for j in (k - 1, k, k - 2, k + 1):
            if 0 <= j < len(self.funcs) and self.funcs[j][1] is not None:
                d = self.funcs[j][1] - self.funcs[j][0]
                if d not in deltas:
                    deltas.append(d)
        found = [(d, self.same_code(addr, addr + d, 8)) for d in deltas]
        found = [(d, eq) for d, eq in found if eq]
        if not found:
            raise ValueError(f'{addr:#010x}: outside .pdata, and no neighbouring delta matches its code')
        d, eq = found[0]
        return addr + d, f'outside .pdata, neighbour delta {d:+#x}, {eq}'

    def aligned(self, i):
        """{usa address: pal address} for the instructions of a changed
        function that lie in runs equal (relocations masked) in both."""
        import difflib
        if not hasattr(self, '_aligned'):
            self._aligned = {}
        if i not in self._aligned:
            u, p, ul, pl, _ = self.funcs[i]
            a, b = norm(self.usa.words(u, ul // 4)), norm(self.pal.words(p, pl // 4))
            out = {}
            for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(None, a, b, autojunk=False).get_opcodes():
                if tag == 'equal':
                    for k in range(i2 - i1):
                        out[u + 4 * (i1 + k)] = p + 4 * (j1 + k)
            self._aligned[i] = out
        return self._aligned[i]

    # --- data --------------------------------------------------------------
    def _data_refs(self):
        # Absolute data addresses built by lis + addi/load/store in the
        # matched functions, paired USA -> PAL.
        if self._data is not None:
            return self._data
        dmap = collections.defaultdict(collections.Counter)
        sx = lambda v: (v & 0xFFFF) - 0x10000 if v & 0x8000 else v & 0xFFFF
        for u, p, ul, pl, st in self.funcs:
            if st not in ('exact', 'reloc-equal') or ul != pl:
                continue
            uw, pw = self.usa.words(u, ul // 4), self.pal.words(p, pl // 4)
            hu, hp = {}, {}
            for x, y in zip(uw, pw):
                op, rd, ra = x >> 26, (x >> 21) & 31, (x >> 16) & 31
                if op == 15 and ra == 0:
                    hu[rd], hp[rd] = (x & 0xFFFF) << 16, (y & 0xFFFF) << 16
                    continue
                if (op == 14 or 32 <= op <= 55) and ra in hu:
                    au, ap = (hu[ra] + sx(x)) & 0xFFFFFFFF, (hp[ra] + sx(y)) & 0xFFFFFFFF
                    if BASE <= au < BASE + 0x1000000:
                        dmap[au][ap - au] += 1
                if op != 15 and rd in hu and not (op in (36, 37, 38, 39, 44, 45, 52, 53, 54, 55)):
                    hu.pop(rd, None)
                    hp.pop(rd, None)
        self._data = {k: c.most_common(1)[0][0] for k, c in dmap.items()}
        self._data_keys = sorted(self._data)
        return self._data

    def data(self, addr, size=4):
        if addr >= BASE + len(self.usa.data) or addr < BASE:
            return addr, 'outside the image (heap or another window): the same'
        s = self.usa.section(addr)
        if s is None:
            if addr < min(x[1] for x in self.usa.sections):
                if self.usa.bytes(addr, size) != self.pal.bytes(addr, size):
                    raise ValueError(f'{addr:#010x}: in the image header, which differs')
                return addr, 'the image header (the same)'
            raise ValueError(f'{addr:#010x}: in no section')
        name = s[0]
        if name in ('.data', '.tls', '.XEXID', '.idata', '.XBLD'):
            ps = self.pal.named(name)
            if (ps[1], ps[2]) != (s[1], s[2]):
                raise ValueError(f'{addr:#010x}: {name} moved')
            ub, pb = self.usa.bytes(addr, size), self.pal.bytes(addr, size)
            return addr, f'{name} (same layout)' + (', same bytes' if ub == pb else ', initial bytes differ')
        if name == '.rdata':
            refs = self._data_refs()
            want = self.usa.bytes(addr, size)
            cands = []
            if addr in refs:
                cands.append((refs[addr], 'referenced'))
            k = bisect.bisect_right(self._data_keys, addr)
            for j in (k - 1, k):
                if 0 <= j < len(self._data_keys):
                    d = refs[self._data_keys[j]]
                    cands.append((d, f'beside {self._data_keys[j]:#010x}'))
            for d, how in cands:
                if self.pal.bytes(addr + d, size) == want:
                    return addr + d, f'.rdata {how}, delta {d:+#x}, same bytes'
            raise ValueError(f'{addr:#010x}: .rdata, no paired delta finds the same {size} bytes')
        if s[3] == 'code':
            return self.code(addr)
        raise ValueError(f'{addr:#010x}: in {name}')

    def any(self, addr, kind=None, size=4):
        s = self.usa.section(addr)
        if kind == 'code' or (kind is None and s and s[3] == 'code'):
            return self.code(addr)
        return self.data(addr, size)


# --- the recompiler config ---------------------------------------------------
HEX = re.compile(r'0x82[0-9A-Fa-f]{6}\b')


def translate_toml(rm, text, header, replacements):
    out = []
    for line in text.splitlines(keepends=True):
        code, hash_, comment = line.partition('#')
        for old, new in replacements:
            code = code.replace(old, new)

        def sub(m):
            usa = int(m.group(0), 16)
            pal, _ = rm.code(usa)
            return f'0x{pal:08X}'
        out.append(HEX.sub(sub, code) + hash_ + comment)
    return header + ''.join(out)


# Code the PAL build has and the USA one doesn't: two leaf functions (no
# .pdata entry) between the USA's 0x823AEEC0 and 0x823AF130, called by the
# changed functions (the date and number formats): each loads the language
# (0x828FF0C8), and for 0..13 returns a string from a 14-entry jump table
# (index r11, past 13: 0). Their tables are checked against the image below.
PAL_ES_EXTRA_FUNCTIONS = [
    # (address, size, table base (the lis before the bctr), index register, default)
    (0x823AF018, 0xE0, 0x823AF028, 11, 0x823AF0F0),
    (0x823AF0F8, 0xE0, 0x823AF108, 11, 0x823AF1D0),
]


def extra_tables(rm):
    out = []
    for addr, size, base, r, default in PAL_ES_EXTRA_FUNCTIONS:
        ws = rm.pal.words(addr, size // 4)
        bctr = next(addr + 4 * i for i, w in enumerate(ws) if w == 0x4E800420)
        cmp = ws[2]
        assert cmp >> 16 == 0x2B0B, f'{addr:#x}: no cmplwi r11'
        n = (cmp & 0xFFFF) + 1
        labels = list(rm.pal.words(bctr + 4, n))
        assert all(addr <= l < addr + size for l in labels + [default]), f'{addr:#x}: a label outside the function'
        assert bctr + 4 + 4 * n == min(labels), f'{addr:#x}: the table does not end where the cases begin'
        out.append((addr, size, base, r, default, labels))
    return out


def check_inline_tables(rm):
    # The inline tables (survey/find_missing_tables.py: the absolute
    # addresses right after the bctr): the PAL table's words must be the
    # translated labels. (The analyzer's tables were compared with
    # XenonAnalyse's own output for the PAL default.xex: the same.)
    text = (ROOT / 'config/switch_tables.toml').read_text().split('# ---- INLINE')[1]
    n_ok = 0
    for part in re.split(r'\n(?=# bctr)', text):
        m = re.search(r'# bctr (0x[0-9a-f]+) in 0x[0-9a-f]+: absolute table at (0x[0-9a-f]+), index r\d+, (\d+) entries', part)
        if not m:
            continue
        bctr, table, n = int(m.group(1), 16), int(m.group(2), 16), int(m.group(3))
        labels = [int(x, 16) for x in HEX.findall(part.split('labels')[1])]
        pal_bctr = rm.code(bctr)[0]
        words = list(rm.pal.words(pal_bctr + (table - bctr), n))
        if words != [rm.code(x)[0] for x in labels]:
            sys.exit(f'inline table after {bctr:#010x}: the PAL table differs')
        n_ok += 1
    print(f'{n_ok} inline jump tables: the PAL tables hold the translated labels')


def check_function_sizes(rm, toml):
    # A leaf function the config bounds must end where its translation does.
    for m in re.finditer(r'\{ address = (0x[0-9A-F]+), size = (0x[0-9A-F]+) \}', toml):
        a, z = int(m.group(1), 16), int(m.group(2), 16)
        if rm.code(a + z - 4)[0] - rm.code(a)[0] != z - 4:
            sys.exit(f'function {a:#010x} (size {z:#x}): its end moved')


def write_toml(rm, check):
    usa_hash, pal_hash = hook_addresses.USA_XEX_SHA256, hook_addresses.PAL_ES_XEX_SHA256
    main = (ROOT / 'config/nfsmw.toml').read_text()
    assert usa_hash in main
    header = ('# GENERATED by survey/region_map.py from config/nfsmw.toml: do not edit; rerun it.\n'
              '# The PAL (Spanish) disc\'s default.xex: every address is the USA one moved to\n'
              '# where the PAL build has the same code (checked); comments still name the USA\n'
              '# addresses.\n')
    toml = translate_toml(rm, main.replace(usa_hash, pal_hash).replace('(Xbox 360, USA)', '(Xbox 360, Europe/PAL, Spanish)'),
                          header, [('"../game/files/default.xex"', '"../../game/files-pal-es/default.xex"'),
                                   ('"../ppc"', '"../../ppc-pal-es"'),
                                   ('"switch_tables.toml"', '"switch_tables.toml"')])
    check_inline_tables(rm)
    check_function_sizes(rm, main)
    extras = extra_tables(rm)
    # The extra functions join the leaf functions' list.
    marker = '    # manual (see MANUAL in survey/leaf_switch_bounds.py)\n'
    assert toml.count(marker) == 1
    toml = toml.replace(marker, '    # PAL only (survey/region_map.py PAL_ES_EXTRA_FUNCTIONS)\n' + ''.join(
        f'    {{ address = 0x{a:08X}, size = 0x{z:X} }},\n' for a, z, *_ in extras) + marker)
    sw = translate_toml(rm, (ROOT / 'config/switch_tables.toml').read_text(),
                        '# GENERATED by survey/region_map.py from config/switch_tables.toml (PAL addresses).\n', [])
    sw = sw.rstrip('\n') + '\n\n# ---- PAL ONLY (survey/region_map.py PAL_ES_EXTRA_FUNCTIONS) ----\n'
    for a, z, base, r, default, labels in extras:
        sw += f'[[switch]]\nbase = 0x{base:08X}\nr = {r}\ndefault = 0x{default:08X}\nlabels = [\n'
        sw += ''.join(f'    0x{l:08X},\n' for l in labels) + ']\n\n'

    outs = {ROOT / 'config/pal-es/nfsmw.toml': toml, ROOT / 'config/pal-es/switch_tables.toml': sw}
    return emit(outs, check)


# --- the runtime's table -----------------------------------------------------
def write_table(rm, check):
    found = hook_addresses.scan(ROOT)
    rows, bad = [], []
    for addr in sorted(found):
        refs = found[addr]
        kind = hook_addresses.kind_of(addr, refs)
        try:
            pal, how = rm.any(addr, kind)
        except ValueError as e:
            bad.append(f'{e}  ({", ".join(sorted(refs))})')
            continue
        rows.append((addr, pal, how, refs))
    if bad:
        sys.exit('cannot map:\n  ' + '\n  '.join(bad))
    lines = ['// GENERATED by survey/region_map.py table: do not edit; rerun it.',
             '// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).',
             '//',
             '// Every guest address the runtime names (runtime/region/hook_addresses.py finds',
             '// them) and where the PAL (Spanish) default.xex has the same code or data:',
             f'// SB_PAL_ES_ADDRESS(USA, PAL). USA default.xex {hook_addresses.USA_XEX_SHA256},',
             f'// PAL {hook_addresses.PAL_ES_XEX_SHA256}.',
             '']
    for addr, pal, how, refs in rows:
        files = sorted({r.split(':')[0] for r in refs})
        lines.append(f'SB_PAL_ES_ADDRESS(0x{addr:08X}, 0x{pal:08X})  // {how}; {", ".join(files)}')
    names = ['// GENERATED by survey/region_map.py table: do not edit; rerun it.',
             '// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).',
             '//',
             '// SB_PAL_ES_sub_<USA> -> sub_<PAL>: the PAL (Spanish) recompiled function of each one',
             '// the runtime names (region.h\'s SB_HOOK and SB_ORIGINAL use it). A function',
             '// missing here is a build error: rerun survey/region_map.py table.',
             '#pragma once',
             '']
    for addr, pal, how, refs in rows:
        if hook_addresses.kind_of(addr, refs) == 'code':
            names.append(f'#define SB_PAL_ES_sub_{addr:08X} sub_{pal:08X}')
    return emit({ROOT / 'runtime/region/pal_es_addresses.inc': '\n'.join(lines) + '\n',
                 ROOT / 'runtime/region/pal_es_names.h': '\n'.join(names) + '\n'}, check)


def emit(outs, check):
    ok = True
    for path, text in outs.items():
        if check:
            if not path.exists() or path.read_text() != text:
                print(f'{path.relative_to(ROOT)}: out of date (rerun survey/region_map.py)')
                ok = False
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
            print(f'wrote {path.relative_to(ROOT)}')
    return ok


def main():
    if len(sys.argv) != 5 or sys.argv[1] not in ('toml', 'table', 'check'):
        sys.exit(__doc__)
    rm = RegionMap(*sys.argv[2:5])
    ok = True
    if sys.argv[1] in ('toml', 'check'):
        ok &= write_toml(rm, sys.argv[1] == 'check')
    if sys.argv[1] in ('table', 'check'):
        ok &= write_table(rm, sys.argv[1] == 'check')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
