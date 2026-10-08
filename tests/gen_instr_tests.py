"""Generate instruction tests (Xenia's .s format) for instructions we added to
XenonRecomp that Xenia has no tests for, or too few.

Expected values come from an independent Python model of the PowerPC
semantics (big-endian element order, as written in the tests), never from the
recompiler, so a codegen bug can't agree with itself.

    python3 tests/gen_instr_tests.py     # writes tests/instr/nfsmw_*.s
"""
import os
import random
import struct

OUT = os.path.join(os.path.dirname(__file__), 'instr')
rng = random.Random(0x360)


# --- vector helpers: a vector is 4 big-endian words [w0, w1, w2, w3] ---------

def to_bytes(ws):
    return b''.join(struct.pack('>I', w) for w in ws)

def from_bytes(b):
    return [struct.unpack('>I', b[i:i + 4])[0] for i in range(0, 16, 4)]

def elems(ws, size, signed):
    fmt = {1: 'b', 2: 'h', 4: 'i'}[size]
    fmt = fmt if signed else fmt.upper()
    return list(struct.unpack('>' + fmt * (16 // size), to_bytes(ws)))

def pack(values, size, signed):
    fmt = {1: 'b', 2: 'h', 4: 'i'}[size]
    fmt = fmt if signed else fmt.upper()
    return from_bytes(struct.pack('>' + fmt * (16 // size), *values))

def sat(x, lo, hi):
    return max(lo, min(hi, x))

def vec(ws):
    return '[' + ', '.join(f'{w:08X}' for w in ws) + ']'

def rand_vec(size=None):
    """Random vector, biased toward edge values for the element size."""
    if size is None or rng.random() < 0.3:
        return [rng.getrandbits(32) for _ in range(4)]
    n, bits = 16 // size, size * 8
    edges = [0, 1, (1 << bits) - 1, 1 << (bits - 1), (1 << (bits - 1)) - 1]
    vals = [rng.choice(edges) if rng.random() < 0.4 else rng.getrandbits(bits) for _ in range(n)]
    return pack(vals, size, False)


# --- reference models --------------------------------------------------------

def shift_op(size, kind):
    bits = size * 8
    mask = (1 << bits) - 1
    def f(a, b):
        ua, sa, sh = elems(a, size, False), elems(a, size, True), elems(b, size, False)
        out = []
        for x, sx, s in zip(ua, sa, sh):
            s &= bits - 1
            if kind == 'sl': out.append((x << s) & mask)
            elif kind == 'sr': out.append(x >> s)
            elif kind == 'sra': out.append((sx >> s) & mask)
            elif kind == 'rl': out.append(((x << s) | (x >> (bits - s))) & mask)
        return pack(out, size, False)
    return f

def sat_arith(size, signed, op):
    bits = size * 8
    lo, hi = (-(1 << (bits - 1)), (1 << (bits - 1)) - 1) if signed else (0, (1 << bits) - 1)
    def f(a, b):
        return pack([sat(op(x, y), lo, hi) for x, y in zip(elems(a, size, signed), elems(b, size, signed))], size, signed)
    return f

def modular(size, op):
    mask = (1 << (size * 8)) - 1
    def f(a, b):
        return pack([op(x, y) & mask for x, y in zip(elems(a, size, False), elems(b, size, False))], size, False)
    return f

def elementwise(size, signed, op):
    def f(a, b):
        return pack([op(x, y) for x, y in zip(elems(a, size, signed), elems(b, size, signed))], size, signed)
    return f

def pack_op(in_size, in_signed, out_signed):
    out_bits = in_size * 4
    lo, hi = (-(1 << (out_bits - 1)), (1 << (out_bits - 1)) - 1) if out_signed else (0, (1 << out_bits) - 1)
    def f(a, b):
        # vD = sat(vA elements) then sat(vB elements), in element order.
        vals = [sat(x, lo, hi) for x in elems(a, in_size, in_signed) + elems(b, in_size, in_signed)]
        return pack(vals, in_size // 2, out_signed)
    return f

def compare(size, signed, op):
    mask = (1 << (size * 8)) - 1
    def f(a, b):
        return pack([mask if op(x, y) else 0 for x, y in zip(elems(a, size, signed), elems(b, size, signed))], size, False)
    return f


VECTOR_BINOPS = {
    # name: (model, element size for biased inputs)
    'vslh': (shift_op(2, 'sl'), 2),
    'vsrh': (shift_op(2, 'sr'), 2),
    'vsrah': (shift_op(2, 'sra'), 2),
    'vrlh': (shift_op(2, 'rl'), 2),
    'vsrab': (shift_op(1, 'sra'), 1),
    'vaddsws': (sat_arith(4, True, lambda x, y: x + y), 4),
    'vaddsbs': (sat_arith(1, True, lambda x, y: x + y), 1),
    'vsubshs': (sat_arith(2, True, lambda x, y: x - y), 2),
    'vsububm': (modular(1, lambda x, y: x - y), 1),
    'vmaxsh': (elementwise(2, True, max), 2),
    'vminsh': (elementwise(2, True, min), 2),
    'vavguh': (elementwise(2, False, lambda x, y: (x + y + 1) >> 1), 2),
    'vandc': (lambda a, b: [x & ~y & 0xFFFFFFFF for x, y in zip(a, b)], None),
    'vpkswss': (pack_op(4, True, True), 4),
    'vpkswus': (pack_op(4, True, False), 4),
    'vpkshss': (pack_op(2, True, True), 2),
    'vpkuhus': (pack_op(2, False, False), 2),
    'vcmpgtsh': (compare(2, True, lambda x, y: x > y), 2),
    'vcmpgtsw': (compare(4, True, lambda x, y: x > y), 4),
    'vcmpequh': (compare(2, False, lambda x, y: x == y), 2),
    'vcmpgtuh': (compare(2, False, lambda x, y: x > y), 2),
}

RECORD_FORMS = ['vcmpgtsh', 'vcmpgtsw', 'vcmpequh', 'vcmpgtuh']


def write(name, tests):
    with open(os.path.join(OUT, f'nfsmw_{name}.s'), 'w') as f:
        f.write(f'# Generated by tests/gen_instr_tests.py (independent Python model). Do not edit.\n\n')
        for i, t in enumerate(tests, 1):
            f.write(f'test_nfsmw_{name}_{i}:\n' + t + '\n')


def main():
    os.makedirs(OUT, exist_ok=True)
    for f in os.listdir(OUT):
        if f.startswith('nfsmw_'):
            os.remove(os.path.join(OUT, f))

    for name, (model, size) in VECTOR_BINOPS.items():
        tests = []
        for _ in range(8):
            a, b = rand_vec(size), rand_vec(size)
            if name == 'vcmpequh' and rng.random() < 0.5:
                b = list(a)  # make equality actually happen
            d = model(a, b)
            tests.append(
                f'  #_ REGISTER_IN v4 {vec(a)}\n'
                f'  #_ REGISTER_IN v5 {vec(b)}\n'
                f'  {name} v3, v4, v5\n'
                f'  blr\n'
                f'  #_ REGISTER_OUT v3 {vec(d)}\n'
                f'  #_ REGISTER_OUT v4 {vec(a)}\n'
                f'  #_ REGISTER_OUT v5 {vec(b)}\n')
        write(name, tests)

    # Record forms: cr6.lt = all elements true, cr6.eq = none true. The harness
    # can't read CR, so branch on it: r3 bit 0 = all, bit 1 = none.
    for name in RECORD_FORMS:
        model, size = VECTOR_BINOPS[name]
        tests = []
        cases = [(rand_vec(size), rand_vec(size)) for _ in range(4)]
        a = rand_vec(size)
        cases += [(a, a), (pack([0] * (16 // size), size, False), pack([0] * (16 // size), size, False))]
        # all-true for the greater-than forms: a = b + 1 elementwise where possible
        big = pack([(1 << (size * 8 - 1)) - 1] * (16 // size), size, False)
        small = pack([0] * (16 // size), size, False)
        cases += [(big, small), (small, big)]
        for a, b in cases:
            d = model(a, b)
            all_true = all(w == 0xFFFFFFFF for w in d)
            none_true = all(w == 0 for w in d)
            r3 = (1 if all_true else 0) | (2 if none_true else 0)
            tests.append(
                f'  #_ REGISTER_IN v4 {vec(a)}\n'
                f'  #_ REGISTER_IN v5 {vec(b)}\n'
                f'  #_ REGISTER_IN r3 0\n'
                f'  {name}. v3, v4, v5\n'
                f'  bge cr6, .+8\n'
                f'  ori r3, r3, 1\n'
                f'  bne cr6, .+8\n'
                f'  ori r3, r3, 2\n'
                f'  blr\n'
                f'  #_ REGISTER_OUT v3 {vec(d)}\n'
                f'  #_ REGISTER_OUT r3 {r3}\n')
        write(name + '_rc', tests)

    # vspltish: sign-extended 5-bit immediate splatted to every halfword.
    tests = []
    for imm in (-16, -1, 0, 1, 7, 15):
        d = pack([imm & 0xFFFF] * 8, 2, False)
        tests.append(f'  vspltish v3, {imm}\n  blr\n  #_ REGISTER_OUT v3 {vec(d)}\n')
    write('vspltish', tests)

    # Summary-overflow branches after fcmpu: so = unordered (a NaN operand).
    NAN = '0x7FF8000000000000'
    tests = []
    for mnemonic in ('bso', 'bns'):
        for f2, unordered in (('2.0', False), (NAN, True), ('1.0', False)):
            taken = unordered if mnemonic == 'bso' else not unordered
            tests.append(
                f'  #_ REGISTER_IN f1 1.0\n'
                f'  #_ REGISTER_IN f2 {f2}\n'
                f'  #_ REGISTER_IN r3 1\n'
                f'  fcmpu cr1, f1, f2\n'
                f'  {mnemonic} cr1, .+8\n'
                f'  li r3, 2\n'
                f'  blr\n'
                f'  #_ REGISTER_OUT r3 {1 if taken else 2}\n')
    for mnemonic in ('bsolr', 'bnslr'):
        for f2, unordered in (('2.0', False), (NAN, True)):
            returns = unordered if mnemonic == 'bsolr' else not unordered
            tests.append(
                f'  #_ REGISTER_IN f1 1.0\n'
                f'  #_ REGISTER_IN f2 {f2}\n'
                f'  #_ REGISTER_IN r3 1\n'
                f'  fcmpu cr1, f1, f2\n'
                f'  {mnemonic} cr1\n'
                f'  li r3, 2\n'
                f'  blr\n'
                f'  #_ REGISTER_OUT r3 {1 if returns else 2}\n')
    write('bso', tests)

    # cror / crorc: cr7.eq = cr0.lt OR [NOT] cr1.gt (bits 30, 0, 5).
    tests = []
    for mnemonic in ('cror', 'crorc'):
        for x, y in ((-1, 1), (-1, -1), (1, 1), (1, -1)):
            lt0, gt1 = x < 0, y > 0
            result = lt0 or (not gt1 if mnemonic == 'crorc' else gt1)
            tests.append(
                f'  #_ REGISTER_IN r4 0x{x & 0xFFFFFFFFFFFFFFFF:X}\n'
                f'  #_ REGISTER_IN r5 0x{y & 0xFFFFFFFFFFFFFFFF:X}\n'
                f'  #_ REGISTER_IN r3 1\n'
                f'  cmpwi cr0, r4, 0\n'
                f'  cmpwi cr1, r5, 0\n'
                f'  cmpwi cr7, r5, 12345\n'
                f'  {mnemonic} 30, 0, 5\n'
                f'  beq cr7, .+8\n'
                f'  li r3, 2\n'
                f'  blr\n'
                f'  #_ REGISTER_OUT r3 {1 if result else 2}\n')
    write('cror', tests)

    # stfsu: store single, then update rA with the effective address.
    tests = []
    # Values are written as strings with a '.', which is how the harness tells
    # a float input from an integer one.
    for text, disp in (('1.5', 16), ('-2.25', 0), ('3.0e38', -8)):
        value = float(text)
        bits = struct.pack('>f', value)
        ea = 0x1000 + disp
        tests.append(
            f'  #_ REGISTER_IN f1 {text}\n'
            f'  #_ REGISTER_IN r4 0x1000\n'
            f'  stfsu f1, {disp}(r4)\n'
            f'  blr\n'
            f'  #_ REGISTER_OUT r4 0x{ea:X}\n'
            f'  #_ MEMORY_OUT {ea:08X} ' + ' '.join(f'{c:02X}' for c in bits) + '\n')
    write('stfsu', tests)

    print('wrote', sorted(f for f in os.listdir(OUT) if f.startswith('nfsmw_')))


if __name__ == '__main__':
    main()
