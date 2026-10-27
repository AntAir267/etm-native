# Enter the Matrix native: design notes

A native Linux port of Enter the Matrix (Shiny Entertainment / Atari, PC 2003; the 2004
re-release's program) by static recompilation, the method of larush-native and 1nsane-native
(whose translator and Windows stand-in this started from, copied from larush-native on
2026-10-26). Personal use first; built as "bring your own game": the repository holds only our
tools and runtime, and each user's own files are translated and compiled on their machine.

## Decisions

### 1. Goal (Ant, 2026-10-26)
"Take the repack with the fixes and cut content, recompile it to run natively on Linux with
Vulkan." Faithful by default (the 2004 edition as the repack ships it), extras as options.

### 2. The game: Ant's 2004 repack
The repack's zip (never run), unpacked to
`~/.local/share/etm-native/game` (3.5 GB; the zip's own readme kept as RepackReadme.txt).
- Matrix.exe: linked 2003-11-03, the "1.2" patch's program the 2004 edition ships, no SecuROM;
  the repacker's readme says it fixes the low frame rate with LOD off and adds the 2004
  edition's two extra minigames. Base 0x400000, .text 0x401000-0x8f90d5 (5 MB), full .reloc,
  MSVC with the C runtime linked in [verified: imports, sections].
- Imports: d3d9 (Direct3DCreate9 only), DINPUT8 (DirectInput8Create), mss32 (55 Miles Sound
  System 6.5 functions), DivxMediaLib (16), WINMM timeGetTime, COMCTL32 PropertySheetA, and
  KERNEL32/USER32/GDI32/ADVAPI32.
- Movies: DivX 5 AVIs (640x448, 29.97 fps) with CRI ADX sound in separate .adx files, played
  through DivxMediaLib.dll -> DivxDecoder.dll + FileParser.dll (2003-04, only KERNEL32 imports).
  The game decodes the ADX itself.
- Settings: MatrixConfig.ini beside the program (the repack's says RESOLUTION=1280x720,
  FULLSCREEN=1); MatrixOptions.exe is the launcher that writes it (not used).
- The repack's wrappers are not used: d3d9.dll (an ASI loader), scripts/dxwrapper.asi (D3D9 to
  9Ex, vsync, 30 fps cap: LimitPerFrameFPS = 30), scripts/EnterTheMatrixDVD.WidescreenFix.asi
  (widescreen and HUD aspect: to be redone natively), dsound.dll + dsoal-aldrv.dll (DSOAL, EAX
  over OpenAL), dinput8.dll + Xidi (XInput pads as a DirectInput device with a "Matrix" button
  map), eax.dll, d3d9_Proxy.dll (2002, not imported by Matrix.exe).

### 3. What is translated
Four modules as one program (builds/etm12.py MODULES): Matrix.exe (0x400000), DivxMediaLib.dll
(0x10000000), DivxDecoder.dll and FileParser.dll. The three DLLs all prefer 0x10000000, so two
are moved (0x10020000, 0x10090000): tools/pe.py rebase() applies their base relocations before
discovery, the module table keeps the file's preferred base, and the runtime's loader applies
the same relocations when it maps the file (runtime/main.c load_module).
- First translator run: 9,460 functions (Matrix.exe 8,405; DivxMediaLib 237, DivxDecoder 527,
  FileParser 291), 1.2 M instructions, ~80 s, 2.4 GB.
- 3DNow! code (femms, pfmul, pfadd at 0x8406f8, 0x84103c...) cannot be translated: those
  functions stop with a message. The maths dispatcher 0x823a48 installs them only when the CPU
  check 0x823964 reports 3DNow! (bits 0x20 and 0x80), which the runtime's CPUID never does
  (Pentium III); its SSE set (bit 0x40) is translated [verified: decompile].
- Ghidra cross-check (REA, 2026-10-26): Ghidra has 13,612 functions in Matrix.exe's code, we
  13,289. Of Ghidra's 1,910 we lack, 1,705 aren't covered by our code at all: 1,515 are targets
  of incremental-linking stubs that nothing reachable calls (dead code the linker kept) and the
  only references from live code are the 19 3DNow! routines above [verified: work/disc_cover.py,
  relocation and call scans].
- 3,188 jump tables found by scan without a bound [guess: MSVC 7 switch pattern the bound
  finder misses; to check against Ghidra].

### 4. Not translated: stand-ins
- Direct3D 9: larush-native's guest-side COM thunks (tools/d3d9gen.py) over DXVK 3.1.1 native
  (third_party/dxvk-native, copied from larush-native's build: SDL3 WSI, its soft-shadows patch
  is inert unless enabled).
- Miles: runtime/mss32.c, written for this game from li2-native's stand-in. What the game does
  [verified: Ghidra decompiles 0x792818, 0x79723b, 0x79cf9b, 0x79d7f6, 0x79ec9f, 0x7aa749]:
  44.1 kHz stereo driver; 3D provider by name, EAX 3 > EAX 2 > EAX > "Miles Fast 2D Positional
  Audio" (we offer all four: the game takes EAX 3 as with the repack's DSOAL; EAX settings are
  accepted and not heard yet); sound banks' IMA ADPCM decompressed at load with
  AIL_decompress_ADPCM into WAV images; effects play the PCM straight from memory
  (AIL_set_sample_address, AIL_set_3D_sample_info); streams and the interactive music are whole
  files (AIL_file_read with -1: size, then data) played as IMA ADPCM (type 5/7, block 36/72,
  44.1 kHz) and timed with AIL_sample_ms_position; the car engine is a looping 48 kHz sample
  over a buffer the game keeps writing. So PCM voices read program memory as they play
  (dsound.c voice_new_guest), ADPCM is decoded into a voice of its own at start. Volumes and
  pans are floats (Miles 6.5).
