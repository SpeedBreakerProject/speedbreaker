#!/usr/bin/env python3
"""scripts/region_ppc.py on both forms of XenonRecomp's output.

    region_ppc_test.py <region_ppc.py>
        A made-up recompiler output (a few functions, imports, a mid-asm hook,
        the save/restore helpers) written in the wrapper form (XenonRecomp
        built on a Mac or with MSVC), in the alias form (clang elsewhere:
        Linux, SteamOS, Windows' clang-cl) and in the alias form with CRLF line
        ends (a Windows checkout), each namespaced by region_ppc.py: all three
        must come out byte for byte the same, and namespaced as the runtime
        needs (what the output defines in the namespace, the imports and hooks
        global, the wrappers there, no alias left). A second run must refuse.
    region_ppc_test.py <region_ppc.py> <raw output A> <raw output B>
        The same with two real outputs of XenonRecomp for one disc (e.g. a
        Mac's and a Linux machine's): copies of both namespaced, every file
        must match.
"""
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

NS = 'pal_es'
SUBS = ['sub_82000010', 'sub_82000040', 'sub_82000088']
HELPERS = ['__savegprlr_14', '__restgprlr_14']
IMPORTS = ['__imp__XamShowSigninUI', '__imp__NtCreateEvent']
HOOK = 'extern void RenderQueuePublish();'


def body(name, i):
    lines = [f'PPC_FUNC_IMPL(__imp__{name}) {{', '\tPPC_FUNC_PROLOGUE();']
    if i == 1:
        lines += ['loc_82000048:', '\t// bl 0x82000010', '\tctx.lr = 0x8200004C;', f'\t{SUBS[0]}(ctx, base);',
                  '\t// mid-asm hook', '\tRenderQueuePublish();', '\t// bl XamShowSigninUI',
                  f'\t{IMPORTS[0]}(ctx, base);']
    lines += [f'\tctx.r3.u64 = {i};', '\t// blr', '\treturn;', '}']
    return lines


def output(alias, crlf):
    """{file name: text} of a made-up XenonRecomp output."""
    names = SUBS + HELPERS
    files = {}
    files['ppc_config.h'] = '#pragma once\n#define PPC_IMAGE_BASE 0x82000000ull\n#define PPC_IMAGE_SIZE 0x100000ull\n' \
                            '#define PPC_CODE_BASE 0x82000000ull\n#define PPC_CODE_SIZE 0x1000ull\n'
    files['ppc_context.h'] = '#pragma once\n// (the context header)\n'
    files['ppc_recomp_shared.h'] = '\n'.join(['#pragma once', '', '#include "ppc_config.h"', '#include "ppc_context.h"', ''] +
                                             [f'PPC_EXTERN_FUNC({n});' for n in IMPORTS + names] + [''])
    files['ppc_func_mapping.cpp'] = '\n'.join(['#include "ppc_recomp_shared.h"', '', 'PPCFuncMapping PPCFuncMappings[] = {'] +
                                              [f'\t{{ 0x{0x82000010 + 0x30 * i:08X}, {n} }},' for i, n in enumerate(SUBS)] +
                                              ['\t{ 0, nullptr }', '};', ''])
    for f, group in (('ppc_recomp.0.cpp', SUBS[:2]), ('ppc_recomp.1.cpp', SUBS[2:] + HELPERS)):
        lines = ['#include "ppc_recomp_shared.h"', '']
        for n in group:
            i = names.index(n)
            if i == 1:
                lines.append(HOOK)
                lines.append('')
            if alias:
                lines.append(f'__attribute__((alias("__imp__{n}"))) PPC_WEAK_FUNC({n});')
            lines += body(n, i) + ['']
            if not alias:
                lines += [f'PPC_WEAK_FUNC({n}) {{', f'\t__imp__{n}(ctx, base);', '}', '']
        files[f] = '\n'.join(lines)
    if crlf:
        files = {k: v.replace('\n', '\r\n') for k, v in files.items()}
    return files


def namespace(script, d):
    r = subprocess.run([sys.executable, '-I', script, str(d), NS], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f'FAIL: region_ppc.py on {d.name}: rc {r.returncode}: {r.stdout}{r.stderr}')
    return r.stdout.strip()


def tree(d):
    return {p.name: p.read_bytes() for p in sorted(d.iterdir())}


def same(a, b, what, untouched_crlf=False):
    ta, tb = tree(a), tree(b)
    if untouched_crlf:  # the headers region_ppc.py doesn't write keep their line ends
        tb = {k: v.replace(b'\r\n', b'\n') if k in ('ppc_config.h', 'ppc_context.h') else v for k, v in tb.items()}
    if ta.keys() != tb.keys():
        sys.exit(f'FAIL: {what}: different files: {sorted(ta.keys() ^ tb.keys())}')
    diff = [k for k in ta if ta[k] != tb[k]]
    if diff:
        sys.exit(f'FAIL: {what}: {len(diff)} files differ: {diff[:5]}')
    return len(ta)


