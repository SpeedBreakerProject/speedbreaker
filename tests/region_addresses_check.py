#!/usr/bin/env python3
"""A PAL release's addresses for the runtime's hooks (runtime/region/region.h):
the Spanish one's (pal-es, the default) or the German one's (pal-de):

    python3 tests/region_addresses_check.py <repo> [<usa dir> <pal dir> <funcmap.tsv>] [--region pal-de]

Without the images (the default; no game data needed):
  - every guest address the runtime's sources name (runtime/region/
    hook_addresses.py) has a PAL address in runtime/region/pal_es_addresses.inc
    (pal_de_addresses.inc), and every function they name a PAL name in
    pal_es_names.h (pal_de_names.h);
  - every hook is an SB_HOOK (a plain PPC_FUNC(sub_...) override would leave
    the PAL code unhooked) and every SB_ORIGINAL names a hooked function;
  - every code or .rdata constant the runtime's .cpp files use is used through
    region::Addr (its PAL address can differ; .data, .bss and addresses outside
    the image are the same in both, which the table records);
  - no address is in one of the 19 functions the PAL build changed;
  - config/<region>/ is config/ translated: the same settings and hooks, only
    their addresses moved (a hook added to config/nfsmw.toml since the region's
    was written fails here).
With the two decrypted images (survey/region_map.py's inputs: <dir>/image.bin
and xexsurvey.txt) and the function map, also: the bytes at each USA address
and its PAL one match (instructions with branch displacements and address
halves masked; data exactly), and the committed table, names and config/pal-es/
(config/pal-de/) are what survey/region_map.py writes from them.
"""
import pathlib
import re
import subprocess
import sys


HEX82 = re.compile(r'0x82[0-9A-Fa-f]{6}\b')


def config_shape(text, regional):
    """The recompiler config with what a region changes taken out: the guest
    addresses in its settings (not in comments, which keep the USA ones), the
    lines naming the disc and its folders, the GENERATED header and the
    PAL-only block survey/region_map.py adds. Two configs of one shape differ
    in addresses only."""
    out, skip = [], False
    for line in text.splitlines():
        if regional and line.startswith('# GENERATED') or line.startswith('# The PAL') or \
                line.startswith('# where the PAL build') or line.startswith('# addresses.'):
            continue
        s = line.strip()
        if s.startswith('# PAL only (') or s.startswith('# ---- PAL ONLY ('):
            skip = True
            continue
        if skip:
            if s.startswith('# manual') or (s.startswith('#') and 'PAL' not in s and not s.startswith('{')):
                skip = False
            elif s.startswith('{') or s.startswith('[[switch]]') or s.startswith('base') or s.startswith('r =') \
                    or s.startswith('default') or s.startswith('labels') or s.startswith('0x') or s in (']', ''):
                continue
            else:
                skip = False
        if s.startswith('# XenonRecomp config for') or s.startswith('# default.xex SHA-256') \
                or s.startswith('file_path') or s.startswith('out_directory_path'):
            continue
        code, hash_, comment = line.partition('#')
        out.append(HEX82.sub('0x82______', code) + hash_ + comment)
    while out and out[-1] == '':
        out.pop()
    return out


