#!/usr/bin/env python3
"""The Direct3D inventory's runs (tests/d3d_inventory.sh: work/inv/<window>.txt) summed up for VULKAN.md.

    d3dinv_report.py [inventory dir] [scene ...]     (default work/inv, every scene there)

Prints, per section, each key with its count per frame in each scene's window ('-' not seen in the
window; '*' seen only outside it, at load); the whole run's count where only that matters
(resources made, shaders, declarations)."""
import os
import re
import sys

LINE = re.compile(r'^\s*(\d+)\s+(\d+)\s+([\d.]+)\s{2}(.*)$')


def load(path):
    head, rows = {}, {}
    for l in open(path):
        if l.startswith('#'):
            m = re.search(r'frames (\d+)-(\d+)', l)
            if m:
                head['window'] = (int(m.group(1)), int(m.group(2)))
            m = re.search(r'(\d+) draws in the window; distinct keys a frame: mean ([\d.]+), max (\d+)', l)
            if m:
                head['draws'], head['keys_mean'], head['keys_max'] = int(m.group(1)), float(m.group(2)), int(m.group(3))
            m = re.search(r'distinct pipeline keys: (\d+) in the run, (\d+) in the window', l)
            if m:
                head['keys_run'], head['keys_win'] = int(m.group(1)), int(m.group(2))
            continue
        m = LINE.match(l)
        if m:
            rows[m.group(4)] = (int(m.group(1)), int(m.group(2)), float(m.group(3)))
    return head, rows


def main(a):
    d = a[0] if a else 'work/inv'
    scenes = a[1:] or [s for s in ('front', 'scenes', 'lobby')
                       if os.path.exists(os.path.join(d, s + '.txt'))]
    data = {s: load(os.path.join(d, s + '.txt')) for s in scenes}
    # (animated values, a new one most frames: the bump matrices; summed per stage and state)
    for s in scenes:
        rows = data[s][1]
        for k in [k for k in rows if re.match(r'TSS stage \d+ BUMPENV', k)]:
            m = re.match(r'(TSS stage \d+ \w+)', k)
            g = m.group(1) + ' = (float values)'
            a, b, c = rows.pop(k)
            o = rows.get(g, (0, 0, 0.0))
            rows[g] = (o[0] + a, o[1] + b, o[2] + c)
    keys = sorted(set(k for s in scenes for k in data[s][1]))
    w = max(8, max(len(s) for s in scenes) + 1)
    print('scene windows: ' + ', '.join('%s %d-%d' % (s, *data[s][0]['window']) for s in scenes))
    print('pipeline keys (a draw\'s shaders, vertex layout, fixed states, targets): ' +
          '; '.join('%s %d a frame (max %d), %d in the window, %d in the run' % (
              s, data[s][0]['keys_mean'], data[s][0]['keys_max'], data[s][0]['keys_win'], data[s][0]['keys_run'])
              for s in scenes))
    sections = [('M ', 'calls a frame by method'), ('DRAW ', 'draws a frame'), ('RS ', 'render states set a frame, by value'),
                ('SS ', 'sampler states a frame, by value'), ('TSS ', 'texture stage states a frame, by value'),
                ('SETTEX ', 'textures bound a frame'), ('SETRT ', 'render targets set a frame'), ('SETDS ', 'depth buffers set a frame'),
                ('CLEAR ', 'clears a frame'), ('STRETCHRECT ', 'StretchRect a frame'), ('COPY ', 'copies'),
                ('STREAM', 'streams and indices'), ('INDICES', None), ('SETDECL ', 'declarations bound a frame'), ('FVF ', 'FVFs'),
                ('SETSHADER ', 'shader binds a frame'), ('CONST ', 'constant ranges set a frame'), ('XFORM ', 'fixed-function transforms'),
                ('LOCK ', 'locks a frame'), ('QUERY', 'queries'), ('CHECK', 'capability checks (whole run)'),
                ('CREATE ', 'resources made (whole run)'), ('DRAWPAIR ', 'shader pairs drawn a frame, with fog and what the samplers hold'),
                ('MATERIAL', 'materials set a frame'), ('LIGHT', 'lights set and enabled a frame'),
                ('FFVS ', 'fixed-function vertex state at draws without a vertex shader (or with pre-transformed vertices), a frame'),
                ('FFPS ', 'fixed-function texture stages at draws without a pixel shader, a frame')]
    done = set()
    for pre, title in sections:
        ks = [k for k in keys if k.startswith(pre) and k not in done]
        if not ks:
            continue
        done |= set(ks)
        if title:
            print('\n## ' + title)
            print('%s  %s' % (''.join(s.rjust(w) for s in scenes), 'key'))
        whole = pre in ('CREATE ', 'CHECK')
        for k in ks:
            cells = []
            for s in scenes:
                r = data[s][1].get(k)
                if not r:
                    cells.append('-'.rjust(w))
                elif whole:
                    cells.append(str(r[0]).rjust(w))
                elif r[1] == 0:
                    cells.append('*'.rjust(w))
                else:
                    v = r[2]
                    cells.append(('%.2f' % v if v < 10 else '%.0f' % v).rjust(w))
            print('%s  %s' % (''.join(cells), k))


if __name__ == '__main__':
    main(sys.argv[1:])
