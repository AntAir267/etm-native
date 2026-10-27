# The brief for a helper rewriting a batch

Each batch of builds/decomp_funcs.txt goes to one helper working in its own git worktree, with
this brief (the parts in angle brackets filled in). The coordinator merges each finished branch
into master one at a time, runs tests/ab.sh after each merge, and folds the reports into DESIGN.md
and HOWTO.md.

---

You are rewriting part of a recompiled 2003 PC game's drawing code as readable C. The project is
Enter the Matrix recompiled to native code: this repository (you are in a git
worktree of it; the main checkout is the repository itself). The game's x86 code was statically
translated to C (gen/, generated, gitignored); the runtime is runtime/*.c. We replace translated
functions with hand-written readable C (runtime/decomp/) that behaves identically, verified by
A/B runs comparing memory and every Direct3D call frame by frame. The rewrite is the base for
frame-rate interpolation (blending what moves between the game's fixed 60 Hz updates) and later
graphics work, so knowing what each function draws and which state it reads matters: name it.

FIRST: run `tools/worktree_init.sh` in your worktree (copies the main checkout's generated code,
build and the A/B baseline). Build with `make JOBS=4`.

READ before starting: runtime/decomp/HOWTO.md (all of it), runtime/decomp/renderpass.c (the worked
example), runtime/decomp/decomp.h, runtime/decomp/stackfix.c, and DESIGN.md's section "The drawing
code's rewrite".

YOUR TASK: rewrite these <N> functions (<what>, ~<instructions> instructions; largest <...>):
<addresses>
<What is known about them.> Find out what they are and name the file(s)
runtime/decomp/<what it is>.c. Every function registered with DECOMP. Calls to functions outside
your list stay game_call / game_callm to the translated function (the stub's target).

RULES:
- Identical behaviour is the bar: `tests/ab.sh` must print `same` with your rewrites in. Work
  incrementally: a few functions at a time, build, check, commit on your branch. Bisect with
  ETM_DECOMP_OFF. Each check runs the game twice at most (hidden, --test): never more than one
  check at a time, and don't run the game for anything else.
- Readable: named #defines for globals/fields with comments, a header comment per function in game
  terms, the repository's comment style. Don't guess names you can't support; keep the address in
  the name then. New claims are marked [verified] with the evidence, or [guess].
- Do not edit shared files except: your new file(s); runtime/decomp/stackfix.c and builds/etm12.py
  for a stack-leftover fix (regeneration happens in `make`, ~3 minutes); decomp.h only for a
  missing generic helper (say so in your report).
- If a function is impractical, leave it to the translation and say why. Aim for all.
- The user may be playing on this machine: never kill processes by name; stop only a process you
  started (by its PID). No desktop screenshots. Keep builds at JOBS=4.
- Commit on your branch with messages ending with the line
  "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>". Don't push, don't merge.

REPORT (your final message): the branch name and its last commit; the functions rewritten (file by
file, what each does in a line) and any left with the reason; the tests/ab.sh result; stack fixes
added; what each function reads that moves between updates (object positions and matrices,
skeleton poses, the camera, particles: where they live), for the interpolation; branches the route
doesn't reach; traps worth adding to HOWTO.md.