def main():
    args = sys.argv[1:]
    region = 'pal-es'
    if '--region' in args:
        i = args.index('--region')
        if i + 1 >= len(args) or args[i + 1] not in ('pal-es', 'pal-de'):
            sys.exit(__doc__)
        region = args[i + 1]
        del args[i:i + 2]
    if len(args) not in (1, 4):
        sys.exit(__doc__)
    root = pathlib.Path(args[0]).resolve()
    sys.path.insert(0, str(root / 'runtime/region'))
    import hook_addresses as ha
    stem = region.replace('-', '_')
    macro = 'SB_' + stem.upper()
    table_file, names_file = root / f'runtime/region/{stem}_addresses.inc', root / f'runtime/region/{stem}_names.h'

    fails = []
    table = {}
    hows = {}
    for line in table_file.read_text().splitlines():
        m = re.match(macro + r'_ADDRESS\((0x[0-9A-F]{8}), (0x[0-9A-F]{8})\)\s*// ([^;]*);', line)
        if m:
            usa, pal = int(m.group(1), 16), int(m.group(2), 16)
            if usa in table:
                fails.append(f'{usa:#010x} twice in the table')
            table[usa], hows[usa] = pal, m.group(3)
    names = {}
    for line in names_file.read_text().splitlines():
        m = re.match('#define ' + macro + r'_sub_([0-9A-F]{8}) sub_([0-9A-F]{8})$', line)
        if m:
            names[int(m.group(1), 16)] = int(m.group(2), 16)

    found = ha.scan(root)
    for addr, refs in sorted(found.items()):
        where = ', '.join(sorted(refs)[:3])
        if addr not in table:
            fails.append(f'{addr:#010x} ({where}): no {region} address; rerun survey/region_map.py table --region {region}')
            continue
        if ha.kind_of(addr, refs) == 'code' and names.get(addr) != table[addr]:
            fails.append(f'sub_{addr:08X} ({where}): not in {names_file.name} (or another address)')
        if 'changed' in hows[addr]:
            fails.append(f'{addr:#010x} ({where}): in a function the PAL build changed ({hows[addr]}): review it')
    for addr in table:
        if addr not in found:
            fails.append(f'{addr:#010x}: in the table but no longer in the sources (rerun survey/region_map.py table --region {region})')

    # The region's recompiler config is the USA one translated (survey/
    # region_map.py toml): the same settings and hooks, only their addresses
    # moved. One the USA config gained since (a new midasm hook) and the
    # region's didn't means it wasn't written again: no image needed to see it.
    for name in ('nfsmw.toml', 'switch_tables.toml'):
        usa_cfg, reg_cfg = root / 'config' / name, root / 'config' / region / name
        if not reg_cfg.exists():
            fails.append(f'config/{region}/{name}: missing (survey/region_map.py toml --region {region})')
            continue
        a, b = config_shape(usa_cfg.read_text(), False), config_shape(reg_cfg.read_text(), True)
        if a != b:
            import difflib
            d = [l for l in difflib.unified_diff(a, b, f'config/{name}', f'config/{region}/{name}', n=0, lineterm='')][2:8]
            fails.append(f'config/{region}/{name} is not config/{name} translated (rerun survey/region_map.py toml'
                         f' --region {region}): ' + ' | '.join(d))

    rt = root / 'runtime'
    hooks = ha.hooked_functions(root)
    print(f'{region}: {len(found)} guest addresses named by the runtime, {len(table)} in the PAL table, {len(hooks)} hooked functions, '
          f'{sum(1 for a in table if table[a] != a)} at another PAL address')
    originals = set()
    plain = re.compile(r'^\s*PPC_FUNC\(sub_82[0-9A-F]{6}\)\s*(\{.*)?$')
    for p in ha.sources(root):
        text = ha.strip_comments(p.read_text(errors='replace'))
        rel = p.relative_to(rt).as_posix()
        for n, line in enumerate(text.split('\n'), 1):
            if plain.match(line) and not text.split('\n')[n - 2].rstrip().endswith('\\'):
                fails.append(f'{rel}:{n}: a plain PPC_FUNC(sub_...) hook: use SB_HOOK (region/region.h)')
            for m in re.finditer(r'SB_ORIGINAL\((sub_82[0-9A-F]{6})\)', line):
                originals.add((int(m.group(1)[4:], 16), f'{rel}:{n}'))
    for addr, where in sorted(originals):
        if addr not in hooks:
            fails.append(f'{where}: SB_ORIGINAL(sub_{addr:08X}) of a function no SB_HOOK hooks')

    # Code and .rdata constants: their PAL address may differ, so the .cpp
    # files use them through region::Addr.
    cpp_text = {p: ha.strip_comments(p.read_text(errors='replace')) for p in ha.sources(root) if p.suffix in ('.cpp', '.mm')}
    for addr, refs in sorted(found.items()):
        if addr not in table or ha.kind_of(addr, refs) == 'code':
            continue
        how = hows[addr]
        if how.startswith('.data') or how.startswith('outside the image') or how.startswith('the image header'):
            if table[addr] != addr:
                fails.append(f'{addr:#010x}: {how}, yet at {table[addr]:#010x}')
            continue
        for ref in sorted(refs):
            file, line_no = ref.split(' ')[0].rsplit(':', 1)
            path = rt / file
            line = ha.strip_comments(path.read_text(errors='replace')).split('\n')[int(line_no) - 1]
            m = re.search(r'\b(k[A-Z]\w*)\s*=\s*0x' + f'{addr:08X}', line, re.I)
            if m:
                name = m.group(1)
                uses = [(p, l) for p, t in cpp_text.items() for l in t.split('\n')
                        if re.search(r'\b' + name + r'\b', l) and not re.search(r'\b' + name + r'\s*=\s*0x', l)]
                if uses and not any(re.search(r'region::Addr\((?:[\w:]*::)?' + name + r'\)', l) for _, l in uses):
                    fails.append(f'{ref}: {name} ({how}) is used in {uses[0][0].relative_to(rt)} without region::Addr')
            elif not re.search(r'region::Addr\(0x' + f'{addr:08X}', line, re.I):
                fails.append(f'{ref}: {addr:#010x} ({how}) used without region::Addr')

    if len(args) == 4:
        usa_dir, pal_dir, funcmap = args[1:4]
        sys.path.insert(0, str(root / 'survey'))
        import region_map as rm
        m = rm.RegionMap(usa_dir, pal_dir, funcmap)
        if m.usa.bytes(rm.BASE, 4) is None:
            fails.append('the USA image is empty')
        for usa, pal in sorted(table.items()):
            if usa >= rm.BASE + len(m.usa.data):
                continue
            s = m.usa.section(usa)
            if s and s[3] == 'code':
                if not m.same_code(usa, pal, 4):
                    fails.append(f'{usa:#010x} -> {pal:#010x}: the code differs')
            elif m.usa.bytes(usa, 4) != m.pal.bytes(pal, 4) and not hows[usa].endswith('initial bytes differ'):
                fails.append(f'{usa:#010x} -> {pal:#010x}: the data differs')
        r = subprocess.run([sys.executable, '-I', str(root / 'survey/region_map.py'), 'check', usa_dir, pal_dir, funcmap,
                            '--region', region], capture_output=True, text=True)
        print(r.stdout.strip())
        if r.returncode:
            fails.append('survey/region_map.py check: ' + (r.stdout + r.stderr).strip().replace('\n', '; '))
        print(f'bytes checked at {len(table)} USA/PAL address pairs')

    for f in fails:
        print('FAIL', f)
    print('region_addresses_check:', 'FAILED' if fails else 'all checks passed')
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
