"""The guest addresses the runtime's sources name: every sub_82XXXXXX (a
recompiled function: a hook, or one it calls or names) and every 0x82XXXXXX
literal outside comments (code inside a function: return addresses, ranges;
data: globals, tables, constants). survey/region_map.py maps each to the PAL
(Spanish) default.xex into pal_es_addresses.inc; tests/region_addresses_check.py
checks the table still covers them. Python 3, no dependencies.
"""
import pathlib
import re

USA_XEX_SHA256 = 'aebdf3c14d1e4e4ed2c130d94bc1a0e97bd6d5bf39bdfdef33c3909c9cc4ba4c'
PAL_ES_XEX_SHA256 = 'aad15fc218d034de3131f746e0b763dd8049a40dd676efc87667fcaf4fc72694'

SKIP_DIRS = {'thirdparty'}
SKIP_FILES = {'pal_es_addresses.inc', 'pal_es_names.h'}
EXTENSIONS = {'.cpp', '.h', '.inc', '.mm'}
SUB = re.compile(r'\bsub_(82[0-9A-F]{6})\b')
LIT = re.compile(r'\b0x(82[0-9A-Fa-f]{6})(?:u|U)?\b')
COMMENT = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"', re.S)


def strip_comments(text):
    # Comments go (kept as newlines, so line numbers hold); strings stay.
    def sub(m):
        s = m.group(0)
        return s if s.startswith('"') else '\n' * s.count('\n')
    return COMMENT.sub(sub, text)


def sources(root):
    rt = pathlib.Path(root) / 'runtime'
    for p in sorted(rt.rglob('*')):
        if p.suffix in EXTENSIONS and p.is_file() and not (SKIP_DIRS & set(p.relative_to(rt).parts)) \
                and p.name not in SKIP_FILES:
            yield p


def scan(root):
    """{address: {"file:line sub"|"file:line lit", ...}}"""
    found = {}
    rt = pathlib.Path(root) / 'runtime'
    for p in sources(root):
        text = strip_comments(p.read_text(errors='replace'))
        rel = p.relative_to(rt).as_posix()
        for n, line in enumerate(text.split('\n'), 1):
            for m in SUB.finditer(line):
                found.setdefault(int(m.group(1), 16), set()).add(f'{rel}:{n} sub')
            for m in LIT.finditer(line):
                found.setdefault(int(m.group(1), 16), set()).add(f'{rel}:{n} lit')
    return found


def kind_of(addr, refs):
    """'code' for a function name; None (by section) for a literal."""
    return 'code' if any(r.endswith(' sub') for r in refs) else None


def hooked_functions(root):
    """The functions the runtime overrides: SB_HOOK(sub_X) / HFR_COUNT(sub_X, ...)."""
    hooks = {}
    pat = re.compile(r'\b(?:SB_HOOK|HFR_COUNT)\(\s*sub_(82[0-9A-F]{6})\b')
    rt = pathlib.Path(root) / 'runtime'
    for p in sources(root):
        text = strip_comments(p.read_text(errors='replace'))
        for n, line in enumerate(text.split('\n'), 1):
            if '#define' in line:
                continue
            for m in pat.finditer(line):
                hooks.setdefault(int(m.group(1), 16), []).append(f'{p.relative_to(rt).as_posix()}:{n}')
    return hooks
