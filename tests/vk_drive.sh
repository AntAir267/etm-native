#!/bin/bash
# Our Vulkan renderer against DXVK in the driving and hovercraft levels (tests/vk_shots.sh's route never
# drives): the same deterministic hidden run through each, the outside view from +60 (Switch View),
# throttle and steering, frames compared with tests/vk_compare.sh's limits.
#   tests/vk_drive.sh [--out DIR] [LEVEL ...]
export WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-0} XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
out=work/vk_drive levels=()
while [ $# -gt 0 ]; do
    case "$1" in
    --out) out=$2; shift 2 ;;
    *) levels+=("$1"); shift ;;
    esac
done
[ ${#levels[@]} -gt 0 ] || levels=(2.0 2.1 5.0 8.0 8.1 10.0 10.1 12.9 13.0 14.0)
etm=$PWD/build/etm
shots=+30,+90,+150,+210,+270,+330,+390,+450,+510,+570,+630,+690
for l in "${levels[@]}"; do
    for r in dxvk vulkan; do
        d=$out/$l/$r
        rm -rf "$d"; mkdir -p "$d/user"
        printf 'renderer = %s\nresolution = 1280x720\n' "$r" > "$d/user/options.txt"
        ( cd "$d" && env ETM_LEVEL="$l" ETM_LEVEL_SKIP=1 ETM_LEVEL_FRAMES=720 ETM_KEYS=+60:LCTRL \
            ETM_PAD="+100:A*600,+300:LX=-20000*60,+450:LX=20000*60" ETM_SHOT=$shots \
            timeout -s KILL 600 "$etm" --test --hidden --user user > log.txt 2>&1; echo "exit $?" >> log.txt ) &
    done
    wait
    echo "== $l: $(tail -1 "$out/$l/dxvk/log.txt"), $(tail -1 "$out/$l/vulkan/log.txt")"
    tests/vk_compare.sh "$out/$l/dxvk/user" "$out/$l/vulkan/user"
    grep -h 'not supported\|VUID\|FATAL' "$out/$l/vulkan/log.txt" | cut -c1-160 | sort | uniq -c | sort -rn | head -5
done
