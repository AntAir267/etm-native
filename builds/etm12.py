"""Per-build knowledge for Enter the Matrix PC, the 2004 re-release's program (Matrix.exe "1.2",
linked 2003-11-03, no SecuROM) as in the 2004 repack.

Addresses only: no code or data from the game lives in this repository.
"""
NAME = 'etm-1.2'

# The modules translated together, the program first: (name as imported, file, sha256[, base]).
# The three DivX movie DLLs ship with the game and all prefer 0x10000000: two are moved (their
# relocations applied, as Windows' loader would).
MODULES = [
    ('Matrix.exe', 'Matrix.exe', 'c0f02924a35fd8e409b88a22e74bc6405437a770979a72e2b2b818041730e55f'),
    ('DivxMediaLib.dll', 'DivxMediaLib.dll', 'db203e9034d919ff4fd2691c2c89e4f1c45b691eb412c1a5ab689a6610242abc'),
    ('DivxDecoder.dll', 'DivxDecoder.dll', '0d0f53a811073da35cdfe8d02504533e624d03a1c492dd52f226c228335ee9b5', 0x10020000),
    ('FileParser.dll', 'FileParser.dll', '7fe13f97511cda65c9d70db2f96d56c80ea5b3390a0ed24fa23aa92eb6375c23', 0x10090000),
]

EXTRA_ENTRIES = []
NOT_ENTRIES = []
JUMP_TABLES = {}
NONLOCAL_JUMPS = []
NORETURN_IMPORTS = ['ExitProcess', 'ExitThread', 'TerminateProcess', 'FatalExit', 'FatalAppExitA']

MEM_OVERRIDES = {}

# Functions the runtime may hook (hook_<name>, run at the function's entry; returns 1 if it did
# the call itself).
HOOKS = {
    # the settings loader (MatrixConfig.ini, [RENDERING] RESOLUTION 0-6 -> width, height at
    # 0x947f60, 0x947f64; anything else 640x480): game.c sets the size option resolution asks for
    0x7b85e0: 'settings_load',
    # the 2D renderer (the blit command list at [0x92ed64], [0x979bcc] bytes, replayed once a
    # frame: menus, text, HUD): game.c lays the list out for the screen's shape first
    0x7a73db: 'blit_draw',
    # the projection matrix made (m, width, height, focal, far, aspect factor) and handed to the
    # device (0x7a2df4): game.c zooms in-engine scenes on wide screens
    0x62a236: 'projection',
    # stack leftovers (runtime/decomp/stackfix.c): frames cleared on entry in both modes, for the
    # bytes the original copies into game memory without writing them first
    0x64a2e9: 'sf_64a2e9',
    # the frame timer (0xe7cca8): its reading (seconds since the frame began) and its advance by
    # a step (in cut scenes, keeping the remainder); the frame end (0x7c0915) paces frames by them.
    # frame.c: with frames drawn between updates the frame end neither waits nor advances it
    0x677a90: 'timer_elapsed',
    0x7c1b30: 'timer_advance',
    # the input poll (the main loop's, before each frame: devices read, current -> previous):
    # frame.c skips it before a frame drawn between updates that runs none
    0x7b5333: 'input_poll',
}

# the drawing code's functions, each hookable for its rewrite (runtime/decomp/, decomp.h DECOMP)
import os as _os
for _l in open(_os.path.join(_os.path.dirname(__file__), 'decomp_funcs.txt')):
    _l = _l.split('#')[0].strip()
    if _l:
        HOOKS.setdefault(int(_l, 16), 'dc_%x' % int(_l, 16))

CONSTS = {}
