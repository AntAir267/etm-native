#!/bin/bash
# The drawing rewrites against the translation in other levels than tests/ab.sh's lobby (the level
# loader's way in): each level run twice, rewrites off and on, the same scripted input by kind,
# memory and draw-stream hashes every 10 frames compared. A rewrite is only proven where its branches
# ran: the lobby route never drove, and its decoder and arrow paths differed the moment a drive began.
#   tests/ab_levels.sh [--frames N] [--out DIR] [LEVEL...]
export WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-0} XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
frames=1200 out=work/ab_levels levels=()
while [ $# -gt 0 ]; do
    case "$1" in
    --frames) frames=$2; shift 2 ;;
    --out) out=$2; shift 2 ;;
    *) levels+=("$1"); shift ;;
    esac
done
[ ${#levels[@]} -gt 0 ] || levels=(1.0 1.10 2.0 2.1 4.1 5.0 6.0 7.0 8.0 8.1 10.0 10.1 12.9 13.0 14.0 15.0 15.3 17.20 3.0 16.1)
etm=$PWD/build/etm
main=$(git worktree list --porcelain 2>/dev/null | awk '/^worktree /{print $2; exit}')
[ -n "$main" ] && { mkdir -p "$main/work"; exec 9> "$main/work/ab.lock"; flock 9; }
rc=0
for l in "${levels[@]}"; do
    d=$out/$l
    for m in off on; do
        rm -rf "$d/$m"; mkdir -p "$d/$m/user"
        printf 'resolution = 1280x720\n' > "$d/$m/user/options.txt"
        ( cd "$d/$m" && env ETM_LEVEL="$l" ETM_LEVEL_SKIP=1 ETM_LEVEL_FRAMES="$frames" \
            ETM_PAD="+200:A*300,+500:LY=-32000*300,+800:LX=32000*200,+900:RT=32000*100" \
            $( [ $m = off ] && echo ETM_DECOMP_OFF=all ) timeout -s KILL 900 "$etm" --test --hidden --user user \
            --hash hash.txt --hash-every 10 > log.txt 2>&1; echo "exit $?" >> log.txt ) &
    done
    wait
    a=$(tail -1 "$d/off/log.txt"); b=$(tail -1 "$d/on/log.txt")
    first=$(paste -d' ' <(cut -d' ' -f1,3,4 "$d/off/hash.txt") <(cut -d' ' -f3,4 "$d/on/hash.txt") |
            awk '$2!=$4 || $3!=$5 {print $1, ($2!=$4?"memory":""), ($3!=$5?"draws":""); exit}')
    n=$(wc -l < "$d/on/hash.txt")
    grep -q 'FATAL\|Segmentation' "$d/on/log.txt" && first="$first FATAL"
    if [ -z "$first" ] && [ "$a" = "$b" ]; then echo "$l: same ($n hashes, $b)"; else echo "$l: DIFFERS at $first ($n hashes; off $a, on $b)"; rc=1; fi
done
exit $rc
