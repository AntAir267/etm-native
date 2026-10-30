#!/bin/bash
# Gameplay at another drawn rate (DESIGN.md 9b): the lobby route (tests/ab.sh's keys) with frames
# drawn between updates (frame_rate = uncapped) at 60 a second (a: one update a frame, the game's
# design; under the test clock the game's own pacing runs 2-3 a frame) and at RATE (b); scripted keys, checkpoints and dumps fall on the same updates (main.c sched()). The
# game's state must match at the same update: memory is compared at update 6900 (in the lobby fight),
# leaving out what is only drawing's and so differs by design, as DRAW_ONLY below (--dump TICK: another
# update, nothing left out; --hashes: the raw memory hashes, which always differ for the same reason).
# b's clock switches to RATE at frame FROM (default 2300, just into the game proper, which starts at
# 2283): the route's menu keys are timed for the default test clock.
# --renderer dxvk|vulkan: both runs with that renderer (VULKAN.md); --serial: one run after the other.
#   tests/rate.sh [--rate N] [--from FRAME] [--frames N] [--every N] [--dump TICK] [--hashes] [--out DIR]
#                 [--renderer dxvk|vulkan] [--serial]
# over ssh (the T7 worker) a display is needed: the desktop session's, as larush-native's tests do
export WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-0} XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
rate=120 from=2300 frames=6910 every=10 out=work/rate dump= renderer= serial=0 hashes=0
# Following the drawn rate rather than the game's state (DESIGN.md 9b): the frame timer and frame-rate
# statistics, frame counters (0x931f5c, 0x97ec48, 0x97ff48, 0x97ff54, 0xe7cca8), the level-of-detail scale
# that follows the frame rate (0x97ecbc), the projection and inverse view the render pass keeps in the view
# and its copies (0x97f2f0-0x97f310, 0x97f4b0-0x97f4f8, 0x151bc20-0x151bc50, 0x151bdf0-0x151be38),
# shader-constant staging (0xe81000), skinned vertices with the blended pose (0x3c80000) [from their
# values]; and written through 0x7c2006, the render pass's flush [verified: ETM_WATCH backtraces,
# 2026-10-30]: the render queue's records (0x86f0000-0x8701000), with a pointer into them (0x153ae04) [guess:
# its cursor], and the cached render state (0x93e884).
DRAW_ONLY="00931f5c-00931f60 0093e884-0093e898 0097ec48-0097ec4c 0097ecbc-0097ecc0 0097f2f0-0097f310
 0097f4b0-0097f4f8 0097ff48-0097ff4c 0097ff54-0097ff58 00e7cca8-00e7ccb0 00e81000-00e82000
 0151bc20-0151bc50 0151bdf0-0151be38 0153ae04-0153ae08 03c80000-03ca0000 086f0000-08701000"
while [ $# -gt 0 ]; do
    case "$1" in
    --rate) rate=$2; shift 2 ;;
    --from) from=$2; shift 2 ;;
    --frames) frames=$2; shift 2 ;;
    --every) every=$2; shift 2 ;;
    --dump) dump=$2; shift 2 ;;
    --hashes) hashes=1; shift ;;
    --out) out=$2; shift 2 ;;
    --renderer) renderer=$2; shift 2 ;;
    --serial) serial=1; shift ;;
    *) echo "usage: $0 [--rate N] [--from FRAME] [--frames N] [--every N] [--dump TICK] [--hashes] [--out DIR] [--renderer R] [--serial]"; exit 2 ;;
    esac
done
auto=0
if [ -z "$dump" ] && [ $hashes = 0 ]; then dump=6900 auto=1; fi
keys="1200:RETURN,1400:DOWN,1430:RETURN,1700:RETURN,2000:ESCAPE,2200:RETURN,2700:RETURN,3200:RETURN,4000:RETURN,4300:RETURN,4600:RETURN"
main=$(git worktree list --porcelain | awk '/^worktree /{print $2; exit}')
mkdir -p "$out" "$main/work"
exec 9> "$main/work/ab.lock"
flock 9
run() {    # run <a|b> <ETM_TEST_FPS>
    local u="$out/user_$1"
    rm -rf "$u"; mkdir -p "$u"
    echo "frame_rate=uncapped" > "$u/options.txt"
    local d=()
    [ -n "$dump" ] && d=(--dump-memory "$dump" "$out/mem_$1.bin")
    [ -n "$renderer" ] && d+=(--option "renderer=$renderer")
    ETM_KEYS="$keys" ETM_TEST_FPS="$2" timeout -s KILL 1200 ./build/etm --test --user "$u" --hidden \
        --frames "$frames" --hash "$out/hash_$1.txt" --hash-every "$every" "${d[@]}" > "$out/log_$1.txt" 2>&1 || true
}
if [ $serial = 1 ]; then
    run a 60
    run b "$rate@$from"
else
    run a 60 &
    pa=$!
    run b "$rate@$from" &
    pb=$!
    wait $pa $pb
fi
if [ $auto = 1 ]; then
    skips=()
    for r in $DRAW_ONLY; do skips+=(--skip "$r"); done
    d=$(.venv/bin/python tools/memdiff.py "$out/mem_a.bin" "$out/mem_b.bin" --max 1000 "${skips[@]}" | grep -v '^[0-9]* differing\|^011f')
    if [ -z "$d" ]; then
        echo "same at update $dump, apart from what is only drawing's"
        exit 0
    fi
    echo "differ at update $dump ($(echo "$d" | wc -l) runs):"
    echo "$d" | head -30
    exit 1
fi
if [ -n "$dump" ]; then
    .venv/bin/python tools/memdiff.py "$out/mem_a.bin" "$out/mem_b.bin" --max 60
    exit 0
fi
d=$(diff <(cut -d' ' -f1,3 "$out/hash_a.txt") <(cut -d' ' -f1,3 "$out/hash_b.txt") | grep '^[<>]' | head -2)
if [ -z "$d" ]; then
    echo "same ($(wc -l < "$out/hash_a.txt") checkpoints)"
else
    echo "differ: $(echo "$d" | head -1 | cut -c3-) (60) vs $(echo "$d" | tail -1 | cut -c3-) ($rate)"
    exit 1
fi
