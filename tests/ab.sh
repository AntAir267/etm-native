#!/bin/bash
# The drawing code's rewrites (runtime/decomp/) against the translation, in the real game: the test
# route (DESIGN.md: fresh test user, English, New Game, Normal, Ghost, the save prompt, the Post
# Office lobby) runs twice, rewrites off and on, hidden, each writing its memory hash and the
# draw-stream hash every few frames (--hash), and the two are compared. Prints "same" or the first
# frame that differs.
#   tests/ab.sh [--frames N] [--every N] [--out DIR] [--route NAME] [--fresh] [--renderer dxvk|vulkan]
#               [--serial] [-- extra etm arguments]
# --renderer: both runs with that renderer (VULKAN.md: the hashes do not depend on it; vulkan's runs
# are kept apart, in DIR_vulkan, so each renderer's translation run stays saved); --serial: the two
# runs one after the other instead of side by side (one game at a time on a machine in use).
# The translation's run (rewrites off) depends only on the translation and the runtime outside
# runtime/decomp/: it is saved (work/ab/hash_a.txt with its build id) and reused while the same
# translation is built; --fresh runs it again.
# Routes: lobby (default: menus, the cinematic skipped, the jack-in and intro scenes, the lobby fight)
set -e
cd "$(dirname "$0")/.."
frames=6910 every=10 out=work/ab route=lobby extra=() fresh=0 renderer= serial=0
while [ $# -gt 0 ]; do
    case "$1" in
    --frames) frames=$2; shift 2 ;;
    --every) every=$2; shift 2 ;;
    --out) out=$2; shift 2 ;;
    --route) route=$2; shift 2 ;;
    --fresh) fresh=1; shift ;;
    --renderer) renderer=$2; shift 2 ;;
    --serial) serial=1; shift ;;
    --) shift; extra=("$@"); break ;;
    *) echo "unknown argument $1"; exit 2 ;;
    esac
done
case "$route" in
lobby) keys="1200:RETURN,1400:DOWN,1430:RETURN,1700:RETURN,2000:ESCAPE,2200:RETURN,2700:RETURN,3200:RETURN,4000:RETURN,4300:RETURN,4600:RETURN" ;;
*) echo "unknown route $route"; exit 2 ;;
esac
if [ -n "$renderer" ]; then
    extra=(--option "renderer=$renderer" "${extra[@]}")
    [ "$renderer" != dxvk ] && out=${out}_$renderer
fi
mkdir -p "$out"
# one check at a time on the machine, across worktrees (helpers share the main checkout's lock)
main=$(git worktree list --porcelain | awk '/^worktree /{print $2; exit}')
mkdir -p "$main/work"
exec 9> "$main/work/ab.lock"
flock 9
run() {    # run <a|b> <ETM_DECOMP_OFF value or empty>
    local u="$out/user_$1"
    rm -rf "$u"; mkdir -p "$u"
    ETM_KEYS="$keys" ETM_DECOMP_OFF="${2:-$ETM_DECOMP_OFF}" timeout -s KILL 900 ./build/etm --test --user "$u" --hidden \
        --frames "$frames" --hash "$out/hash_$1.txt" --hash-every "$every" "${extra[@]}" > "$out/log_$1.txt" 2>&1 || true
}
# the translation's identity: the generated code, the runtime outside runtime/decomp/ and the
# stack fixes (on in both modes)
id=$( (cat gen/funcs.h; cat runtime/*.c runtime/*.h runtime/decomp/stackfix.c; echo "$route $frames $every ${extra[*]}") | sha1sum | cut -c1-16)
if [ $fresh = 0 ] && [ -s "$out/hash_a.txt" ] && [ "$(cat "$out/hash_a.id" 2>/dev/null)" = "$id" ]; then
    run b ""
else
    if [ $serial = 1 ]; then
        run a all
        run b ""
    else
        run a all &
        pa=$!
        run b "" &
        pb=$!
        wait $pa $pb
    fi
    echo "$id" > "$out/hash_a.id"
fi
if ! [ -s "$out/hash_a.txt" ] || ! [ -s "$out/hash_b.txt" ]; then
    echo "no hashes (see $out/log_a.txt, $out/log_b.txt)"; exit 1
fi
d=$(diff "$out/hash_a.txt" "$out/hash_b.txt" | grep '^[<>]' | head -2)
na=$(wc -l < "$out/hash_a.txt"); nb=$(wc -l < "$out/hash_b.txt")
if [ -z "$d" ] && [ "$na" = "$nb" ]; then
    echo "same ($na hashes to frame $(tail -1 "$out/hash_a.txt" | cut -d' ' -f1))"
else
    echo "differ: $(echo "$d" | head -1 | cut -c3-) (translation) vs $(echo "$d" | tail -1 | cut -c3-) (rewrites); $na / $nb hashes"
    exit 1
fi