- FileParser's asynchronous reads: ReadFileEx reads at once and queues the completion routine
  for the calling thread; WaitForSingleObjectEx(alertable) runs it and returns
  WAIT_IO_COMPLETION, as Windows orders it (runtime/sync.c, files.c). CancelIo has nothing in
  flight.
- --test runs: the turn-taking scheduler reuses a finished thread's place (the movie player
  starts threads for each movie; it ran out of its 8 places on the third movie).
- --test runs: a Sleep on the main thread moves the virtual clock (Sleep(0) 0.1 ms). The game's
  frame wait reads QueryPerformanceCounter (0x677a90) and calls Sleep(0) (0x7c0b40, through the
  service table 0x97edd0) until the frame's time has passed [verified: ETM_TIME_CALLERS counts,
  4.9 M of each in 300 frames]; with only the 1 us a read it spun ~16,000 times a frame (test
  runs at ~10 fps). Now 300 frames take 1.4 s instead of 29 s with the same virtual times.
  ETM_TIME_CALLERS=1 prints the clock reads and sleeps by caller at exit.
- Miles: a sample played to its end keeps its position at the end: the movie player
  (0x7be3fb) times frames from its sound (0x7bddcb: AIL_sample_ms_position, current == total
  means over, then one frame a draw) [verified: decompile; the 1.5 s transition movie trans1,
  whose sound is 1.0 s, stopped at frame 0 of the sound and the front end stayed black].
- Movie sound (.adx beside each .avi) is not CRI ADX: the game reads it with AIL_file_read and
  plays it as IMA ADPCM stereo (trans1.adx: 49,680 bytes = 44,850 frames at 44.1 kHz).

### 5. Translator changes for this program
- Discovery from relocations only (li2-native's change, applied to this repository from outside
  this session at 18:11 on 2026-10-26 and kept): the aligned-dword scan of data took ASCII text
  and round numbers for code; all 366 functions it dropped were bogus starts (0x402000,
  0x474e45 "ENG", ...) [verified: lists compared].
- Incremental-linking stubs: Matrix.exe reaches most functions through a one-jmp stub
  (0x401000-0x409000). A function that is one jmp now makes its target a function of its own,
  so functions keep their real addresses (as in Ghidra) and hooks go on the real address; the
  stub is a tail jump (tools/discover.py). 9,094 -> 14,345 functions, the same instructions.

### 6. The display (game.c, d3d9.c)
- resolution (desktop by default; original; WxH) [verified: 1920x1080 frames]: hook on the
  settings loader 0x7b85e0 sets 0x947f60/0x947f64 after MatrixConfig.ini. The 3D view is Hor+
  by itself (0x6eaf9a keeps (4/3) / (width / height) for the projection).
