#!/usr/bin/env python3
"""The PAL (Spanish) release's addresses for the runtime's hooks
(runtime/region/region.h):

    python3 tests/region_addresses_check.py <repo> [<usa dir> <pal dir> <funcmap.tsv>]

Without the images (the default; no game data needed):
  - every guest address the runtime's sources name (runtime/region/
    hook_addresses.py) has a PAL address in runtime/region/pal_es_addresses.inc,
    and every function they name a PAL name in pal_es_names.h;
  - every hook is an SB_HOOK (a plain PPC_FUNC(sub_...) override would leave
    the PAL code unhooked) and every SB_ORIGINAL names a hooked function;
  - every code or .rdata constant the runtime's .cpp files use is used through
    region::Addr (its PAL address can differ; .data, .bss and addresses outside
    the image are the same in both, which the table records);
  - no address is in one of the 19 functions the PAL build changed.
With the two decrypted images (survey/region_map.py's inputs: <dir>/image.bin
and xexsurvey.txt) and the function map, also: the bytes at each USA address
and its PAL one match (instructions with branch displacements and address
halves masked; data exactly), and the committed table, names and config/pal-es/
are what survey/region_map.py writes from them.
"""
import pathlib
import re
import subprocess
import sys


def main():
    if len(sys.argv) not in (2, 5):
        sys.exit(__doc__)
    root = pathlib.Path(sys.argv[1]).resolve()
    sys.path.insert(0, str(root / 'runtime/region'))
    import hook_addresses as ha

    fails = []
    table = {}
    hows = {}
    for line in (root / 'runtime/region/pal_es_addresses.inc').read_text().splitlines():
        m = re.match(r'SB_PAL_ES_ADDRESS\((0x[0-9A-F]{8}), (0x[0-9A-F]{8})\)\s*// ([^;]*);', line)
        if m:
            usa, pal = int(m.group(1), 16), int(m.group(2), 16)
            if usa in table:
                fails.append(f'{usa:#010x} twice in the table')
            table[usa], hows[usa] = pal, m.group(3)
    names = {}
    for line in (root / 'runtime/region/pal_es_names.h').read_text().splitlines():
        m = re.match(r'#define SB_PAL_ES_sub_([0-9A-F]{8}) sub_([0-9A-F]{8})$', line)
        if m:
            names[int(m.group(1), 16)] = int(m.group(2), 16)

    found = ha.scan(root)
    for addr, refs in sorted(found.items()):
        where = ', '.join(sorted(refs)[:3])
        if addr not in table:
            fails.append(f'{addr:#010x} ({where}): no PAL address; rerun survey/region_map.py table')
            continue
        if ha.kind_of(addr, refs) == 'code' and names.get(addr) != table[addr]:
            fails.append(f'sub_{addr:08X} ({where}): not in pal_es_names.h (or another address)')
        if 'changed' in hows[addr]:
            fails.append(f'{addr:#010x} ({where}): in a function the PAL build changed ({hows[addr]}): review it')
    for addr in table:
        if addr not in found:
            fails.append(f'{addr:#010x}: in the table but no longer in the sources (rerun survey/region_map.py table)')

    rt = root / 'runtime'
    hooks = ha.hooked_functions(root)
    print(f'{len(found)} guest addresses named by the runtime, {len(table)} in the PAL table, {len(hooks)} hooked functions, '
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

    if len(sys.argv) == 5:
        usa_dir, pal_dir, funcmap = sys.argv[2:5]
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
        r = subprocess.run([sys.executable, '-I', str(root / 'survey/region_map.py'), 'check', usa_dir, pal_dir, funcmap],
                           capture_output=True, text=True)
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
