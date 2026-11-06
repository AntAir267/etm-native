# Contributing

Thanks for helping. Bug reports, testing on other hardware and pull requests are all welcome.

## Rules

1. **No game files, ever.** Nothing from the game in the repository, an issue or a pull request:
   not Matrix.exe, the DivX libraries or any other file of the game, nothing extracted from them
   (textures, sounds, text, data tables, shaders), and no generated C (gen/ is the translation of
   your own copy and stays on your computer). Hand-written code is fine, including code that
   rewrites one of the game's functions by hand. Never link to copies of the game.
2. **The checks must pass.** A change keeps the game's behaviour exact. tests/ab.sh (the
   hand-written rewrites against the translation over the scripted lobby route) and
   tests/ab_levels.sh (the same across 20 levels) must say "same"; tests/rate.sh (the game at 60
   and at 120 frames a second) must say "same"; a renderer change also passes tests/vk_shots.sh.
   DESIGN.md says how each works. Optimisations never change what the game does, its timing
   included.
3. **Settings live in the game's own menus.** Anything a player might want to switch goes in the
   game's Options pages (runtime/menu.c), drawn like the game's own, never only in a file or an
   environment variable. The original behaviour must stay reachable, and gameplay never changes
   because of a graphics setting.
4. **Every controller works.** Any controller, plugged in at any time, alongside the keyboard;
   never only the first one found.
5. Enhancements are optional and can be turned off. They come from rendering, not from upscaled
   or replaced game art.

## Pull requests

Keep each one to a single change, and say what it fixes and how you checked it. The maintainer
reads every change in full before building it.

## Reporting bugs

Use the bug report form. Include your system (Linux distribution, graphics card and driver,
controller), what you were doing and where in the game, and last-run.log from
~/.local/share/etm-native.
