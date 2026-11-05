#!/usr/bin/env python3
"""Every disc version (region) has every hook the USA one has:

    python3 tests/region_parity_check.py <repo>

A fix written against the USA build lands in config/nfsmw.toml and the
runtime's sources; each other region (config/<region>/, runtime/region/
<region>_*, made by survey/region_map.py from that region's disc) has to get
the same change, or that disc's build silently misses the fix. v0.2.2's
UltrawideShadowFit midasm hook (#81) is the example: config/pal-es has it,
a config/pal-de made from v0.2.1 doesn't, and nothing that needs no German
image noticed. This check needs no game image. For every region in the tree
(any config/<region>/nfsmw.toml, runtime/region/<region>_addresses.inc or
runtime/install/manifest_<region>.inc), against the USA's:

  - config/<region>/nfsmw.toml: the same keys, the same [[midasm_hook]]s by
    name (as many of each, the same registers and flags), the same leaf
    functions (by size, in order), as many secondary entries, the same
    [main] settings (addresses and the region's own paths may differ);
  - config/<region>/switch_tables.toml: the same jump tables (index register
    and label count, in order);
  - runtime/region/<region>_addresses.inc and <region>_names.h: an entry for
    every guest address and function the runtime's sources name
    (runtime/region/hook_addresses.py, the USA's set by definition: the
    sources are written against the USA addresses), every SB_HOOK and
    SB_ORIGINAL among them, and nothing the sources no longer name;
  - the region is wired in everywhere a region is (WIRING below).

Differences a region has on purpose are listed in ALLOWED, each with its
reason; anything else fails, naming the missing (or extra) entry and its
region, and an ALLOWED entry its region no longer needs fails too (so the
list can't go stale). Region-only code is matched by its own (region)
address, which survey/region_map.py writes from the region's disc.

NFS: Carbon (config/carbon/carbon.toml) is checked the same way once it has
a region: config/carbon/<region>/carbon.toml and switch_tables.toml (GAMES
below). Its runtime tables don't exist yet; when they do, give its Game a
`runtime` entry like Most Wanted's.

Python 3.11+ (tomllib); older ones skip (exit 77).
"""
import difflib
import pathlib
import re
import sys

try:
    import tomllib
except ImportError:  # Python < 3.11
    tomllib = None

SKIP = 77  # ctest's SKIP_RETURN_CODE (tests/CMakeLists.txt)

# Intentional differences: (region, kind, key): why. Kinds:
#   'function'    a region-only leaf function in the config's functions list,
#                 key = (its address, its size) in that region;
#   'switch'      a region-only jump table in its switch_tables.toml, key = base;
#   'midasm_hook' a region-only [[midasm_hook]], key = its name;
#   'wiring'      a WIRING line the region doesn't need, key = the file.
ALLOWED = {
    # The PAL build's two extra leaf functions (no .pdata), between the USA's
    # 0x823AEEC0 and 0x823AF130: the date and number formats' per-language
    # strings, called by the functions the PAL build changed
    # (survey/region_map.py PAL_ES_EXTRA_FUNCTIONS), and their jump tables.
    ('pal-es', 'function', (0x823AF018, 0xE0)): 'PAL-only leaf: the date format strings by language',
    ('pal-es', 'function', (0x823AF0F8, 0xE0)): 'PAL-only leaf: the number format strings by language',
    ('pal-es', 'switch', 0x823AF028): 'the jump table of the PAL-only leaf at 0x823AF018',
    ('pal-es', 'switch', 0x823AF108): 'the jump table of the PAL-only leaf at 0x823AF0F8',
    # The German disc (public PR 88) is laid out like the Spanish one: the same
    # two leaves at the same addresses (survey/region_map.py's
    # find_extra_functions finds them; PR 88 review, issues/pr88/REVIEW.md).
    ('pal-de', 'function', (0x823AF018, 0xE0)): 'PAL-only leaf: the date format strings by language',
    ('pal-de', 'function', (0x823AF0F8, 0xE0)): 'PAL-only leaf: the number format strings by language',
    ('pal-de', 'switch', 0x823AF028): 'the jump table of the PAL-only leaf at 0x823AF018',
    ('pal-de', 'switch', 0x823AF108): 'the jump table of the PAL-only leaf at 0x823AF0F8',
    # tests/region_addresses_check.py checks pal-es with no --region (its
    # default), so its ctest line names no region.
    ('pal-es', 'wiring', 'tests/CMakeLists.txt'): 'region_addresses_check\'s default region',
}

