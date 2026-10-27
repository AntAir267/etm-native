"""How a function's float maths rounds in the translation, for rewriting it as C (runtime/decomp/).

The game runs the x87 with control word 0x047f (single precision, rounding DOWN). The translation's
C keeps a function's x87 stack either in C locals (s1, s2...) or in the CPU record (FPUSH/ST()).
In the first kind GCC compiles a single operation on two floats whose result is only stored as a
float, (float)((f80)a + (f80)b), as SSE (addss...), which rounds to NEAREST; in the second kind
nothing is narrowed. A rewrite must round the same: plain (f80) casts for the first kind (GCC then
narrows the same operations), loads through an asm barrier for the second (see quat.c's LX, X).

    fpround.py ADDR [ADDR ...]

Per function: the translation's mode (locals / memory / none), then its count of SSE and x87
arithmetic beside the rewrite's dc_<addr> (from the built objects: make first). Counts differ where
the rewrite loops or shares code the original repeats; the SSE kinds are what must match.
"""
import collections, glob, re, subprocess, sys

X87 = re.compile(r'f(add|sub|mul|div)r?p?[sl]?$')
SSE = re.compile(r'(add|sub|mul|div|sqrt)(ss|sd)$')


def body(obj, sym):
    out = subprocess.run(['objdump', '-d', '--no-show-raw-insn', obj], capture_output=True, text=True).stdout
    m = re.search(r'^[0-9a-f]+ <%s>:\n(.*?)(?=^\n|\Z)' % re.escape(sym), out, re.S | re.M)
    return m.group(1) if m else ''


def count(text):
    c = collections.Counter()
    for line in text.splitlines():
        parts = line.split('\t')
        if len(parts) < 2 or not parts[1].split():
            continue
        mn = parts[1].split()[0]
        m = X87.match(mn)
        if m:
            c['x87_' + m.group(1)] += 1
            continue
        m = SSE.match(mn)
        if m:
            c['sse_' + m.group(1) + m.group(2)[1]] += 1
        elif mn in ('fsqrt', 'fsin', 'fcos', 'fchs'):
            c[mn] += 1
    return dict(sorted(c.items()))


def main():
    gen = glob.glob('gen/code_*.c')
    for a in sys.argv[1:]:
        a = a.lower().zfill(8)
        src = subprocess.run(['grep', '-l', '^void F_%s_orig' % a] + gen, capture_output=True, text=True).stdout.split()
        if not src:
            print(a, 'not in the translation (no hook?)')
            continue
        s = open(src[0]).read()
        i = s.index('void F_%s_orig' % a)
        b = s[i:s.find('\n}\n', i)]
        st = len(re.findall(r'FPUSH\(|ST\(\d\)', b))
        loc = len(re.findall(r'\bs\d+ = ', b))
        mode = 'memory' if st and not loc else 'locals' if loc and not st else 'mixed' if st else 'none'
        t = count(body('build/obj/gen_' + src[0].split('/')[-1].replace('.c', '.o'), 'F_%s_orig' % a))
        d = {}
        for o in glob.glob('build/obj/dc_*.o'):
            text = body(o, 'dc_' + a.lstrip('0'))
            if text:
                d = count(text)
                break
        print('%s %-6s translation %s\n%16s rewrite     %s' % (a, mode, t, '', d))


if __name__ == '__main__':
    main()