def check_namespaced(d):
    """What the runtime needs of the namespaced output."""
    fails = []
    shared = (d / 'ppc_recomp_shared.h').read_text()
    if not shared.startswith(f'// Namespaced by scripts/region_ppc.py: {NS}\n'):
        fails.append('ppc_recomp_shared.h: no marker line')
    outer, _, inner = shared.partition(f'namespace {NS}\n{{')
    for n in IMPORTS:
        if f'PPC_EXTERN_FUNC({n});' not in outer:
            fails.append(f'the import {n} is not declared global')
    for n in SUBS + HELPERS:
        if f'PPC_EXTERN_FUNC({n});' not in inner or f'PPC_EXTERN_FUNC({n});' in outer:
            fails.append(f'{n} is not declared in namespace {NS} (only)')
    for f in ('ppc_recomp.0.cpp', 'ppc_recomp.1.cpp'):
        t = (d / f).read_text()
        head, _, rest = t.partition(f'namespace {NS}\n{{')
        if not rest.rstrip().endswith('}'):
            fails.append(f'{f}: the code is not in namespace {NS}')
        if 'alias(' in t or 'PPC_FUNC_IMPL(' in t:
            fails.append(f'{f}: an alias or PPC_FUNC_IMPL is left')
        if f == 'ppc_recomp.0.cpp' and (HOOK not in head or HOOK in rest):
            fails.append(f'{f}: the hook declaration is not global')
        for n in re.findall(r'^PPC_FUNC\(__imp__(\w+)\) \{$', rest, re.M):
            if f'PPC_WEAK_FUNC({n}) {{\n\t__imp__{n}(ctx, base);\n}}\n' not in rest:
                fails.append(f'{f}: no wrapper for {n}')
    defined = sorted(re.findall(r'^PPC_FUNC\(__imp__(\w+)\)', ''.join((d / f).read_text() for f in ('ppc_recomp.0.cpp', 'ppc_recomp.1.cpp')), re.M))
    if defined != sorted(SUBS + HELPERS):
        fails.append(f'defined: {defined}')
    fm = (d / 'ppc_func_mapping.cpp').read_text()
    if f'namespace {NS}\n{{\nPPCFuncMapping PPCFuncMappings[]' not in fm:
        fails.append('ppc_func_mapping.cpp: PPCFuncMappings not in the namespace')
    ri = (d / 'ppc_region_info.cpp').read_text()
    if f'namespace {NS}' not in ri or 'kCodeBase = PPC_CODE_BASE' not in ri:
        fails.append('ppc_region_info.cpp')
    if any(b'\r' in v for v in tree(d).values()):
        fails.append('a CR is left')
    if fails:
        sys.exit('FAIL: ' + '; '.join(fails))


def main():
    if len(sys.argv) not in (2, 4):
        sys.exit(__doc__)
    script = sys.argv[1]
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        if len(sys.argv) == 4:
            a, b = tmp / 'a', tmp / 'b'
            shutil.copytree(sys.argv[2], a)
            shutil.copytree(sys.argv[3], b)
            print('A:', namespace(script, a))
            print('B:', namespace(script, b))
            n = same(a, b, 'the two outputs namespaced')
            print(f'PASS: {n} files the same')
            return
        dirs = {}
        for name, alias, crlf in (('wrapper', False, False), ('alias', True, False), ('alias-crlf', True, True)):
            d = tmp / name
            d.mkdir()
            for f, text in output(alias, crlf).items():
                (d / f).write_bytes(text.encode())
            print(f'{name}:', namespace(script, d))
            dirs[name] = d
        check_namespaced(dirs['wrapper'])
        n = same(dirs['wrapper'], dirs['alias'], 'the alias form vs the wrapper form')
        same(dirs['wrapper'], dirs['alias-crlf'], 'the alias form with CRLF vs the wrapper form', untouched_crlf=True)
        # Already namespaced: refused, nothing changed.
        before = tree(dirs['alias'])
        r = subprocess.run([sys.executable, '-I', script, str(dirs['alias']), NS], capture_output=True, text=True)
        if r.returncode == 0 or tree(dirs['alias']) != before:
            sys.exit('FAIL: a second run did not refuse')
        print(f'PASS: the wrapper form, the alias form and the alias form with CRLF namespace to the same {n} files')


if __name__ == '__main__':
    main()