- ui (center by default; stretch): the 2D design is 640 x 448 replayed by the blit renderer
  0x7a73db, x times [0x97eda0] = width / 640, y times height / 448. Before each replay the
  command list is laid out for a centred 4:3 area (x scale height / 480); what spans the design
  width (fades, the cutscenes' letterbox bars) spans the screen. The movie picture (0x7be3fb) is
  pillarboxed through d3d9.c's screen-space draw hook (ui2d_layout). The HUD sits in the 4:3
  area for now (corner anchoring needs a play/menu state: to do). Command formats in game.c.
- d3d9.c: larush-native's game-specific changes removed (fog distances from a LA Rush address,
  hemispheric ambient constants, UI squeezing keyed to LA Rush state, shader substitution on by
  default, shadow filter); the calls reach DXVK as the game makes them. ETM_LOG_2D=1 logs every
  screen-space draw with its caller and extent.

### 7. Frame pacing
The game paces itself (0x7c0915, at each frame's end): the frame's time is rounded up to 1/60,
1/30, 1/20, 1/15 or 1/12 s, waited out with QueryPerformanceCounter and Sleep(0), and used as the
next simulation step [verified: decompile]. So it runs at up to 60 a second (the Xbox version's
rate); the PC version's reputation for unsteady frame rates is frames straddling 1/60. The repack
capped it at 30 (dxwrapper LimitPerFrameFPS). Default: the game's own pacing; frame_limit = 30
gives the repack's cap.

### 8. Controllers (pad.c, game.c)
- The game's own joystick support [verified: decompiles 0x7b4b78, 0x7b53c2, 0x7b8470 and the
  binding table read at run time]: DIJOYSTATE2; X/Y move, RotZ strafes, the D-pad works menus;
  buttons 1-12 and the D-pad's directions are copied to +0x250 of the input object (0x e7c018)
  where the binding check reaches them as codes -9 .. -24 (-1 .. -8 the mouse). With a joystick
  present the game fills each binding's second slot with joystick defaults (listed in pad.c).
- The controller's buttons follow the repack's Xidi "Matrix" numbering (Y 1, RT 2, A 3, B 4,
  LT 7, X 8, Back 10, LS 11, RS 12, Start = Escape key, right stick across RotZ), which with
  those defaults is the Xbox version's layout (its manual, archive.org xboxmanual_Enter_the_Matrix).
  LB / RB, which Xidi used for RotZ, get buttons 5 / 6 (switch shooting / hand-to-hand, and the
  right mouse's second action) so every default has a button. In menus (game.c game_front_end,
  the input update's own test) A also presses Enter and B Escape: the game's joystick accept is
  button 2 (RT).

## Progress
- 2026-10-26: project set up; game unpacked; builds/etm12.py; first translator run; Miles
  stand-in, ReadFileEx; first runs: the intro movies (Atari, Warner Bros., Shiny) play through
  the translated DivX decoder and the game reaches its front end (language menu drawn
  correctly, menu background movies) in a hidden --test run (work/testuser).
- 2026-10-26: test runs 20x faster; front end complete: language, title menu (Load, New Game,
  Options, Hacking, Quit), difficulty, the opening cinematic (Escape skips), character select,
  Ghost's jack-in construct scene, the in-engine intro (B1_PO_01_G_EXTERIOR_START), the save
  prompt (Saved\Save written to the user folder), then gameplay in B1_PO_02_G_LOBBY: Ghost,
  HUD, tutorial tips, a guard fighting him; all drawn correctly at 640x480. Scripted route
  (frames, from a fresh test user): 1200 RETURN (English), 1400 DOWN, 1430 RETURN (New Game),
  1700 RETURN (Normal), 2000 ESCAPE (skip the cinematic), 2200 RETURN (Ghost), 3200 RETURN
  (skip the intro scene), 4000 RETURN (save: Yes), 4300/4600 RETURN; in the lobby by ~4700.
- 2026-10-26: resolution and widescreen: desktop size by default, Hor+ 3D, 2D kept 4:3 and
  centred, full-width fades and bars, pillarboxed movies [verified: 1080p frames of the title,
  a cinematic, character select, the in-engine intro and the lobby].
- 2026-10-26: sound checked (a recording of the mix: each intro's sound for its file's length);
  installed to ~/.local/opt/etm-native with an applications-menu entry (Enter the Matrix);
  ~/.local/share/etm-native/options.txt starts at display = fullscreen, resolution = desktop.
- 2026-10-26: controllers: the Xbox layout through the game's own joystick bindings; the whole
  front end and into the lobby with the scripted controller only [verified: frames].
