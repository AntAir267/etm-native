#!/bin/bash
# The image check of our own Vulkan renderer (VULKAN.md 2.10): the same deterministic hidden --test
# frames drawn by DXVK (renderer = dxvk) and by ours (renderer = vulkan), compared per frame with
# tests/vk_compare.sh's limits: 2D frames identical (0 pixels differ), 3D frames at most 0.5 % of
# pixels with a channel more than 5 levels off, PSNR at least 45 dB, no 16x16 block whose mean is more
# than 4 levels off. Two routes from a fresh test user each:
#   - the lobby route (tests/ab.sh's keys): the language menu (1100), the title menu (1380), the
#     opening film (1900), the in-engine scenes (2700 the jack-in construct, 3500 the street), the HUD
#     with a tutorial message (4700), the lobby fight (5600, 6500, 6900);
#   - the Options route: title menu > Options > Video (1790), its last row, Renderer, highlighted (1850);
#     that row's value shows each run's own renderer (DXVK / Vulkan), so its box is painted out in
#     both pictures before they are compared (the originals kept as raw_<frame>.png).
# At 640x480 (the test runs' own size) and 1280x720 (resolution = 1280x720 in the test user's
# options.txt). ETM_VK_VALIDATE=1 is passed to our runs (the Khronos validation layer; its messages
# counted in the verdict). Pictures in work/shots/TAG/<size>_<renderer>/, verdicts in work/shots/TAG.txt.
#   tests/vk_shots.sh [TAG] [SIZE ...]       (TAG vk, sizes 640x480 1280x720 by default)
cd "$(dirname "$0")/.."
tag=${1:-vk}
shift
sizes=("$@")
[ ${#sizes[@]} -eq 0 ] && sizes=(640x480 1280x720)
lobby_keys="1200:RETURN,1400:DOWN,1430:RETURN,1700:RETURN,2000:ESCAPE,2200:RETURN,2700:RETURN,3200:RETURN,4000:RETURN,4300:RETURN,4600:RETURN"
lobby_shots="1100,1380,1900,2700,3500,4700,5600,6500,6900"
opt_keys="1200:RETURN,1400:DOWN,1430:DOWN,1460:RETURN,1600:DOWN,1630:DOWN,1660:RETURN,1800:UP"
opt_shots="1790,1850"
two_d="1100 1380 1900 1790 1850"
out=work/shots/$tag
mkdir -p "$out"
export WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-0} XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
# one game run at a time on the machine, across worktrees (tests/ab.sh's lock; a copy without the
# repository's .git, the T7's, locks its own folder)
main=$(git worktree list --porcelain 2>/dev/null | awk '/^worktree /{print $2; exit}')
main=${main:-$PWD}
mkdir -p "$main/work"
exec 9> "$main/work/ab.lock"
flock 9
run() {    # run <dir> <size> <renderer> <frames> <keys> <shots>
    local u=$1/user
    rm -rf "$u"; mkdir -p "$u"
    [ "$2" != 640x480 ] && printf 'resolution = %s\n' "$2" > "$u/options.txt"
    local v=
    [ "$3" = vulkan ] && v=$ETM_VK_VALIDATE
    ETM_VK_VALIDATE=$v ETM_KEYS="$5" ETM_SHOT="$6" timeout -s KILL 1200 ./build/etm --test --user "$u" --hidden --frames "$4" \
        --option renderer=$3 > "$1/log_$4.txt" 2>&1 || echo "run $1 ($4 frames) failed: $1/log_$4.txt"
    mv "$u"/shot_*.png "$1"/ 2>/dev/null
}
# the Renderer row's value box on the Video page (menu.c: values at x 330 of the 640 x 448 design, the
# fourteenth row at y 64 + 13 x 25; the design is laid out in a centred 4:3 area of the screen's
# height): painted black in both pictures
mask() {    # mask <dir> <size> <frame>
    local w=${2%x*} h=${2#*x} f=$1/shot_$3.png
    [ -e "$f" ] || return
    local s x0 y0 x1 y1
    s=$(awk -v h="$h" 'BEGIN { print h / 448 }')
    x0=$(awk -v w="$w" -v h="$h" -v s="$s" 'BEGIN { printf "%d", (w - 640 * h / 480) / 2 + 320 * h / 480 }')
    x1=$(awk -v w="$w" -v h="$h" 'BEGIN { printf "%d", (w - 640 * h / 480) / 2 + 520 * h / 480 }')
    y0=$(awk -v s="$s" 'BEGIN { printf "%d", 383 * s }')
    y1=$(awk -v s="$s" 'BEGIN { printf "%d", 417 * s }')
    cp "$f" "$1/raw_$3.png"
    convert "$1/raw_$3.png" -fill black -draw "rectangle $x0,$y0 $x1,$y1" "$f"
}
fail=0
{
for s in "${sizes[@]}"; do
    for r in dxvk vulkan; do
        d=$out/${s}_$r
        rm -rf "$d"; mkdir -p "$d"
        run "$d" "$s" $r 6910 "$lobby_keys" "$lobby_shots"
        run "$d" "$s" $r 1860 "$opt_keys" "$opt_shots"
        for f in ${opt_shots//,/ }; do mask "$d" "$s" "$f"; done
    done
    echo "== $s: ours against DXVK"
    grep -h 'Direct3DCreate9\|present parameters' "$out/${s}_vulkan/log_6910.txt" | head -2 | sed 's/^ *[0-9.]* \[0\] /   /'
    nv=$(cat "$out/${s}_vulkan"/log_*.txt | grep -c 'vulkan ERROR\|vulkan warning')
    echo "   validation layer: $([ -n "$ETM_VK_VALIDATE" ] && echo "on, $nv messages" || echo off)"
    [ -n "$ETM_VK_VALIDATE" ] && [ "$nv" != 0 ] && fail=1
    TWO_D="$two_d" tests/vk_compare.sh "$out/${s}_dxvk" "$out/${s}_vulkan" || fail=1
done
echo "verdict: $([ $fail = 0 ] && echo ok || echo FAIL)"
} 2>&1 | tee "$out.txt"
grep -q '^verdict: ok' "$out.txt"
