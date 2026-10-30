#!/bin/bash
# tools/levels_sheet.sh [DIR]: one contact sheet per level of a tests/levels.sh sweep (its four frames
# side by side at 320x240, labelled with the frame), DIR/<level>.sheet.png, for looking at the sweep.
cd "$(dirname "$0")/.."
dir=${1:-work/levels}
for d in "$dir"/*/user; do
    lvl=$(basename "$(dirname "$d")")
    case "$lvl" in *.base|list_user) continue ;; esac
    shots=$(ls "$d"/shot_*.png 2>/dev/null | sort -t_ -k2 -n)
    [ -n "$shots" ] || continue
    montage $shots -geometry 320x240+2+2 -tile x1 -label '%t' -pointsize 12 "$dir/$lvl.sheet.png"
done
ls "$dir"/*.sheet.png
