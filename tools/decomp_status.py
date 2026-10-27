"""Where the drawing code's rewrite stands: each function of builds/decomp_funcs.txt with the
runtime/decomp/ file that registers its rewrite (DECOMP), or as still the translation; a
commented-out registration shows as off.

    decomp_status.py [--todo]          (--todo: only the functions not rewritten, with sizes)
"""
import glob, os, pickle, re, sys

funcs = [int(l.split('#')[0].strip(), 16) for l in open('builds/decomp_funcs.txt')
         if l.split('#')[0].strip()]
done, off = {}, {}
for p in sorted(glob.glob('runtime/decomp/*.c')):
    s = open(p).read()
    for m in re.finditer(r'^(\s*/\*\s*)?DECOMP\(([0-9a-f]+),', s, re.M):
        (off if m.group(1) else done)[int(m.group(2), 16)] = p.split('/')[-1]
sizes = pickle.load(open('work/dis_cache.pkl', 'rb')) if os.path.exists('work/dis_cache.pkl') else {}
size = lambda a: len(sizes.get(a, []))
todo = [a for a in funcs if a not in done]
if '--todo' in sys.argv:
    for a in todo:
        print('%06x %5d%s' % (a, size(a), '  (off: %s)' % off[a] if a in off else ''))
else:
    files = {}
    for a in funcs:
        if a in done:
            files.setdefault(done[a], []).append(a)
    for f in sorted(files):
        print('%-18s %3d' % (f, len(files[f])))
print('%d of %d rewritten (%d of %d instructions), %d to go%s' % (
    len(funcs) - len(todo), len(funcs), sum(size(a) for a in funcs if a in done), sum(size(a) for a in funcs),
    len(todo), ', %d off' % len([a for a in todo if a in off]) if off else ''))
