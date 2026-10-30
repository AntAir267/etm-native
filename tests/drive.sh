#!/bin/bash
# Driving under each drawing setup (the city drive unless a load point is given): the same scripted
# run (accelerate from +300), the camera's speed per update and per second of the virtual clock, and
# frames. The game must drive the same whatever is drawn:
#   base      frame_rate = uncapped at 60 a second (one update a frame), DXVK
#   r165      uncapped at 165, DXVK (frames between updates)
#   r165vk    uncapped at 165, our renderer
#   vk60      uncapped at 60, our renderer
#   paced     frame_rate = 60 (the game's own pacing), DXVK
#   nodecomp  as base, the drawing rewrites off
#   tests/drive.sh [--level NAME] [--out DIR] [--frames N] [--only "a b"]
export WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-0} XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
level=B1_DC_01_N_CITY_SECTION_A out=work/drive frames=1400 only="base r165 r165vk vk60 paced nodecomp"
while [ $# -gt 0 ]; do
    case "$1" in
    --level) level=$2; shift 2 ;;
    --out) out=$2; shift 2 ;;
    --frames) frames=$2; shift 2 ;;
    --only) only=$2; shift 2 ;;
    *) echo "usage: $0 [--level NAME] [--out DIR] [--frames N] [--only \"names\"]"; exit 2 ;;
    esac
done
etm=$PWD/build/etm
run() {   # run <name> <fps> <frame_rate> <renderer> [env...]
    local name=$1 fps=$2 rate=$3 rend=$4; shift 4
    local d=$out/$name
    rm -rf "$d"; mkdir -p "$d/user"
    printf 'frame_rate = %s\nrenderer = %s\nresolution = 1280x720\n' "$rate" "$rend" > "$d/user/options.txt"
    ( cd "$d" && env ETM_LEVEL="$level" ETM_LEVEL_SKIP=1 ETM_LEVEL_FRAMES="$frames" ETM_TEST_FPS="$fps" \
        ETM_PAD="+300:A*$((frames-320))" ETM_SHOT=+700,+701,+702,+703,+1200 \
        ETM_PEEK=97f508:3,97ec4c "$@" timeout -s KILL 900 "$etm" --test --hidden --user user > log.txt 2>&1 )
    echo "$name: exit $? $(grep -c 'not supported' "$d/log.txt") unsupported"
}
for n in $only; do
    case $n in
    base) run base 60 uncapped dxvk ;;
    r165) run r165 165 uncapped dxvk ;;
    r165vk) run r165vk 165 uncapped vulkan ;;
    vk60) run vk60 60 uncapped vulkan ;;
    paced) run paced 60 60 dxvk ;;
    nodecomp) run nodecomp 60 uncapped dxvk ETM_DECOMP_OFF=all ;;
    esac
done
python3 - "$out" $only <<'PY'
import re, sys, math
out = sys.argv[1]
print('%-10s %8s %8s %9s %10s %10s' % ('setup', 'frames', 'updates', 'seconds', 'per update', 'per second'))
for n in sys.argv[2:]:
    rows = []
    for l in open('%s/%s/log.txt' % (out, n), errors='replace'):
        m = re.match(r'\s*([\d.]+) \[\d+\] peek (\d+) (.*)', l)
        if m:
            v = re.findall(r'\(([-\d.e+]+|nan|-?inf)\)', m.group(3))
            h = re.findall(r'=([0-9a-f]{8})', m.group(3))
            if len(v) >= 3 and len(h) >= 4:
                rows.append((float(m.group(1)), int(m.group(2)), float(v[0]), float(v[1]), float(v[2]), int(h[3], 16)))
    # the last 400 updates: full throttle by then
    if not rows:
        print(n, 'no data'); continue
    end = rows[-1][5]
    part = [r for r in rows if r[5] >= end - 400]
    d = sum(math.dist(a[2:5], b[2:5]) for a, b in zip(part, part[1:]))
    du = max(part[-1][5] - part[0][5], 1)
    dt = max(part[-1][0] - part[0][0], 1e-6)
    print('%-10s %8d %8d %9.2f %10.2f %10.1f' % (n, part[-1][1] - part[0][1], du, dt, d / du, d / dt))
PY
