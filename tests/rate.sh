#!/bin/bash
# Gameplay at another drawn rate (DESIGN.md 9b): the lobby route (tests/ab.sh's keys) with frames
# drawn between updates (frame_rate = uncapped) at 60 a second (a: one update a frame, the game's
# design; under the test clock the game's own pacing runs 2-3 a frame) and at RATE (b); scripted keys, checkpoints and dumps fall on the same updates (main.c sched()). The
# game's state must match at each checkpoint: compares the memory hashes, or with --dump TICK the
# memory at that update (tools/memdiff.py), which shows what differs.
# b's clock switches to RATE at frame FROM (default 2300, just into the game proper, which starts at
# 2283): the route's menu keys are timed for the default test clock.
# --renderer dxvk|vulkan: both runs with that renderer (VULKAN.md); --serial: one run after the other.
#   tests/rate.sh [--rate N] [--from FRAME] [--frames N] [--every N] [--dump TICK] [--out DIR]
#                 [--renderer dxvk|vulkan] [--serial]
rate=120 from=2300 frames=6910 every=10 out=work/rate dump= renderer= serial=0
while [ $# -gt 0 ]; do
    case "$1" in
    --rate) rate=$2; shift 2 ;;
    --from) from=$2; shift 2 ;;
    --frames) frames=$2; shift 2 ;;
    --every) every=$2; shift 2 ;;
    --dump) dump=$2; shift 2 ;;
    --out) out=$2; shift 2 ;;
    --renderer) renderer=$2; shift 2 ;;
    --serial) serial=1; shift ;;
    *) echo "usage: $0 [--rate N] [--from FRAME] [--frames N] [--every N] [--dump TICK] [--out DIR] [--renderer R] [--serial]"; exit 2 ;;
    esac
done
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
