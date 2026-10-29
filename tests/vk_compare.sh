#!/bin/bash
# Pictures from our Vulkan renderer against DXVK's for the same frames (VULKAN.md "The check method"):
# for each shot_<frame>.png in both folders, the pixels that differ at all, those more than 2 % (5 of 255
# levels) off in any channel, the PSNR, and the largest 16x16 block's mean difference in levels; then
# the verdict: 2D frames (TWO_D="frame ...") must be identical; 3D frames at most 0.5 % of pixels more
# than 2 % off, PSNR at least 45 dB, no 16x16 block more than 4 levels off on average.
#   tests/vk_compare.sh DXVK_DIR VULKAN_DIR [frame ...]
a=$1 b=$2; shift 2
frames=("$@")
[ ${#frames[@]} -eq 0 ] && for f in "$a"/shot_*.png; do x=${f##*/shot_}; frames+=("${x%.png}"); done
fail=0
for f in "${frames[@]}"; do
    A=$a/shot_$f.png B=$b/shot_$f.png
    [ -e "$A" ] && [ -e "$B" ] || { echo "$f: missing"; fail=1; continue; }
    total=$(identify -format '%[fx:w*h]' "$A")
    ae=$(compare -metric AE "$A" "$B" null: 2>&1 | cut -d' ' -f1)
    ae2=$(compare -metric AE -fuzz 2% "$A" "$B" null: 2>&1 | cut -d' ' -f1)
    psnr=$(compare -metric PSNR "$A" "$B" null: 2>&1 | cut -d' ' -f1)
    block=$(convert "$A" "$B" -compose difference -composite -filter box -resize 6.25% \
            -format '%[fx:255*max(maxima.r,max(maxima.g,maxima.b))]' info: 2>/dev/null)
    pct=$(awk -v n="$ae2" -v t="$total" 'BEGIN { printf "%.3f", 100 * n / t }')
    two_d=0
    for t in $TWO_D; do [ "$t" = "$f" ] && two_d=1; done
    if [ $two_d = 1 ]; then
        ok=$([ "$ae" = 0 ] && echo ok || echo FAIL)
    else
        ok=$(awk -v p="$pct" -v s="$psnr" -v k="$block" 'BEGIN { print (p <= 0.5 && (s == "inf" || s >= 45) && k <= 4) ? "ok" : "FAIL" }')
    fi
    [ "$ok" = ok ] || fail=1
    printf '%-6s %s  differ %8s  >2%%: %7s (%6s %%)  PSNR %7s  worst 16x16 block %5.1f levels%s\n' "$f" "$ok" "$ae" "$ae2" \
        "$pct" "$psnr" "$block" "$([ $two_d = 1 ] && echo '  (2D: exact)')"
done
exit $fail
