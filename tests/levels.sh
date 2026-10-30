#!/bin/bash
# The level loader's sweep (DESIGN.md decision 12): each level loaded straight from the game's level
# table (ETM_LEVEL) in a hidden --test run and played for N frames from the load point's start (the
# scripted input's mark, which the loader sets), frames saved at +1, +300, +700 and the last, and one
# scripted input from +300 to +500 by the level's kind:
#   on foot             the left stick forward (walk)
#   driving / pilot     A (accelerate / speed up)
#   gunner              the left stick across (aims the gun)
#   cut scene           Escape (skips its film or in-engine scene: the game moves on)
# Both runs skip a story film or in-engine scene at the load point's start first (ETM_LEVEL_SKIP=1,
# but in the cut scenes' own level), so the input meets the game itself. The run without the input
# (the same frames: --test is deterministic) shows whether the input did anything: the loader's
# status line at +600 (level, screen, cinema bars, film, camera position, view hash) differs.
# Writes DIR/table.txt; the frames are DIR/<level>/shot_*.png (tools/levels_sheet.sh DIR: one sheet each).
#   tests/levels.sh [--frames N] [--out DIR] [--renderer dxvk|vulkan] [LEVEL ...]
# LEVEL as --level takes it (a file name, NAME@LEVEL or LEVEL.POINT); default: the list below.
export WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-0} XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
set -e
cd "$(dirname "$0")/.."
frames=1500 out=work/levels extra=() levels=()
while [ $# -gt 0 ]; do
    case "$1" in
    --frames) frames=$2; shift 2 ;;
    --out) out=$2; shift 2 ;;
    --renderer) extra=(--option "renderer=$2"); shift 2 ;;
    -*) echo "unknown argument $1"; exit 2 ;;
    *) levels+=("$1"); shift ;;
    esac
done
[ ${#levels[@]} -gt 0 ] || levels=(
    B1_PO_01_N_EXTERIOR_START       # the first Post Office load point (Niobe)
    B1_PO_02_G_LOBBY                # the Post Office lobby (Ghost; where New Game's route arrives)
    B3_MC_01_G_GROUND_FLOOR_CENTRAL # the chateau (Ghost, beat 3)
    B5_PP_02_N_EXT_LOWER_SHAVEN     # the power plant (Niobe, beat 5)
    B1_DC_01_N_CITY_SECTION_A       # the city drive (Niobe at the wheel)
    B1_DC_01_G_CITY_SECTION_A       # the same drive, Ghost shooting
    B2_DA_01_G_MAIN_AQUEDUCT        # the aqueduct (Ghost driving)
    B7_HC_01_N_HOVERCRAFT_TUNNEL_A  # the Logos (Niobe piloting)
    17.28                           # a cut scene: the airport's opening (Niobe; a film first)
    17.20                           # the cut scene New Game starts with (Ghost; in-engine)
    B7_HC_01_G_HOVERCRAFT_TUNNEL_F  # the last beat's level (Ghost on the guns)
)
mkdir -p "$out"
main=$(git worktree list --porcelain | awk '/^worktree /{print $2; exit}')
mkdir -p "$main/work"
exec 9> "$main/work/ab.lock"
flock 9
etm=$(pwd)/build/etm
# the kinds, from the game's own table
./build/etm --test --user "$out/list_user" --hidden --list-levels "${extra[@]}" > "$out/list.txt" 2>/dev/null || true
run() {    # run <dir> <level> [env assignments...]
    local d=$1 lvl=$2; shift 2
    rm -rf "$d"; mkdir -p "$d/user"
    ( cd "$d" && env ETM_LEVEL="$lvl" ETM_LEVEL_FRAMES="$frames" "$@" timeout -s KILL 600 "$etm" --test --user user --hidden \
        --frames 20000 "${extra[@]}" > log.txt 2>&1; echo "exit $?" >> log.txt )
}
status_at() {    # status_at <log> <+frame>: the loader's status line's fields after the frame number
    grep -o "level: frame [0-9]* (+$2) .*" "$1" | head -1 | sed 's/^level: frame [0-9]* ([^)]*) //'
}
printf '%-32s %-6s %-20s %-6s %-7s %-6s %-7s %-22s %s\n' level entry kind who loaded frames crashed input "input changed (+600)" > "$out/table.txt"
for lvl in "${levels[@]}"; do
    name=$(echo "$lvl" | tr -c 'A-Za-z0-9_.\n' '_')
    d="$out/$name"
    # the table's line for it: the loader's own "starting" line names entry and kind
    run "$d.base" "$lvl" ETM_LEVEL_SKIP=1
    start=$(grep -o "level: starting .*" "$d.base/log.txt" | head -1)
    entry=$(echo "$start" | awk '{print $3}')
    kind=$(echo "$start" | sed -n 's/^[^(]*(\(.*\), \(Ghost\|Niobe\), difficulty.*/\1/p')
    who=$(echo "$start" | sed -n 's/.*, \(Ghost\|Niobe\), difficulty.*/\1/p')
    case "$kind" in
    "driving"|"hovercraft (pilot)") input=(ETM_PAD="+300:A*200") ;;
    *gunner*) input=(ETM_PAD="+300:LX=32000*200") ;;
    "cut scene") input=(ETM_KEYS="+300:ESCAPE") ;;
    *) input=(ETM_PAD="+300:LY=-32000*200") ;;
    esac
    run "$d" "$lvl" "${input[@]}" ETM_LEVEL_SKIP=1 ETM_SHOT="+1,+300,+700,+$frames"
    loaded=no; grep -q "level: .* running from frame" "$d/log.txt" && loaded=yes
    played=$(grep -o "level: [0-9]* frames played" "$d/log.txt" | awk '{print $2}')
    crashed=no; grep -q "FATAL" "$d/log.txt" "$d.base/log.txt" && crashed=yes
    a=$(status_at "$d.base/log.txt" 600); b=$(status_at "$d/log.txt" 600)
    changed=no; [ -n "$a" ] && [ "$a" != "$b" ] && changed=yes
    [ -z "$a" ] && changed=-
    skipped=$(grep -c "level: .* skipped" "$d.base/log.txt" || true)
    printf '%-32s %-6s %-20s %-6s %-7s %-6s %-7s %-22s %s\n' "$lvl" "${entry:--}" "${kind:--}" "${who:--}" "$loaded" "${played:-0}" "$crashed" \
        "${input[0]#ETM_*=}" "$changed (skips $skipped; without: ${a:-none} | with: ${b:-none})" >> "$out/table.txt"
done
cat "$out/table.txt"
