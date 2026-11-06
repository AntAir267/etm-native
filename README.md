# Enter the Matrix: Recompiled

Enter the Matrix (Shiny Entertainment, 2003), the 2004 edition's Windows program, translated
from x86 machine code into C and rebuilt as a native Linux game: Vulkan graphics (DXVK by
default, or the project's own Vulkan renderer), SDL3 input and sound, controller support, and new
settings inside the game's own Options menu.

> **Generally untested.** It has been played through the first levels only. It has **not been
> played start to finish**, and the driving and hovercraft levels are untested apart from short
> scripted test runs. Expect bugs, some of them serious. Reports are very welcome.

**None of the game is in this repository.** You need your own copy of the game. The translation
of its program is made on your own computer from your copy, and the build refuses files it
doesn't recognise.

## What it adds

Everything below is an option in the game's own Options pages, and the original behaviour is
always one setting away.

- Widescreen at any resolution, with the HUD in the screen's corners; the in-engine cut scenes and
  the films fill a wide screen.
- Frame rate: the game's own 60, or uncapped with what moves blended between the game's updates
  (on foot; driving and hovercraft stay at the game's own 30).
- Every controller works, plugged in at any time, alongside the keyboard; rumble; button prompts
  for the controller in tips and on the Controls pages; the A button skips cut scenes.
- Anti-aliasing, anisotropic filtering, vsync and a frame limit.
- A longer draw distance in the driving levels.
- Level Select under Load on the title menu: any level from any of its load points.
- The legal notices can be skipped.

## What you need

- The game files of the 2004 edition of Enter the Matrix, whose program is version 1.2
  (Matrix.exe, without copy protection). The build checks four files by SHA-256 (builds/etm12.py):
  Matrix.exe and the three DivX movie libraries that ship with the game (DivxMediaLib.dll,
  DivxDecoder.dll, FileParser.dll). Other versions are untested and will be refused.
- Linux on x86-64, a graphics driver with Vulkan 1.3, gcc, GNU make and Python 3 with Capstone.
- DXVK 3.1.1 built natively with SDL3, and SDL3 itself: third_party/README.md says how.

## Building

1. Put the game's files in a folder (by default ~/.local/share/etm-native/game).
2. Set up Python: `python3 -m venv .venv && .venv/bin/pip install capstone`.
3. Build DXVK and SDL3 into third_party/ as third_party/README.md describes.
4. `make GAME=/path/to/the/game` translates the program into gen/ (a few minutes) and builds
   build/etm. `make install` puts it in ~/.local/opt/etm-native with an applications-menu entry.

Settings and saves go to ~/.local/share/etm-native; each run writes last-run.log there.

DESIGN.md is the project's full record: how the translation and the runtime work, every decision
and the checks. VULKAN.md covers the project's own renderer.

## Helping

CONTRIBUTING.md has the rules; the main one is no game files, ever. The most useful thing right
now is playing it and reporting what goes wrong, with your last-run.log.

## Licence

The Unlicense: public domain (LICENSE). Libraries used: DXVK and SDL3 (zlib licence, not
included), dr_flac by David Reid (public domain or MIT-0, included in third_party/dr_libs).
Enter the Matrix and its files belong to their owners and are not part of this project.
