"""survey/sibling_map.py's writer: the new region's config and runtime tables
from the sibling's, every address mapped by SiblingMap and checked against
the new image. Errors are collected and printed together, as region_map.py
does: each is an address to look at by hand."""
import re
import sys

from region_map import BASE, HEX, decode_jump_table, is_exit, bl_targets
from sibling_map import ROOT, REGION_INFO, DATA_SECTIONS, parse_switches, function_starts

PAL_ONLY_TOML = re.compile(r'    # PAL only \(survey/region_map\.py [A-Z_]+\)\n((?:    \{ address = 0x[0-9A-F]+, size = 0x[0-9A-F]+ \},\n)*)')
PAL_ONLY_SW = '\n\n# ---- PAL ONLY ('
# .rdata items whose bytes the runtime's sources say (game/language.h).
KNOWN_RDATA = {0x82078AA8: b'_ntsc\0', 0x82078AB0: b'_pal\0'}
MANUAL_MARKER ='    # manual (see MANUAL in survey/leaf_switch_bounds.py)\n'


def uncommented(text):
    """The lines of a toml, .inc or .h with comments and blank lines taken out."""
    out = []
    for line in text.splitlines():
        s = re.sub(r'\s*(//|#(?!(?:define|include|pragma|if|ifdef|ifndef|else|endif|undef)\b)).*$', '', line)
        if s.strip():
            out.append(s)
    return out


def translate(sm, text, starts, bad):
    out = []
    for n, line in enumerate(text.splitlines(keepends=True), 1):
        code, hash_, comment = line.partition('#')

        def sub(m):
            x = int(m.group(0), 16)
            try:
                return f'0x{sm.code(x, x in starts)[0]:08X}'
            except ValueError as e:
                bad.append(f'line {n}: {e}')
                return m.group(0)
        out.append(HEX.sub(sub, code) + hash_ + comment)
    return ''.join(out)