# [main] settings that are the region's own (paths to its disc and output).
REGION_PATHS = {'file_path', 'out_directory_path'}


class Game:
    def __init__(self, name, toml, region_toml, switch, region_switch, runtime):
        self.name, self.toml, self.region_toml = name, toml, region_toml
        self.switch, self.region_switch, self.runtime = switch, region_switch, runtime


# {id}: the region id (pal-es); {stem}: pal_es; {STEM}: PAL_ES; {Camel}: PalEs.
# Each is a file every region must be named in, the text it needs, and what
# that text does.
WIRING = [
    ('runtime/region/region.cpp', '"{stem}_addresses.inc"', 'its address table, compiled in'),
    ('runtime/region/region.cpp', '"{id}"', 'its region id (region::Id / FromId)'),
    ('runtime/region/region.h', 'SB_{STEM}', 'SB_HOOK\'s forwarder and SB_ORIGINAL\'s choice for it'),
    ('runtime/region/hook_addresses.py', '{STEM}_XEX_SHA256', 'its default.xex hash'),
    ('runtime/install/manifest.cpp', '"manifest_{stem}.inc"', 'its install manifest'),
    ('recomp/CMakeLists.txt', 'ppc-{id}', 'its recompiled code, linked'),
    ('runtime/CMakeLists.txt', 'SB_{STEM}=1', 'the runtime built with its code'),
    ('scripts/ios/build.sh', 'libNfsmwRecomp{Camel}Lib.a', 'its code in the iOS link'),
    ('survey/region_map.py', '{id}', 'the generator that writes its config and tables'),
    ('tests/CMakeLists.txt', '--region {id}', 'its region_addresses_check'),
]

GAMES = [
    Game('Most Wanted', 'config/nfsmw.toml', 'config/{id}/nfsmw.toml',
         'config/switch_tables.toml', 'config/{id}/switch_tables.toml', runtime=True),
    # No regions yet (only the USA disc): checked once config/carbon/<region>/ exists.
    Game('Carbon', 'config/carbon/carbon.toml', 'config/carbon/{id}/carbon.toml',
         'config/carbon/switch_tables.toml', 'config/carbon/{id}/switch_tables.toml', runtime=False),
]

# Not regions: other games' manifests (runtime/install/manifest_<x>.inc).
NOT_REGIONS = {'carbon'}


def is_addr(v):
    return isinstance(v, int) and not isinstance(v, bool) and 0x82000000 <= v < 0x84000000


def hexs(v):
    return f'0x{v:08X}' if isinstance(v, int) else repr(v)


def forms(rid):
    stem = rid.replace('-', '_')
    return {'id': rid, 'stem': stem, 'STEM': stem.upper(), 'Camel': ''.join(p.capitalize() for p in stem.split('_'))}


