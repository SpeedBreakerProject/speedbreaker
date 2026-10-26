#!/usr/bin/env python3
"""Put a second region's XenonRecomp output in a C++ namespace, so it links
into the same binary as the USA code (ppc/):

    tools/XenonRecomp/build/XenonRecomp/XenonRecomp config/pal-es/nfsmw.toml tools/XenonRecomp/XenonUtils/ppc_context.h
    python3 scripts/region_ppc.py ppc-pal-es pal_es

Both outputs name their functions after guest addresses (sub_82XXXXXX), and
many addresses are the same in both, so as written they would collide. In
place, once (a marker line says it was done; run XenonRecomp again first to
redo it):
  - every function the output defines (sub_*, __imp__sub_*, the save/restore
    helpers) goes into `namespace <ns>`, with C++ linkage (the USA's
    __imp__sub_* are extern "C");
  - what it only declares stays global: the imports (__imp__XamShowSigninUI,
    ...: the runtime's, shared by both) and the mid-asm hooks (extern void
    RenderQueuePublish(), ...: one host function for both);
  - PPCFuncMappings becomes <ns>::PPCFuncMappings;
  - ppc_region_info.cpp gives <ns>::kCodeBase/kCodeSize/kImageBase/kImageSize
    (the region's ppc_config.h values; the runtime reads them at startup).
The runtime's hooks (runtime/region/region.h) reach the namespaced code.

XenonRecomp's output has two forms, by the compiler it was built with: on a
Mac, or with GCC or MSVC, each function's body is followed by a wrapper,
`PPC_WEAK_FUNC(sub_X) { __imp__sub_X(ctx, base); }`; built with clang
elsewhere (Linux, SteamOS, Windows' clang-cl) it declares sub_X as an alias
of the body instead, `__attribute__((alias("__imp__sub_X"))) PPC_WEAK_FUNC(sub_X);`
before it. An alias names the body's unmangled symbol, which the namespace
renames, so the alias form is first rewritten to the wrapper form, the same
text the Mac writes: the result is the same files byte for byte on every
system (tests/region_ppc_test.py). Files are read with any line ending
(CRLF after a Windows checkout) and written with LF.
"""
import pathlib
import re
import sys

MARKER = '// Namespaced by scripts/region_ppc.py: '
ALIAS = re.compile(r'__attribute__\(\(alias\("__imp__(\w+)"\)\)\) PPC_WEAK_FUNC\(\1\);')


def wrapper_form(text, name):
    """The alias form's functions as the wrapper form (XenonRecomp's #ifndef
    XENON_RECOMP_USE_ALIAS text): each alias line dropped, and the wrapper
    written after its body's closing brace and the blank line after it."""
    lines = text.split('\n')
    out, pending, i, n = [], None, 0, 0
    while i < len(lines):
        line = lines[i]
        m = ALIAS.fullmatch(line)
        if m:
            if pending is not None:
                sys.exit(f'{name}: the alias for {m.group(1)} comes before {pending}\'s body ends')
            pending = m.group(1)
            n += 1
            i += 1
            continue
        out.append(line)
        if pending is not None and line == '}' and i + 1 < len(lines) and lines[i + 1] == '':
            out += ['', f'PPC_WEAK_FUNC({pending}) {{', f'\t__imp__{pending}(ctx, base);', '}', '']
            pending = None
            i += 2
            continue
        i += 1
    if pending is not None:
        sys.exit(f'{name}: {pending}\'s body has no closing brace')
    return '\n'.join(out), n


def read(p):
    with open(p, encoding='utf-8', newline=None) as f:  # CRLF or LF
        return f.read()


def write(p, text):
    with open(p, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


def main():
    if len(sys.argv) != 3 or not re.fullmatch(r'[a-z_][a-z0-9_]*', sys.argv[2]):
        sys.exit(__doc__)
    d, ns = pathlib.Path(sys.argv[1]), sys.argv[2]
    shared = d / 'ppc_recomp_shared.h'
    text = read(shared)
    if text.startswith(MARKER):
        sys.exit(f'{d} is already namespaced ({text.splitlines()[0][len(MARKER):]}); run XenonRecomp again to redo it')
    cpps = sorted(d.glob('ppc_recomp.*.cpp'))
    if not cpps:
        sys.exit(f'{d}: no ppc_recomp.*.cpp')

    # The wrapper form first (see above), then what the output defines.
    sources, aliases = {}, 0
    for p in cpps:
        sources[p], n = wrapper_form(read(p), p.name)
        aliases += n
    defined = set()
    for p in cpps:
        for m in re.finditer(r'^PPC_(?:FUNC_IMPL|WEAK_FUNC)\((\w+)\)', sources[p], re.M):
            defined.add(m.group(1))

    head, decls = [], []
    for line in text.splitlines():
        m = re.fullmatch(r'PPC_EXTERN_FUNC\((\w+)\);', line)
        if m:
            decls.append((m.group(1), line))
        else:
            head.append(line)
    inner = [l for n, l in decls if n in defined]
    outer = [l for n, l in decls if n not in defined]
    out = [MARKER + ns] + head + ['', '// Declared, not defined here: the imports (the runtime\'s).'] + outer + \
        ['', f'namespace {ns}', '{'] + inner + ['}', '']
    write(shared, '\n'.join(out))

    for p in cpps:
        lines = sources[p].split('\n')
        includes = [l for l in lines if l.startswith('#include')]
        externs = sorted({l for l in lines if l.startswith('extern ')})
        body = [l.replace('PPC_FUNC_IMPL(', 'PPC_FUNC(', 1) if l.startswith('PPC_FUNC_IMPL(') else l
                for l in lines if not l.startswith('#include') and not l.startswith('extern ')]
        write(p, '\n'.join([MARKER + ns] + includes + [''] + externs + ['', f'namespace {ns}', '{'] + body + ['}', '']))

    fm = d / 'ppc_func_mapping.cpp'
    t = read(fm)
    start = t.index('PPCFuncMapping PPCFuncMappings[]')
    write(fm, MARKER + ns + '\n' + t[:start] + f'namespace {ns}\n{{\n' + t[start:].rstrip('\n') + '\n}\n')

    write(d / 'ppc_region_info.cpp',
        MARKER + ns + '\n'
        '#include "ppc_config.h"\n'
        '#include <cstdint>\n\n'
        f'namespace {ns}\n{{\n'
        '    extern const uint64_t kImageBase = PPC_IMAGE_BASE, kImageSize = PPC_IMAGE_SIZE;\n'
        '    extern const uint64_t kCodeBase = PPC_CODE_BASE, kCodeSize = PPC_CODE_SIZE;\n'
        '}\n')
    form = f'the alias form ({aliases} aliases) rewritten to the wrapper form' if aliases else 'the wrapper form'
    print(f'{d}: {len(cpps)} files, {len(inner)} functions in namespace {ns}, {len(outer)} imports global; {form}')


if __name__ == '__main__':
    main()
