#!/bin/bash
# The call tree of one call of a function in a real game, for rewriting (runtime/decomp/): a copy of
# the program with the generated code built with -finstrument-functions (work/calltree/etm,
# tools/calltree.c), run like tests/scene.sh's runs, then named by tools/calltree.py.
#   tools/calltree.sh FN FRAME OUT [larush arguments...]
#   e.g. ETM_KEYS=... tools/calltree.sh 646e8b 6800 work/tree.txt (the test route's keys: DESIGN.md)
# OUT gets the tree (indented: function or D3D method, calls), OUT.pkl the rows for scripts.
set -e
cd "$(dirname "$0")/.."
fn=$1 frame=$2 out=$3; shift 3
O=work/calltree/obj; mkdir -p $O
CF="-std=gnu11 -g -fno-strict-aliasing -frounding-math -fno-omit-frame-pointer -Iruntime -Igen -Ithird_party/sdl3/include -O1 -w -finstrument-functions -finstrument-functions-exclude-file-list=runtime/,/usr/"
ls gen/code_*.c gen/table.c gen/stubs.c | xargs -P 12 -I{} sh -c 'f={}; o='$O'/$(basename $f .c).o; [ $o -nt $f ] || gcc '"$CF"' -DCPU_FAST_MEM -c $f -o $o'
[ $O/d3d9_gen.o -nt gen/d3d9_gen.c ] || gcc $CF -Ithird_party/dxvk-native/include/dxvk -c gen/d3d9_gen.c -o $O/d3d9_gen.o
gcc -O1 -g -c tools/calltree.c -o $O/calltree.o
gcc -o work/calltree/etm $(for f in runtime/*.c; do echo build/obj/rt_$(basename $f .c).o; done) \
    $(for f in runtime/decomp/*.c; do [ -e "$f" ] && echo build/obj/dc_$(basename $f .c).o; done) build/obj/rt_shaders_gen.o $O/*.o \
    -rdynamic -Lthird_party/sdl3/lib -Wl,-rpath,"$PWD/build/lib" -lSDL3 -pthread -lm -ldl -lz
ln -sfn ../../build/lib work/calltree/lib
user=work/testuser
rm -rf $user/Saved
ETM_TRACE_FN=$fn ETM_TRACE_FRAME=$frame ETM_TRACE_OUT=$out.raw \
    timeout -s KILL 900 work/calltree/etm --test --user $user --hidden --frames $((frame + 2)) \
    --option resolution=1280x720 "$@" > work/calltree/run.log 2>&1 || true
python3 tools/calltree.py work/calltree/etm $out.raw $out
