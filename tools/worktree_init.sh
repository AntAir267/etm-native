#!/bin/bash
# Make a git worktree of this repository ready to build and test without starting over: the main
# checkout's generated code (gen/) and objects (build/) copied and marked current, its built
# libraries (third_party/sdl3, dxvk-native) and Python (.venv) linked, the A/B check's saved
# translation run (work/ab/hash_a.txt) copied. Run inside the worktree.
set -e
top=$(git rev-parse --show-toplevel)
main=$(git worktree list --porcelain | awk '/^worktree /{print $2; exit}')
cd "$top"
[ "$top" = "$main" ] && { echo "this is the main checkout"; exit 0; }
rm -rf gen build
cp -a "$main/gen" gen
mkdir -p build
cp -a "$main/build/obj" "$main/build/lib" "$main/build/etm" build/
for d in sdl3 dxvk-native; do ln -sfn "$main/third_party/$d" "third_party/$d"; done
ln -sfn "$main/.venv" .venv
mkdir -p work/ab work/decomp work/logs
[ -f "$main/work/ab/hash_a.txt" ] && cp "$main/work/ab/hash_a.txt" "$main/work/ab/hash_a.id" work/ab/
[ -f "$main/work/dis_cache.pkl" ] && cp "$main/work/dis_cache.pkl" work/
find gen -type f -exec touch {} +
sleep 1
find build -type f -exec touch {} +
[ -f work/dis_cache.pkl ] && touch work/dis_cache.pkl
echo "ready: $top (make JOBS=4)"
