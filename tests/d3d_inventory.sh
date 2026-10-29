#!/bin/bash
# The Direct3D inventory (runtime/d3d9_inv.c, VULKAN.md 1): the lobby route of tests/ab.sh once,
# hidden, from a fresh test user, counting the program's Direct3D calls (ETM_D3D_INV) over three
# windows: the front end and the film (frames 1-2000), the in-engine scenes (2283-4600), the lobby
# fight (4700-6910); saved as work/inv/<window>.txt (front, scenes, lobby) with the shaders the game
# made (work/inv/shaders/<hash>.vsh.bin / .psh.bin). tools/d3dinv_report.py sums them up.
#   tests/d3d_inventory.sh [-- extra etm arguments]      (e.g. -- --option renderer=vulkan)
cd "$(dirname "$0")/.."
extra=()
[ "$1" = -- ] && { shift; extra=("$@"); }
out=work/inv
u=$out/user
mkdir -p $out
rm -rf "$u"; mkdir -p "$u"
keys="1200:RETURN,1400:DOWN,1430:RETURN,1700:RETURN,2000:ESCAPE,2200:RETURN,2700:RETURN,3200:RETURN,4000:RETURN,4300:RETURN,4600:RETURN"
# one game run at a time on the machine, across worktrees (tests/ab.sh's lock)
main=$(git worktree list --porcelain | awk '/^worktree /{print $2; exit}')
mkdir -p "$main/work"
exec 9> "$main/work/ab.lock"
flock 9
ETM_KEYS="$keys" ETM_D3D_INV="1-2000,2283-4600,4700-6910" ETM_SHOT="1990,4590,6900" \
    timeout -s KILL 1800 ./build/etm --test --user "$u" --hidden --frames 6910 "${extra[@]}" > $out/log.txt 2>&1
for w in front:1-2000 scenes:2283-4600 lobby:4700-6910; do
    n=${w%%:*} r=${w#*:}
    if [ -f "$u/d3d_inventory_$r.txt" ]; then
        mv "$u/d3d_inventory_$r.txt" $out/$n.txt
        echo "$n: $out/$n.txt ($(head -2 $out/$n.txt | tail -1))"
    else
        echo "$n: no inventory (see $out/log.txt)"
    fi
done
rm -rf $out/shaders
[ -d "$u/shaders" ] && mv "$u/shaders" $out/shaders && echo "shaders: $(ls $out/shaders | wc -l) in $out/shaders"