class Checker:
    def __init__(self, root):
        self.root = root
        self.fails = []
        self.used = set()
        self.present = set()

    def fail(self, region, msg):
        self.fails.append(f'{region}: {msg}')

    def allowed(self, region, kind, key):
        if (region, kind, key) in ALLOWED:
            self.used.add((region, kind, key))
            return True
        return False

    # ---- config/<region>/*.toml ------------------------------------------

    def compare_value(self, region, where, usa, reg):
        """A setting: addresses may differ (present in both is enough); anything else must match."""
        if is_addr(usa) and is_addr(reg):
            return
        if isinstance(usa, list) and isinstance(reg, list) and usa and all(is_addr(x) for x in usa) \
                and all(is_addr(x) for x in reg):
            if len(usa) != len(reg):
                self.fail(region, f'{where}: {len(reg)} entries, USA {len(usa)}')
            return
        if usa != reg:
            self.fail(region, f'{where}: {reg!r}, USA {usa!r}')

    def compare_hooks(self, region, rel, usa_rel, kind, usa, reg):
        """[[midasm_hook]]-style lists: by name; as many of each; the same non-address keys."""
        def by_name(items):
            d = {}
            for h in items:
                d.setdefault(h.get('name'), []).append(h)
            return d
        u, r = by_name(usa), by_name(reg)
        for name in u:
            if name not in r:
                at = ', '.join(hexs(h.get('address')) for h in u[name])
                self.fail(region, f'{rel} has no [[{kind}]] "{name}" ({usa_rel} has it at {at}): '
                                  f'regenerate it with survey/region_map.py toml for {region}, or add it by hand '
                                  f'at {region}\'s address of the same instruction')
                continue
            if len(u[name]) != len(r[name]):
                self.fail(region, f'{rel} has {len(r[name])} [[{kind}]] "{name}", {usa_rel} {len(u[name])} '
                                  f'(USA at {", ".join(hexs(h.get("address")) for h in u[name])})')
            for i, (uh, rh) in enumerate(zip(u[name], r[name])):
                for key in sorted(set(uh) | set(rh)):
                    if key not in rh:
                        self.fail(region, f'{rel} [[{kind}]] "{name}" #{i + 1}: no {key} '
                                          f'(USA {key} = {uh[key]!r})')
                    elif key not in uh:
                        self.fail(region, f'{rel} [[{kind}]] "{name}" #{i + 1}: {key} = {rh[key]!r}, not in USA\'s')
                    else:
                        self.compare_value(region, f'{rel} [[{kind}]] "{name}" #{i + 1} {key}', uh[key], rh[key])
        for name in r:
            if name not in u and not self.allowed(region, kind, name):
                self.fail(region, f'{rel} has [[{kind}]] "{name}", which {usa_rel} doesn\'t: '
                                  f'add it there too, or to ALLOWED in tests/region_parity_check.py with the reason')

    def compare_functions(self, region, rel, usa_rel, usa, reg):
        """The leaf functions list: matched in order by size (region_map.py translates it line by line)."""
        us = [f.get('size') for f in usa]
        rs = [f.get('size') for f in reg]
        sm = difflib.SequenceMatcher(None, us, rs, autojunk=False)
        for op, i1, i2, j1, j2 in sm.get_opcodes():
            if op == 'equal':
                continue
            for f in usa[i1:i2]:
                self.fail(region, f'{rel} lacks the function {usa_rel} has at USA {hexs(f.get("address"))} '
                                  f'(size {f.get("size", 0):#x})')
            for f in reg[j1:j2]:
                key = (f.get('address'), f.get('size'))
                if not self.allowed(region, 'function', key):
                    self.fail(region, f'{rel} has a function {usa_rel} doesn\'t: {hexs(key[0])} (size {key[1]:#x}); '
                                      'add it there too, or to ALLOWED with the reason')
        return len(rs) - len(us)

    def compare_doc(self, region, rel, usa_rel, usa, reg, path=''):
        for key in usa:
            where = f'{rel} {path}{key}'
            if key not in reg:
                self.fail(region, f'{where}: missing ({usa_rel} has it)')
                continue
            u, r = usa[key], reg[key]
            if isinstance(u, dict) and isinstance(r, dict):
                self.compare_doc(region, rel, usa_rel, u, r, f'{path}{key}.')
            elif isinstance(u, list) and u and all(isinstance(x, dict) for x in u) and all('name' in x for x in u):
                self.compare_hooks(region, rel, usa_rel, key, u, r if isinstance(r, list) else [])
            elif key == 'functions' and isinstance(u, list) and isinstance(r, list):
                self.compare_functions(region, rel, usa_rel, u, r)
            elif key in REGION_PATHS:
                if not isinstance(r, str) or not r:
                    self.fail(region, f'{where}: {r!r}, a path expected')
            else:
                self.compare_value(region, where, u, r)
        for key in reg:
            if key not in usa:
                if isinstance(reg[key], list) and all(isinstance(x, dict) and 'name' in x for x in reg[key]):
                    self.compare_hooks(region, rel, usa_rel, key, [], reg[key])
                else:
                    self.fail(region, f'{rel} {path}{key}: not in {usa_rel}')

    def compare_switches(self, region, rel, usa_rel, usa, reg):
        def sig(t):
            return (t.get('r'), len(t.get('labels', [])), 'default' in t)
        us, rs = usa.get('switch', []), reg.get('switch', [])
        sm = difflib.SequenceMatcher(None, [sig(t) for t in us], [sig(t) for t in rs], autojunk=False)
        for op, i1, i2, j1, j2 in sm.get_opcodes():
            if op == 'equal':
                continue
            for t in us[i1:i2]:
                self.fail(region, f'{rel} lacks the jump table {usa_rel} has at USA base {hexs(t.get("base"))} '
                                  f'(r{t.get("r")}, {len(t.get("labels", []))} labels)')
            for t in rs[j1:j2]:
                if not self.allowed(region, 'switch', t.get('base')):
                    self.fail(region, f'{rel} has a jump table {usa_rel} doesn\'t: base {hexs(t.get("base"))} '
                                      f'(r{t.get("r")}, {len(t.get("labels", []))} labels); add it there too, or to ALLOWED')
        for key in sorted(set(usa) ^ set(reg)):
            self.fail(region, f'{rel}: top-level {key!r} in only one of it and {usa_rel}')
        return len(us), len(rs)

    def load(self, region, rel):
        try:
            return tomllib.loads((self.root / rel).read_text())
        except FileNotFoundError:
            self.fail(region, f'{rel}: missing')
        except tomllib.TOMLDecodeError as e:
            self.fail(region, f'{rel}: not valid TOML ({e})')
        return None

    def check_config(self, game, region):
        f = forms(region)
        rel, srel = game.region_toml.format(**f), game.region_switch.format(**f)
        usa, reg = self.load('USA', game.toml), self.load(region, rel)
        if usa is not None and reg is not None:
            self.compare_doc(region, rel, game.toml, usa, reg)
        usw, rsw = self.load('USA', game.switch), self.load(region, srel)
        if usw is not None and rsw is not None:
            self.compare_switches(region, srel, game.switch, usw, rsw)
        if usa is None or reg is None:
            return f'{rel} unreadable'
        hooks = sum(len(v) for k, v in reg.items() if isinstance(v, list) and v and isinstance(v[0], dict) and 'name' in v[0])
        fns = len(reg.get('main', {}).get('functions', []))
        tables = len(rsw.get('switch', [])) if rsw else 0
        return f'{hooks} midasm hooks, {fns} leaf functions, {tables} jump tables'

    # ---- runtime/region/<region>_* ---------------------------------------

    def check_runtime(self, region):
        f = forms(region)
        sys.path.insert(0, str(self.root / 'runtime/region'))
        import hook_addresses as ha
        found = ha.scan(self.root)
        code = {a for a, refs in found.items() if ha.kind_of(a, refs) == 'code'}
        hooks = ha.hooked_functions(self.root)
        tpath = f'runtime/region/{f["stem"]}_addresses.inc'
        npath = f'runtime/region/{f["stem"]}_names.h'
        table, names = set(), set()
        try:
            for line in (self.root / tpath).read_text().splitlines():
                m = re.match(rf'SB_{f["STEM"]}_ADDRESS\((0x[0-9A-Fa-f]{{8}}),', line)
                if m:
                    table.add(int(m.group(1), 16))
        except FileNotFoundError:
            self.fail(region, f'{tpath}: missing (survey/region_map.py table writes it)')
            table = None
        try:
            for line in (self.root / npath).read_text().splitlines():
                m = re.match(rf'#define SB_{f["STEM"]}_sub_([0-9A-Fa-f]{{8}}) sub_[0-9A-Fa-f]{{8}}\s*$', line)
                if m:
                    names.add(int(m.group(1), 16))
        except FileNotFoundError:
            self.fail(region, f'{npath}: missing (survey/region_map.py table writes it)')
            names = None
        for gen in (f'{f["stem"]}_addresses.inc', f'{f["stem"]}_names.h'):
            if gen not in ha.SKIP_FILES:
                self.fail(region, f'runtime/region/hook_addresses.py SKIP_FILES lacks {gen} '
                                  '(its PAL addresses would be scanned as the runtime\'s own)')
        if table is not None:
            for a in sorted(set(found) - table):
                self.fail(region, f'{tpath} lacks {a:#010x} ({", ".join(sorted(found[a])[:3])}): '
                                  f'rerun survey/region_map.py table for {region}')
            for a in sorted(table - set(found)):
                self.fail(region, f'{tpath} has {a:#010x}, which the runtime no longer names: '
                                  f'rerun survey/region_map.py table for {region}')
        if names is not None:
            for a in sorted(code - names):
                what = f'SB_HOOK at {", ".join(hooks[a][:2])}' if a in hooks else ', '.join(sorted(found[a])[:3])
                self.fail(region, f'{npath} lacks sub_{a:08X} ({what}): rerun survey/region_map.py table for {region}')
            for a in sorted(names - code):
                self.fail(region, f'{npath} has sub_{a:08X}, a function the runtime no longer names')
        return f'{len(table or ())} addresses, {len(names or ())} function names ({len(hooks)} hooked)'

    def check_wiring(self, region):
        f = forms(region)
        for rel, pattern, what in WIRING:
            text = pattern.format(**f)
            try:
                body = (self.root / rel).read_text(errors='replace')
            except FileNotFoundError:
                body = ''
            if text not in body and not self.allowed(region, 'wiring', rel):
                self.fail(region, f'{rel} doesn\'t name {text!r} ({what})')
        manifest = f'runtime/install/manifest_{f["stem"]}.inc'
        if not (self.root / manifest).exists():
            self.fail(region, f'{manifest}: missing (scripts/gen_install_manifest.py writes it)')

    # ---- the regions -----------------------------------------------------

    def regions(self, game):
        found = set()
        top = (self.root / game.region_toml).parent.parent
        name = pathlib.Path(game.region_toml).name
        if top.is_dir():
            for d in sorted(top.iterdir()):
                if d.is_dir() and (d / name).exists():
                    found.add(d.name)
        if game.runtime:
            for p in (self.root / 'runtime/region').glob('*_addresses.inc'):
                found.add(p.name[:-len('_addresses.inc')].replace('_', '-'))
            for p in (self.root / 'runtime/install').glob('manifest_*.inc'):
                stem = p.name[len('manifest_'):-len('.inc')]
                if stem not in NOT_REGIONS:
                    found.add(stem.replace('_', '-'))
        return sorted(found)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    if tomllib is None:
        print('region_parity_check: skipped (needs Python 3.11+ for tomllib)')
        sys.exit(SKIP)
    root = pathlib.Path(sys.argv[1]).resolve()
    c = Checker(root)
    for game in GAMES:
        if not (root / game.toml).exists():
            print(f'{game.name}: no {game.toml} in this tree')
            continue
        regions = c.regions(game)
        if not regions:
            print(f'{game.name}: USA only ({game.toml}), nothing to compare')
            continue
        for region in regions:
            c.present.add(region)
            parts = [c.check_config(game, region)]
            if game.runtime:
                parts.append(c.check_runtime(region))
                c.check_wiring(region)
            print(f'{game.name} {region}: ' + '; '.join(parts))
    for key in sorted(set(ALLOWED) - c.used, key=str):
        if key[0] in c.present:
            region, kind, k = key
            shown = f'({hexs(k[0])}, {k[1]:#x})' if isinstance(k, tuple) else hexs(k) if isinstance(k, int) else repr(k)
            c.fail(region, f'ALLOWED has {kind} {shown} ("{ALLOWED[key]}"), which {region} no longer differs by: '
                           'remove it from tests/region_parity_check.py')
    for f in c.fails:
        print('FAIL', f)
    print('region_parity_check:', 'FAILED' if c.fails else 'every region matches the USA')
    sys.exit(1 if c.fails else 0)


if __name__ == '__main__':
    main()
