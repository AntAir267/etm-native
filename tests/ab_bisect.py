#!/usr/bin/env python3
"""Which drawing rewrite makes a level differ from the translation (tests/ab_levels.sh finds the
levels): the level run with all rewrites off, then with one file's rewrites on at a time, the same
scripted input; each run's first differing checkpoint.
  tests/ab_bisect.py [--jobs N] [--out DIR] [--only a.c | addr,addr] [--each a.c,b.c] LEVEL:FRAMES ...
--only tries that group instead of each file; --each those files' functions one at a time."""
import argparse, concurrent.futures as cf, glob, os, re, shutil, subprocess
PAD = '+200:A*300,+500:LY=-32000*300,+800:LX=32000*200,+900:RT=32000*100'
ap = argparse.ArgumentParser()
ap.add_argument('--jobs', type=int, default=8)
ap.add_argument('--out', default='work/ab_bisect')
ap.add_argument('--only')
ap.add_argument('--each')
ap.add_argument('levels', nargs='+')
a = ap.parse_args()
os.environ.setdefault('WAYLAND_DISPLAY', 'wayland-0')
os.environ.setdefault('XDG_RUNTIME_DIR', '/run/user/%d' % os.getuid())
etm = os.path.abspath('build/etm')
reg = {}
for f in sorted(glob.glob('runtime/decomp/*.c')):
    r = re.findall(r'^\s*DECOMP\((\w+),', open(f).read(), re.M)
    if r:
        reg[os.path.basename(f)] = r
every = [x for v in reg.values() for x in v]
if a.each:
    groups = {x: [x] for f in a.each.split(',') for x in reg[f]}
elif a.only:
    groups = {a.only: reg.get(a.only) or a.only.split(',')}
else:
    groups = reg

def run(level, frames, name, off):
    d = '%s/%s/%s' % (a.out, level, re.sub(r'\W', '_', name)[:40])
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d + '/user')
    open(d + '/user/options.txt', 'w').write('resolution = 1280x720\n')
    env = dict(os.environ, ETM_LEVEL=level, ETM_LEVEL_SKIP='1', ETM_LEVEL_FRAMES=frames, ETM_PAD=PAD,
               ETM_DECOMP_OFF=off)
    with open(d + '/log.txt', 'w') as log:
        subprocess.run(['timeout', '-s', 'KILL', '900', etm, '--test', '--hidden', '--user', 'user', '--hash',
                        'hash.txt', '--hash-every', '10'], cwd=d, env=env, stdout=log, stderr=log)
    return {l.split()[0]: l.split()[2:4] for l in open(d + '/hash.txt')} if os.path.exists(d + '/hash.txt') else {}

with cf.ThreadPoolExecutor(a.jobs) as ex:
    for spec in a.levels:
        level, frames = spec.split(':')
        base = run(level, frames, 'off', 'all')
        names = list(groups)
        jobs = [ex.submit(run, level, frames, n, ','.join(x for x in every if x not in set(groups[n]))) for n in names]
        print('%s: %d checkpoints with all off' % (level, len(base)), flush=True)
        for n, j in zip(names, jobs):
            h = j.result()
            first = next((k for k in sorted(base, key=int) if k in h and h[k] != base[k]), None)
            if first:
                print('  %-24s %3d  first differs at %s %s%s' % (n[:24], len(groups[n]), first,
                      'memory ' if h[first][0] != base[first][0] else '',
                      'draws' if h[first][1] != base[first][1] else ''), flush=True)