def own_extra_tables(sm, sw_text):
    """The new image's absolute jump tables that the translated tables don't
    cover (region_map.find_extra_functions without a USA image): each with
    the leaf function holding it when it lies outside .pdata."""
    img = sm.img
    covered = set()
    for m in re.finditer(r'^base = (0x[0-9A-F]+)$', sw_text, re.M):
        a = int(m.group(1), 16)
        for k in range(0, 0x100, 4):
            ws = img.words(a + k, 1)
            if ws and ws[0] == 0x4E800420:
                covered.add(a + k)
                break
    found, targets = [], None
    for _, lo, hi, kind in img.sections:
        if kind != 'code':
            continue
        ws = img.words(lo, (hi - lo) // 4) or ()
        for i, w in enumerate(ws):
            bctr = lo + 4 * i
            if w != 0x4E800420 or bctr in covered:
                continue
            t = decode_jump_table(img, bctr)
            if t is None or not all(lo <= x < hi for x in t['labels'] + [t['default']]):
                continue
            f = sm.ufunc(bctr)
            if f:
                found.append((None, None, t, f'in .pdata function {f[0]:#010x}'))
                continue
            k = max(0, __import__('bisect').bisect_right(sm.fstarts, bctr) - 1)
            floor = sm.funcs[k][0] + sm.funcs[k][1] if sm.funcs[k][0] <= bctr else lo
            a = t['first']
            while a - 4 >= floor and not is_exit(img.words(a - 4, 1)[0]) and img.words(a - 4, 1)[0] != 0:
                a -= 4
            start = a
            a = max(t['labels'] + [t['default']])
            while a < hi and not is_exit(img.words(a, 1)[0]):
                a += 4
            end = a + 4
            if not all(start <= x < end for x in t['labels'] + [t['default']]):
                continue
            if targets is None:
                targets = bl_targets(img)
            found.append((start, end - start, t, 'leaf, ' + ('a bl target' if start in targets else 'NOT a bl target: check by hand')))
    for start, size, t, note in found:
        print(f'  {"switch only" if start is None else f"function {start:#010x} size {size:#x}"}: jump table at '
              f'{t["table"]:#010x}, bctr {t["bctr"]:#010x} (r{t["r"]}, {len(t["labels"])} entries, '
              f'default {t["default"]:#010x}; {note})')
    return [(s, z, t['base'], t['r'], t['default'], t['labels']) for s, z, t, _ in found]


def check_leaf_sizes(sm, toml, bad):
    # A leaf function the config bounds must end where its translation does.
    for m in re.finditer(r'\{ address = (0x[0-9A-F]+), size = (0x[0-9A-F]+) \}', toml):
        a, z = int(m.group(1), 16), int(m.group(2), 16)
        try:
            if sm.code(a + z - 4)[0] - sm.code(a)[0] != z - 4:
                bad.append(f'leaf function {a:#010x} (size {z:#x}): its end moved')
        except ValueError as e:
            bad.append(f'leaf function {a:#010x} (size {z:#x}): {e}')


def check_helpers(sm, toml, bad):
    # The compiler helpers, found by their byte patterns in the new image
    # (survey/xexsurvey.cpp's list).
    pats = {'restgprlr_14': b'\xE9\xC1\xFF\x68', 'savegprlr_14': b'\xF9\xC1\xFF\x68', 'restfpr_14': b'\xC9\xCC\xFF\x70',
            'savefpr_14': b'\xD9\xCC\xFF\x70', 'restvmx_14': b'\x39\x60\xFE\xE0\x7D\xCB\x60\xCE',
            'savevmx_14': b'\x39\x60\xFE\xE0\x7D\xCB\x61\xCE', 'restvmx_64': b'\x39\x60\xFC\x00\x10\x0B\x60\xCB',
            'savevmx_64': b'\x39\x60\xFC\x00\x10\x0B\x61\xCB'}
    for key, pat in pats.items():
        m = re.search(rf'^{key}_address = (0x[0-9A-F]+)', toml, re.M)
        if m and sm.img.bytes(int(m.group(1), 16), len(pat)) != pat:
            bad.append(f'{key}_address {m.group(1)}: not the helper\'s bytes in the new image')


def code_refs(sm, addr):
    """How many lis+addi/load/store pairs in the new image's code build addr."""
    hi, lo = (addr + 0x8000) >> 16 & 0xFFFF, addr & 0xFFFF
    n = 0
    for _, s, e, kind in sm.img.sections:
        if kind != 'code':
            continue
        ws = sm.img.words(s, (e - s) // 4) or ()
        for i, w in enumerate(ws):
            if w >> 26 == 15 and (w >> 16) & 31 == 0 and w & 0xFFFF == hi:
                rt = (w >> 21) & 31
                for x in ws[i + 1:i + 9]:
                    op, ra = x >> 26, (x >> 16) & 31
                    if ra == rt and (op == 14 or 32 <= op <= 55) and x & 0xFFFF == lo:
                        n += 1
                        break
    return n


def map_runtime(sm, sibling, region, bad):
    """[(usa, new, how, files)] from the sibling's address table."""
    sib_stem, macro = sibling.replace('-', '_'), 'SB_' + sibling.replace('-', '_').upper()
    names = set(int(m, 16) for m in re.findall(rf'#define {macro}_sub_(82[0-9A-F]{{6}}) ',
                                                  (ROOT / f'runtime/region/{sib_stem}_names.h').read_text()))
    for name, (lo, hi) in DATA_SECTIONS.items():
        s = next((s for s in sm.img.sections if s[0] == name), None)
        if not s or (s[1], s[2]) != (lo, hi):
            bad.append(f'{name}: moved in the new image ({s})')
    rows = []
    pat = re.compile(rf'^{macro}_ADDRESS\((0x[0-9A-F]+), (0x[0-9A-F]+)\)  // (.*?); (.*)$')
    for line in (ROOT / f'runtime/region/{sib_stem}_addresses.inc').read_text().splitlines():
        m = pat.match(line)
        if not m:
            continue
        usa, sib, how, files = int(m.group(1), 16), int(m.group(2), 16), m.group(3), m.group(4)
        try:
            if how.startswith(('.data', 'outside the image', 'the image header')):
                new, why = sib, how.split(',')[0] + ' (the same)'
                if how.startswith('the image header') and sm.img.bytes(sib, 4) != b'XEX2' and sib == BASE:
                    pass
            elif usa in KNOWN_RDATA:
                want = KNOWN_RDATA[usa]
                if sm.img.bytes(sib, len(want)) != want:
                    raise ValueError(f'{usa:#010x} (sibling {sib:#010x}): .rdata, not {want!r} in the new image')
                new, why = sib, f'.rdata, the sibling\'s address, holds {want[:-1].decode()!r}'
            elif how.startswith('.rdata'):
                # Where the sibling has it, if the new image's code builds the
                # address and its bytes are the sibling's kind (strings stay
                # strings); else unmapped.
                n = code_refs(sm, sib)
                if not n:
                    raise ValueError(f'{usa:#010x} (sibling {sib:#010x}): .rdata, not referenced by the new image\'s code there')
                new, why = sib, f'.rdata, the sibling\'s address, built by {n} lis pair(s) in the new image\'s code'
            else:
                new, why = sm.code(sib, usa in names)
        except ValueError as e:
            bad.append(f'{e}  ({files})')
            continue
        rows.append((usa, new, why, files, usa in names))
    return rows


def write(sm, region, check):
    sibling = sm.sibling
    sib_name, _, sib_hash = REGION_INFO[sibling]
    name, title, new_hash = REGION_INFO[region]
    assert new_hash, f'{region}: no default.xex SHA-256 in runtime/region/hook_addresses.py'
    bad = []
    starts = function_starts(sm.sib_toml)
    starts |= set(int(m, 16) for m in re.findall(r'sub_(82[0-9A-F]{6})', (ROOT / f'runtime/region/{sibling.replace("-", "_")}_names.h').read_text()))

    # --- config/<region>/nfsmw.toml ------------------------------------
    toml = sm.sib_toml
    # The sibling's own extra functions go; the new image's are found below.
    m = PAL_ONLY_TOML.search(toml)
    sib_extras = [int(x, 16) for x in re.findall(r'address = (0x[0-9A-F]+)', m.group(1))] if m else []
    if m:
        toml = toml[:m.start()] + toml[m.end():]
    body = toml.split('\n', 4)[4] if toml.startswith('# GENERATED') else toml
    body = body.replace(sib_hash, new_hash).replace(f'(Xbox 360, {REGION_INFO[sibling][1]})', f'(Xbox 360, {title})')
    body = body.replace(f'"../../game/files-{sibling}/default.xex"', f'"../../game/files-{region}/default.xex"')
    body = body.replace(f'"../../ppc-{sibling}"', f'"../../ppc-{region}"')
    # The same four lines' shape as survey/region_map.py's header
    # (tests/region_addresses_check.py's config_shape drops them).
    header = ('# GENERATED by survey/sibling_map.py from config/' + sibling + '/nfsmw.toml: do not edit; rerun it.\n'
              f'# The {name} disc\'s default.xex: every address is the {sib_name} one moved to\n'
              '# where the PAL build has it (checked against its image); comments still name the USA\n'
              '# addresses.\n')
    toml_bad = []
    new_toml = header + translate(sm, body, starts, toml_bad)
    bad += [f'config: {b}' for b in toml_bad]
    check_helpers(sm, new_toml, bad)
    check_leaf_sizes(sm, body, bad)

    # --- config/<region>/switch_tables.toml ------------------------------
    sw = sm.sib_sw
    cut = sw.find(PAL_ONLY_SW)
    if cut >= 0:
        sw = sw[:cut] + '\n'
    sw = sw.split('\n', 1)[1] if sw.startswith('# GENERATED') else sw
    sw_bad = []
    new_sw = f'# GENERATED by survey/sibling_map.py from config/{sibling}/switch_tables.toml ({name} addresses).\n' \
        + translate(sm, sw, starts, sw_bad)
    bad += [f'switch_tables: {b}' for b in sw_bad]

    print('jump tables in the new image the translated ones do not cover:')
    extras = own_extra_tables(sm, new_sw)
    leafs = [(a, z) for a, z, *_ in extras if a is not None]
    print(f'  {len(extras)} table(s), {len(leafs)} leaf function(s); the sibling\'s own were at '
          + ', '.join(f'{a:#010x}' for a in sib_extras))
    assert new_toml.count(MANUAL_MARKER) == 1
    new_toml = new_toml.replace(MANUAL_MARKER, '    # PAL only (survey/sibling_map.py own_extra_tables)\n' + ''.join(
        f'    {{ address = 0x{a:08X}, size = 0x{z:X} }},\n' for a, z in leafs) + MANUAL_MARKER)
    if extras:
        new_sw = new_sw.rstrip('\n') + '\n\n# ---- PAL ONLY (survey/sibling_map.py own_extra_tables) ----\n'
        for a, z, base, r, default, labels in extras:
            new_sw += f'[[switch]]\nbase = 0x{base:08X}\nr = {r}\ndefault = 0x{default:08X}\nlabels = [\n'
            new_sw += ''.join(f'    0x{l:08X},\n' for l in labels) + ']\n\n'

    # --- runtime/region/<stem>_{addresses.inc,names.h} -------------------
    rows = map_runtime(sm, sibling, region, bad)
    if bad:
        sys.exit(f'cannot map {len(bad)} address(es):\n  ' + '\n  '.join(bad[:300]) + ('\n  ...' if len(bad) > 300 else ''))

    stem = region.replace('-', '_')
    macro = 'SB_' + stem.upper()
    import hook_addresses
    lines = ['// GENERATED by survey/sibling_map.py write: do not edit; rerun it.',
             '// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).',
             '//',
             f'// Every guest address the runtime names (runtime/region/hook_addresses.py finds',
             f'// them) and where the {name} default.xex has it: the {sib_name} table\'s',
             f'// address moved to the new layout. {macro}_ADDRESS(USA, PAL). USA default.xex',
             f'// {hook_addresses.USA_XEX_SHA256}, PAL {new_hash}.',
             '']
    for usa, new, how, files, _ in rows:
        lines.append(f'{macro}_ADDRESS(0x{usa:08X}, 0x{new:08X})  // {how}; {files}')
    hdr = ['// GENERATED by survey/sibling_map.py write: do not edit; rerun it.',
           '// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).',
           '//',
           f'// {macro}_sub_<USA> -> sub_<PAL>: the {name} recompiled function of each one',
           '// the runtime names (region.h\'s SB_HOOK and SB_ORIGINAL use it). A function',
           f'// missing here is a build error: rerun survey/sibling_map.py write --region {region}.',
           '#pragma once',
           '']
    hdr += [f'#define {macro}_sub_{usa:08X} sub_{new:08X}' for usa, new, _, _, is_name in rows if is_name]

    outs = {ROOT / f'config/{region}/nfsmw.toml': new_toml,
            ROOT / f'config/{region}/switch_tables.toml': new_sw,
            ROOT / f'runtime/region/{stem}_addresses.inc': '\n'.join(lines) + '\n',
            ROOT / f'runtime/region/{stem}_names.h': '\n'.join(hdr) + '\n'}
    ok = True
    for path, text in outs.items():
        if check:
            # Content only: with a USA image the committed files are
            # survey/region_map.py's, whose comments say other things.
            if not path.exists() or uncommented(path.read_text()) != uncommented(text):
                print(f'{path.relative_to(ROOT)}: out of date (rerun survey/sibling_map.py write)')
                ok = False
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
            print(f'wrote {path.relative_to(ROOT)}')
    return ok
