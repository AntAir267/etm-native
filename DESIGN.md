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
  pillarboxed through d3d9.c's screen-space draw hook (ui2d_layout). Command formats in game.c.
- hud (corners by default; center): in play (game_front_end() false) the blit items are grouped
  (a text line is many glyph items: same row, each starting near the last one's end) and each
  group is anchored to the left edge, the centre or the right edge by its extent, so the HUD
  keeps the 4:3 layout's distance from the screen's edges. Menus, the pause menu included, stay
  centred (their left-aligned labels would split otherwise); the dimmed HUD behind the pause menu
  moves into the 4:3 area while paused.
- movies (fill by default; fit): the story's cinematics (movies\fmv\*.avi, films letterboxed
  about 2.5:1 inside the 640 x 448 picture [verified: frame dump]) are drawn across the screen's
  width, their own bars covering the top and bottom; the other movies (logos, menu backdrops,
  character select, with 4:3 menus laid over them) stay pillarboxed. Which movie plays comes from
  the last .avi opened (files.c file_open_notify).
- cutscenes (fill by default; bars): in cinema mode [0x97f238] is the bars' height in pixels (an
  eighth of the screen), the 3D is drawn into the band between (viewport, and the projection
  0x62a236 gets the band's height) and 0x6023cf draws 58-high 2D bars [verified: decompiles,
  values at run time]. The band on 4:3 is about 16:9, the scenes' framing. On a wide screen the
  viewport becomes the screen (d3d9.c SetViewport -> ui_viewport), the projection gets the
  screen's height and a focal length times s = min(screen / band, (w / h) / (4/3)), and the 2D
  bars shrink to (1 - s x band / screen) / 2: none at 16:9, slim at 16:10 [verified: frames at
  1920x1080 and 2560x1600].
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

### 7b. Why the frame rate stays at 60 (uncapping)
The simulation is a fixed 60 Hz step: each drawn frame runs the updates due (0x6469f1 counts them
from the measured rate with a fractional carry; 0x646e8b loops over them) and draws once, on the
last update [verified: decompiles]. Nothing is blended between updates, and with 0 updates due
nothing is drawn; above 60 drawn frames each update would show twice, and stepping faster would
speed the whole game up (cutscene frame counters, timers count updates). Drawing above 60 needs
blending between updates for everything that moves (larush-native's render_rate, interp_game.c):
a project of its own, not started.

### 7c. Sound: EAX reverb
The game picks EAX 3 (its provider attribute check "EAX3 room LF") and, on area changes, hands
"EAX all parameters" the address of a pointer to one of its EAX 3 listener presets (0x9437b8
Generic, 0x943a58, ...: environment, size, diffusion, room, room HF/LF, decay time, HF/LF
ratios, reflections and delay, reverb and delay, ...) [verified: values at run time]; every 3D
sample gets "EAX sample reverb mix" -1 (by distance); 2D samples (music, menus) get none.
dsound.c has a reverb send bus: Freeverb's network (8 combs, 4 allpasses a channel) with the
combs' feedback from the decay time, damping from the HF ratio, pre-delay reflections + reverb
delay, level from room + reverb (mB) [verified: offline impulse response follows the decay time
at low and mid frequencies]. 3D samples send their mixed signal (send 1); AIL_set_sample_reverb_
levels sets a 2D sample's send.

### 7d. The frame wait sleeps
The frame end's wait (0x7c0915) spun on QueryPerformanceCounter + Sleep(0); that Sleep(0) (call
site 0x7c0b4a) now sleeps 0.2 ms a turn (sync.c sleep0_spin_site): 40 s of intros and menus
take 3.7 s of CPU instead of a core at full load.

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

### 9. The drawing code's rewrite (Ant, 2026-10-26: "why don't we do a full drawing code rewrite here?")
For frame-rate interpolation (7b) and later graphics work, the drawing code is rewritten as
readable C (runtime/decomp/), as larush-native did, instead of replaying recorded Direct3D streams
(considered first: generic, but it has to match draws between frames by guesswork and leaves
anything drawn from rewritten buffers at 60).
- Where the drawing is [verified: tools/calltree.sh 6465c8 6800 on the lobby route]: each frame
  (0x6465c8 -> 0x646e8b) first draws, per view, the state the last updates left: 0x747b66 (the
  world: 449 draws in the lobby frame), 0x6f309c -> 0x6f2732 -> 0x7b0aa8 -> 0x7b1838 -> 0x7b1808
  (objects and characters: 941 draws), 0x6ea603 (68), 0x646c33, 0x646cec, 0x646c9e; then runs the
  updates due, the last one also preparing the next view (frustum 0x62bf7a, projection 0x62a236,
  0x649f80...). That render pass is 115 functions, ~24,000 instructions in the lobby frame
  (builds/decomp_funcs.txt); other situations (driving, hovercraft, cutscenes, Focus, sniper) will
  add more.
- Tooling (from larush-native, ported): decomp.h (DECOMP registry, game_call family, dc_frame,
  decomp_ret), decomp.c (option decomp, ETM_DECOMP_OFF), tests/ab.sh (the lobby route twice,
  rewrites off and on, memory and draw hashes every 10 frames; the translation's run saved and
  reused), tools/disfn.py (instructions with stub targets), tools/calltree.sh, tools/decomp_status.py,
  runtime/watch.c (ETM_WATCH write watchpoint), runtime/decomp/stackfix.c (frames cleared in both
  modes where the original copies stack leftovers: 0x64a2e9 found by the first A/B), HOWTO.md and
  BRIEF.md for helpers in worktrees (tools/worktree_init.sh).
- Batches: world (0x747b66, 0x6ea603, 0x646c33's and 0x6f309c's small subtrees), characters
  (0x6d0f2d's subtree), object submission (0x6f309c, 0x6f2732, 0x7b0aa8, 0x6f3ec5).

### 9b. Interpolation: the frame loop (groundwork, 2026-10-26)
- Update count [verified: disassembly of 0x6469f1, constants read from the exe]: updates this
  frame = carry + [0x97ec5c], where [0x97ec5c] = 60 / [0x97ec60] (the target rate: 60, or 30
  when 0x643ef7's flag is set; 50 / 25 in the PAL mode [0x97ec50] == 0, where [0x97ec54] = 1.2),
  plus one update per step the measured rate [0x97ec6c] (1 / the frame end's quantized step,
  0x7c0b5b) falls short, up to 5. The carry [0x97ec64] keeps the fraction, snapped to 0 below
  0.01 and to 1 above 0.99. At NTSC rates the carry is always 0. Second output ([ebp-0x20] of
  0x646e8b): updates that run 0x646367 (normally all).
- 0x646e8b [verified: disassembly]: the render pass runs first, once per view in the list
  [0x97f230] (next at +0x4f0): the view (0x5cc bytes) is copied to 0x97f234, its matrix +0x54 to
  +0x94 and 0x97f474 to 0x97f4b4, then drawn (world 0x747b66, objects 0x6f309c, the mode's
  +0xcc, 0x646c33, then 0x646cec / 0x646c9e if [0x97f800]+0x1b4) and [0x97ef30](view, 1, n x
  [0x97f570]) gets the frame's simulated time; the copy goes back. Then the n updates run
  ([0x97ec68] = 1..n), the last preparing the next frame's views. So a frame shows the state the
  previous frame's updates left, whatever n is; with n = 0 it would draw the same state again.
- Readers of the measured rate [0x97ec6c]: 0x6465c8 (min / max statistics), 0x646e8b (a log line
  every 32 frames), 0x70f002 and 0x712c84 (60 / rate as a frame's update count: these assume
  whole updates per frame), 0x706bcf, 0x728aa0 (log), 0x7bfd39 (sets it at loads), 0x78afa9
  (resets it to 60). Drawing between updates must keep these seeing whole updates per frame.
- One lobby frame's Direct3D calls (ETM_LOG_FRAME=6800, work/frame6800): 1,458
  DrawIndexedPrimitive, 1,452 SetTransform (world, props: fixed function), 4,136
  SetVertexShaderConstantF (376 draws with c0..c13 set: skinned characters [guess: from the
  counts]), 8,502 SetTextureStageState, 4,882 SetRenderState, 40 DrawPrimitiveUP (2D), 3 render
  targets.
- Built (2026-10-26): the frame (0x646e8b) and its update count (0x6469f1) rewritten as C
  (runtime/decomp/frame.c; identical on tests/ab.sh, first try). Option frame_rate = 60 (the
  game's own pacing, default) | uncapped: in game a frame's updates follow the clock (0..4, the
  fraction carried), decided once a frame at the input poll (0x7b5333, hooked: skipped before a
  frame without updates, so input is read once per update and no "pressed" is lost) or at the
  update count; the measured rate the game divides 60 by is set to 60 / the frame's updates
  (1e9 with none: 0x71c1d5 and its 0x712c84 / 0x70f002 count 0); the frame end's timer read
  (0x677a90 from 0x7c0915, hooked) gives one update's step already passed, so it doesn't wait, and
  its cut-scene advance (0x7c1b30, hooked) restarts the timer instead. The render pass is blended
  by the previous frame's carry (interp_apply before it, interp_restore after; fields copied after
  each update: runtime/interp_game.c), which keeps the drawn time one update and one frame behind
  the clock at a constant distance. Front-end frames and in-game frames that stop before the
  updates keep the game's own wait.
- Under --test the game's own pacing runs 2-3 updates a frame in game (the virtual clock's reads
  make each frame a little over 1/60 s, so the frame end picks 1/30 or 1/20): the A/B route
  covers frames with several updates. main.c sched(): with frame_rate = uncapped, scripted keys,
  --hash checkpoints, --dump-memory and --frames count the game's own frames (a free frame counts
  its updates), so runs at different drawn rates meet the same input on the same update.
- tests/rate.sh: the lobby route uncapped at 60 a second (one update a frame, the design) and at
  120 (from frame 2300, just into the game: the route's menu keys are timed for the default test
  clock), memory compared at an update. First result (update 4500, before the input fix): the game
  state (camera, actors) the same; differing only frame counters (0x97ec48, 0x97ff54), frame-rate
  statistics (0x97ff48, 0x931f5c), the frame timer, the input update's frame counter 0x97bb30
  (0x5f648c, called per poll: fixed by skipping the poll), the world's per-frame dynamic lighting
  (0x7a44a0 from 0x7475c8, alternating per frame: double-buffered vertex light records
  [guess]), and the projection kept by the render pass (+0x94: the last mesh's far plane).
- Interpolated so far: the camera (each view's view matrix +0x240, view x projection +0xd4,
  frustum planes +0x114, position +0x2d4, one group: a cut in any draws all at the latest;
  position over 300 units or a view-matrix element over 0.5 in an update is a cut) and the
  characters (interp_skin from skin.c: the pose code runs from the true state on every frame, as
  it writes game state; its output, the skinning matrices, is blended between the last two
  updates' poses for drawing and put back after). Checked (tests/rate.sh with ETM_LOG_CAMERA at
  120): the drawn camera advances evenly every frame, every other frame on the 60 run's position;
  memory at update 4500 still the 60 run's apart from the list above plus 0xe816a0 (the drawing's
  shader-constant staging, memcpy'd by 0x7ae788 in 0x7b1808: render scratch). After the input fix
  and the world step once per update, the input counter, the lighting buffers and 0x6e9415's
  counter match too.
- Rewrite status (2026-10-27): 100 functions in (world 32, characters 43, object submission 20,
  render pass 3, frame 2), tests/ab.sh same. Written but left to the translation
  (DECOMP_PENDING, objsubmit.h): 0x6f309c (the object pass), 0x6f2732 (an actor prepared: lights,
  level of detail, pose), 0x7b0aa8 (the actor drawer), 0x7b1838 (one model pass), 0x6f3ec5 (the
  light set), 0x7b1808 (the parts: CPU skinning into the dynamic vertex buffer) - with them in,
  the lobby route's first SetLight (frame 2283) differs in the last bit: the translation's
  compiled code narrows some x87 operations to SSE (rounding to nearest) and keeps others on the
  x87 (the game's rounding down), per site; objsubmit.h's f32 forces the x87 where needed.
  Characters are skinned on the CPU (0x7b1808, world matrix identity), so interp_skin's blended
  matrices reach the vertices.
- Not yet: objects placed by matrices (the object submission batch will name them), particles,
  held props (placed by the character hook's notice 1 at draw time: they move at the update
  rate until blended like the skins). Open (translation fidelity, not this work): the game's x87
  runs single precision rounding down (control word 0x047f); where GCC narrows a translated x87
  operation to SSE it rounds to nearest (characters helper, tools/fpround.py).
- Plan: larush-native's design (runtime/interp.c): after each update, copy the fields that place
  what is drawn (actors' positions and orientations, skeleton poses, the camera's view record,
  particles); before the render pass blend them by the frame's place between the last two
  updates; after it put the latest back, so the simulation never sees a blended value. The
  in-game frame then runs at the screen's rate with 0 or more updates (real-time accumulator in
  place of 0x6469f1's count), the front end staying at one update a frame. The rewrite's reports
  name the fields.

### 10. The port's settings in the game's own Options pages (runtime/menu.c, 2026-10-27)
Ant: "There are no settings in the settings menus." (GAME-GOALS.md: every setting in the game's
own menu.) Title menu > Options > Video now has, under Subtitles: Display, Resolution, V-Sync,
Frame Rate, Frame Limit, Anti-Aliasing, Anisotropic Filtering, HUD, Movies, Cut Scenes, Menus (and
Renderer since 2026-10-29: decision 11);
Options > Controls has Vibration under Fighting / Driving / Hovercraft / Mouse. All drawn,
highlighted and moved through by the game's own list code [verified: frames at 640x480 and
1280x720, options.txt rewritten by each change].
- How the game builds its pages [verified: disassembly; frames]: the Options screen (0x7028 bytes,
  constructor 0x65c06a, state [+0x24] run by 0x65fc55) keeps each page's rows in list widgets
  (+0x214 the labels, +0x2d4, +0x394 the Video page's values, 0xc0 apart): a 0x78-byte look copied
  in by 0x5f9ca0 (x +0x30, y +0x34 on the 640 x 448 design, wrap width +0x5c, row spacing across
  +0x64 / down +0x68), flag 0x10 (0x6574ca(2, on)) and state 0 (0x5f9ce0) for the highlight,
  items added by 0x6554fc ({look, UTF-16 text}, the text copied), 0x657401 empties, 0x6577af
  selects, 0x656962 draws, 0x6559be runs a list's own input. Texts: Text\TX<language>FE.bin
  (UTF-16; dword 0, count, offsets from +8) through 0x65526b on the screen's +0x1c0 (23 Subtitles,
  26 On, 27 Off, 28 Vibration). Menu actions 0x5f69bd(n) on [+0x994]: 0 up, 1 down, 2 left,
  3 right, 7 back; [0x98f048] the input slot 0x10-0x13 pressed this frame (0x65c1cf), which the
  pages take as up / right / down / left too [guess: the joystick's directions].
- The PC's Video page (0x660a16 build, 0x6617a7 run, 0x661b8a draw): one label (the Xbox's Gamma,
  Contrast, Brightness are looped over and not added) and On / Off across at x 300, the choice
  going to [0x97f810] when the page is left with back. Hooks: video_setup lets the setup run and
  lays its two lists out again as columns (labels x 70, values x 330, twelve rows from y 64, 27 (thirteen 25 apart with Renderer)
  apart: the game's own key-binding pages are denser), the values list holding one item per row
  (its selection, the page's row, highlighted as the Audio page's Output row); video_update is
  0x6617a7 in C with its calls in order (the +0x2d4 list's input, back's writes, the gamma calls
  of its left / right on Subtitles) plus up / down between rows (as the Audio page 0x6628b3: sound
  0x35d, wrapping) and left / right changing the row's value. No text-lookup hook was needed: the
  new labels are added as our own UTF-16 texts in program memory (galloc, only once the page is
  opened), which the game copies; existing words (Subtitles, On, Off, Vibration) come from the
  game's table, so they follow the game's language (the new labels are English).
- Controls (0x66c752 build, 0x66e9bf run through the list's own input, 0x66eab0 draw): hooks add a
  fifth label and its value as a one-item list in +0x394 (unused by the Controls pages), drawn
  after the page; left / right on the row toggles rumble. The pause menu's Controls (the same
  class, made by 0x61db35 as [0x97d250], drawn by another path in game) is left as it was.
- Applied at once: Display (win.c win_options; not in test runs, whose window stays hidden),
  Frame Rate (frame_rate_reapply + frame_limit_options), Frame Limit (frame_limit_options), HUD,
  Movies, Cut Scenes, Menus (game.c game_options_reapply: re-read on the next frame) [verified:
  Menus = Stretch stretched the page itself at 1280x720], Vibration (win_rumble_option),
  Subtitles (the game's own, on back). At the next start: Resolution, V-Sync, Anti-Aliasing,
  Anisotropic Filtering (read when the device is created); their value reads "(on restart)"
  while it differs from the one in force. Every change is option_set + options_save at once.

### 11. Our own Vulkan renderer (Ant, 2026-10-29, relayed by Recomp Advisor; GAME-GOALS exception)
Replace DXVK as the way the game draws, without waiting for a full playthrough. Rules: a Video
page row (Renderer: DXVK / Vulkan), DXVK the default until Ant has played ours; verified without
him (same scenes through both, frame-by-frame image comparisons, the usual checks), on the T7.
Base: larush-native's renderer (VULKAN.md there: host COM objects with Direct3D 9's vtables in
runtime/vk_d3d9.c over vk_core.c; tools/d3d2spv.py compiling shader bytecode to SPIR-V at build
time; bindless textures, constants in a per-frame ring, pipelines by content key and cached on
disk, dynamic rendering). This runtime came from the same lineage (d3d9gen.py wrappers, d3d9.c
specials), so Direct3DCreate9 hands out ours when renderer = vulkan and nothing above changes.
What differs from LA Rush:
- Fixed function: one lobby frame has 1,452 SetTransform, 8,502 SetTextureStageState, 378
  SetMaterial, 45 SetLight / 69 LightEnable, SetFVF draws (DESIGN 9b's frame log): most of the
  world and objects go through Direct3D's fixed-function pipeline, which LA Rush never used.
  Plan: one vertex and one fragment uber-shader written in GLSL, compiled to SPIR-V at build time,
  their paths chosen by specialization constants from a state key (vertex: transform, normals,
  up to 8 lights by type, material sources, texture coordinate generation and texture
  transforms, fog, point size; fragment: up to 8 texture stages' colour/alpha ops and arguments,
  texture factor, alpha test, fog, specular add), the driver dropping the unused paths at pipeline
  creation; the meaning follows DXVK 3.1.1's d3d9_fixed_function.cpp (the reference for parity).
- Shaders: vs/ps 1.x-2.0 (the shadow receivers' and the actors' passes) through d3d2spv.py.
- The movies (DivX frames uploaded to textures), the 2D layout (d3d9.c ui2d, SetViewport special),
  the projection hook and MSAA (d3d9_msaa.c) keep working under both.
Steps:
1. Inventory (port LA Rush's d3d9_inv.c and report): every method, state, format and FVF the
   test routes use (front end, films, lobby, cut scenes; driving and hovercraft need saves: [later]).
2. Round 1: LA Rush's renderer brought over and adapted, plus the fixed-function shaders;
   the Renderer row; draws everything the inventory lists; validation layer silent.
3. Pictures: the same deterministic --test frames through both (menus, a film, a cut scene, the
   lobby fight; 640x480 and 2560x1440), compared per frame as LA Rush does (2D identical; 3D at most
   0.5 % of pixels over 5 levels, PSNR >= 45 dB, no 16x16 block mean over 4 levels off); the memory
   and draw-stream hashes are renderer-independent, so tests/ab.sh and tests/rate.sh must still say
   "same" with renderer = vulkan.
4. Speed: frame times, game-thread and GPU time against DXVK, on the T7 and finally the Win Max
   (quiet); then the cuts LA Rush made (direct device calls from runtime/decomp, buffers copied
   once, pipelines made at load).
Where: the T7 for builds, image runs and comparisons (game files copied there once, with Ant's OK);
final speed and look on the Win Max.
Round 1 (2026-10-29; VULKAN.md is the renderer's own handoff): done as planned, with these findings.
- The test route makes **no shaders at all** [verified: the inventory, VULKAN.md 1]: the "~5 vertex /
  ~8 pixel shaders" above was a guess from the constant counts (the game sets c0-c13 per object with
  no shader bound). Everything is fixed function: the uber-shaders are runtime/shaders/ff.vert and
  ff.frag (DXVK 3.1.1's, their run-time state turned into specialization constants from the state
  key), the per-draw values a block in the ring. tools/d3d2spv.py is brought over for the shaders the
  game's D3DX makes from text (Shaders/SH_BackgroundShadow: some other level).
- The device must answer as DXVK's: our first CheckDeviceFormat accepted D32, and the game took D32
  for its depth buffer where DXVK's gave D24X8 (memory differed); fixed with DXVK's rules (ETM_DUMP_CAPS
  diffs both renderers' answers).
- d3d9_msaa.c never engages here: the game draws its 3D straight into the back buffer, which the
  twins leave alone, so Anti-Aliasing (4x by default) does nothing in this game under either renderer
  [verified: no "antialiasing" lines in the logs]. Left for the drawing side (not this round).
- Options > Video got its thirteenth row, Renderer (DXVK / Vulkan, on restart); the rows are 25 apart.

### 12. The level loader (Ant, 2026-10-29: "a debugger that loads you into those levels")
Testing (the Vulkan renderer's image checks, the rewrite's A/B, interpolation) only reached the front
end, the first scenes and the Post Office lobby: the test route starts a new game, and driving, the
hovercraft and the later levels need saves nobody has. runtime/levels.c starts any load point of the
game's own level table the way the game's Load page starts a saved game. Its header comment has the
details; the essentials [verified: disassembly of the functions named, the table read at run time,
the New Game route watched with ETM_WATCH on the world's level field and ETM_OPEN_BT (game.c: a
backtrace when a matching file opens) on the level files]:
- The table: built at start-up by 0x5d1138 in the world object ([[0x97f230]+0x4ec] = [0x97f720]):
  [+0x194] 18 "levels" (the game's word, "Illegal level number %d"), each {count, "load points"} at
  [+0x1a0], a load point 0x30 bytes (type, file "levels\X.3d2" opened as Levels\X.3d2_42, beat name,
  animation set, id). Names: 0x986678 + level x 0x170, {UTF-16 level name, count, {UTF-16 load point
  name, load point, character} x 30}, the texts of Text\TXeStc.bin ("THE POST OFFICE", "Closing
  Time"). Kinds from the type: 0 on foot, 1 driving, 2 driving as the gunner, 4 hovercraft pilot,
  5 hovercraft gunner, 7 multiplayer arena (the types' meaning [guess: from names, animation sets and
  the frames below, which agree]); level 17 is the in-engine cut scenes (New Game starts 17.19 Niobe /
  17.20 Ghost), 0 the multiplayer arenas and test rooms, 16 "SECRET" (Sparks' training construct, AI
  test rooms). 220 load points, of which 22 have no file on disk (test rooms, the freeway on foot 11.x,
  AITutorial2).
- How a level starts: the in-game handler (0x5392ad -> 0x539311, the world's state +0x190) loads the
  load point world [+0x20] / [+0x24] when world [+4] (updates since it began) is 0 (0x5d6c14 -> entry
  0x74864b -> file 0x741330), in one frame. Only 0x65f335, a front-end method, sets a real level:
  world [+0x20] / [+0x24] from the front end's [+0xc80] / [+0xc84], character [0x97ff10] from [+0xc88],
  screen [0x97ed80] = 2. The front end [0x98d08c] runs page [+0x20] in state [+0x24]: 1 logos, 6/7
  language, 2 title menu (0x65e161), 3 opening film, 4 character select and jack-in (0x65f40e), 8
  options, 10 load (0x65f09d). New Game and Load both end on page 4 state 2 (the jack-in film), then
  0x65f335. Load's way in: [+0xc80..+0xc88] from the save, [0x98c92c] = 0, [+0xc8c] = 1, 0x65d14c
  (films let go), the jack-in film into slot 0xd / 0xc, page 4, state 2.
- The loader does exactly Load's steps with the level, load point and character it is given and no
  save applied (so a new game's inventory and progress), difficulty [0x97f804] Normal (1, New Game's
  default; 0 Easy, 2 Hard) unless asked, from the title menu once it is up (state 1 with its backdrop
  film): the jack-in plays and the game loads the load point itself.
- The developers' level select is cut: 0x6784c3 ("uidebug", reads Levels\LevelList.txt) has no caller
  and its per-frame state only prints "Unsupported"; nothing to re-enable. The front end's text table
  still has "Level Select" (TX<language>FE.bin id 2), which our menu row uses.
- Command line: --level NAME (file name, NAME@LEVEL where files repeat, or LEVEL.POINT), --character
  ghost|niobe, --difficulty easy|normal|hard; without --test it plays in a window as the menu entry
  does. --list-levels prints the table (kind, character, file, name, "(no file)"). Tests: ETM_LEVEL,
  ETM_LEVEL_CHAR, ETM_LEVEL_DIFFICULTY; the language page is answered with Enter; when the load point
  runs the scripted input's mark is set, so ETM_KEYS, ETM_PAD (new: "+frames") and ETM_SHOT (new:
  "+frames") count from the level's start; a status line every 100 frames (level, screen, cinema bars,
  film, camera); ETM_LEVEL_FRAMES=N ends the run N frames in; ETM_LEVEL_SKIP=1 skips a film (Escape) or
  in-engine scene (Enter) at the start. Deterministic under --test like the route.
- Title menu: a sixth row, Level Select (option level_select = on by default; off is the original
  menu). It fills the menu's own list with the levels, then a level's load points ("Closing Time -
  Ghost"; the file's name where the records have none), seven rows at a time with "..." where more
  follow, the game's sounds; Escape goes back; accepting a load point starts it [verified: frames of
  the title menu, the level list, CITY DRIVING's two load points, and the drive it started].
- Checks: tests/levels.sh (the sweep: each level loaded, 1500 frames played, frames at +1, +300, +700,
  +1500, one input by kind, a second run without it to show the input changed something; the table in
  work/levels/table.txt; tools/levels_sheet.sh makes one picture of each level's four frames).
  Inputs: on foot the left stick forward, driving / pilot A, gunner the left stick across (it aims; the
  right stick does nothing there), cut scene Escape; both runs skip a film or in-engine scene at the
  start (ETM_LEVEL_SKIP). First sweep (2026-10-29, Win Max, DXVK, 640x480): all 11 loaded and played
  1500 frames without a crash, the input changed the game in all 11:
    1.0 B1_PO_01_N_EXTERIOR_START  on foot, Niobe: the exterior, then walks into the black (below)
    1.10 B1_PO_02_G_LOBBY          on foot, Ghost: the lobby, tutorial tip, walks into a corridor
    7.0 B3_MC_01_G_..._CENTRAL     on foot, Ghost: beat 3's film and scene skipped, the chateau's hall
    12.9 B5_PP_02_N_EXT_LOWER_SHAVEN on foot, Niobe: the power plant's corridor, distance marker counts
    2.0 B1_DC_01_N_CITY_SECTION_A  driving, Niobe at the wheel: the city at night, drives under a bridge
    2.1 B1_DC_01_G_CITY_SECTION_A  driving, Ghost with a rifle out of the window, Niobe driving
    5.0 B2_DA_01_G_MAIN_AQUEDUCT   driving, Ghost: the aqueduct; "YOU LOST THE PLANE" by +700 (no steering)
    15.0 B7_HC_01_N_..._TUNNEL_A   hovercraft, Niobe: the Logos' cockpit in the tunnels
    17.28 B2_AP_00_N_LOBBY_CIN     cut scene: film, then Ghost and Niobe in the car; Escape: save prompt
    17.20 B1_PO_01_G_EXTERIOR_START cut scene (New Game's): the white construct; Escape: save prompt
    15.3 B7_HC_01_G_..._TUNNEL_F   hovercraft gunner, Ghost: sentinels; GAME OVER by +1500 (no firing)
  Deterministic: the city drive by ETM_LEVEL and by --level, hashes every 50 frames to +1000, identical.
  tests/ab.sh: same (the route never touches the loader). With option level_select = off the route's
  691 memory and draw hashes equal those of the main checkout's own binary run the same day: the hooks
  change nothing else (the Level Select row itself, on by default, adds a list item to the title menu,
  so the route's memory differs from then on, the same with rewrites on and off).
- Found on the way (not fixed):
  - The main checkout's saved translation run (work/ab/hash_a.txt, 11:20 on 2026-10-29) no longer
    reproduces: the same binary run later gives a different memory hash and six fewer API calls from
    frame 10 on, while that day's later runs agree with each other. Something outside the code moves
    the test run [guess: DXVK's GPU memory budget (4.22 GiB then, 5.7 GiB later in the logs) if the
    game asks GetAvailableTextureMem, which DXVK answers from it; our renderer answers a constant].
    tests/ab.sh reuses a saved run whose id matches, so a stale one reports "differ" at frame 10 with
    nothing wrong; --fresh runs it again.
  - In-engine cut scenes skip with Escape, not Enter [verified: 17.20 under the loader, six Enters from
    +600 changed nothing, Escape at +600 brought "Would you like to save your progress?" at +700; left
    alone it runs +1700..+1800, as in the route (2283 to ~4005)]. So pad.c's "A presses Enter in cut
    scenes, which skips them" (2026-10-27, item 3) skips nothing (the pad's Start is Escape and does),
    and the test route's 3200 RETURN is not what ends the intro scene.
  - The freeway on foot (11.0 / 11.1, B4_FF_01_*) and its two cut scenes (17.29 / 17.30) are in the
    table but have no files in the repack, and level 11 has no names record: cut from the game.
  - 1.0 (B1_PO_01_N_EXTERIOR_START, Niobe's exterior; New Game goes from the cut scene 17.19 /
    17.20 to the lobby, so the story may never play 1.0 / 1.9 [guess]): walking straight ahead ~4300
    units from the start leaves Niobe alone on a black screen, the world no longer drawn [guess:
    outside the area meant to be walked; to look at].
  - Load points started without a save have what the level gives (Ghost's P229 in the lobby, Niobe's
    pistol with 12 / 0 in the power plant); no inventory or story flags from earlier levels.

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
- 2026-10-26: private GitHub repo AntAir267/etm-native (Ant's request); HUD at the corners in
  play; cinematics across the screen; in-engine scenes fill the screen with their framing; EAX
  reverb; the frame wait sleeps. The Hacking menu needs a saved game (not tested yet).
- 2026-10-26: drawing-code rewrite started: tooling, 3 render-pass functions (renderpass.c), the
  first stack fix; tests/ab.sh same over 691 checkpoints.
- 2026-10-27 (Ant's first play of the install): (1) "the actual game is stretched": the screen
  probe (win.c desktop_probe) measured the full-screen window before full screen applied; on
  Wayland at 2x scaling the 1280x720 probe is 2560x1440 pixels, and the game drew 16:9 on the
  2560x1600 panel (the machine also has a 1080x1920 and a 2560x1440 screen). Now it waits up to a
  second for the window to take its screen's size and otherwise takes the screen's desktop mode.
  (2) "the joysticks don't work": the machine shows three controller devices (Generic X-Box pad,
  Microsoft X-Box 360 pad, Handheld Daemon Controller), some silenced by Handheld Daemon; each was
  a joystick of its own and the game reads the first. pad.c now merges every controller into one
  joystick (buttons held on any, each stick from the controller pushing it furthest, rumble on
  all) [verified: scripted second controller walks Ghost]. (3) "can't skip the intro with a
  controller": the in-engine intro skips with Enter, which A pressed only in menus; A presses Enter
  in cut scenes too (game.c game_cutscene: in game with the cinema bars up). Correction
  (2026-10-29, the level loader's sweep): Enter does not skip cut scenes, Escape does (the
  route's lobby matched because the intro ends by itself, not because of the Enter at 3200);
  A now presses Escape in cut scenes. (4) "no settings in the
  settings menus": the Video page has only Subtitles; the port's options go into the game's own
  menus (helper in a worktree, under way).
- 2026-10-27: the port's settings in the game's own Options > Video and Controls pages (decision 10,
  runtime/menu.c, five hooks); tests/ab.sh same.

- 2026-10-29: our own Vulkan renderer, round 1 (decision 11, VULKAN.md): larush-native's renderer with
  Direct3D's fixed function as two specialized uber-shaders; option renderer = dxvk (default) | vulkan,
  Video page row Renderer. The test route draws only fixed function (no shaders); every image-check frame
  identical to DXVK's on the Win Max at 640x480 and 1280x720, within the limits on the T7; validation
  silent; tests/ab.sh same under both renderers, their hash files identical to each other.
- 2026-10-29: the level loader (decision 12, runtime/levels.c): --level / ETM_LEVEL / --list-levels and
  the title menu's Level Select start any of the table's 220 load points (198 with files) the way the
  Load page does; tests/levels.sh sweep: 11 levels (on foot both characters, the city drive both ways,
  the aqueduct, the Logos pilot and gunner, two cut scenes) loaded, 1500 frames each, no crash, input
  works in all; tests/ab.sh same.
- 2026-10-29: test runs no longer see the host's screens. The renderer (DXVK, and ours copying it)
  listed the first screen's display modes, and the game keeps that list in memory, so an A/B
  baseline saved with three screens plugged in stopped matching with one (frame 10, six calls
  fewer). Under --test, IDirect3D9 GetAdapterModeCount / EnumAdapterModes /
  GetAdapterDisplayMode answer a fixed list (640x480 .. 2560x1600 at 60 Hz) and a 1920x1080
  desktop (d3d9.c, d3d9gen.py SPECIAL); normal play asks the renderer as before. tests/ab.sh
  --fresh: same.
- 2026-10-29, the level loader's other two findings looked into: (1) Niobe's exterior start (1.0,
  B1_PO_01_N_EXTERIOR_START) going black after a long walk is the level, not the port: the same
  with the drawing rewrites off (ETM_DECOMP_OFF=all, frames identical); the camera keeps going at
  walking speed from z 4752 to past -17000 with nothing stopping it, while in the lobby (1.10) the
  same walk stops at a wall (x 2833) [verified: ETM_PEEK of the camera position, work/niobe]. The
  file is the New Game cut scene's set (17.19 / 17.20); the story never gives the player control
  there, and that side of it has no walls. (2) The freeway on foot (11.x, cut scenes 17.29 / 17.30)
  is cut from this data: its sounds and effects (sound/ and effects/Beat_4_Freeway/*FF_01*) and a
  city map (driving/aa_freeway_on_foot.cty) are present, the level files (Levels/B4_FF_01_*) are
  not; Level Select lists only load points whose file exists. The Xbox disc might hold it [guess:
  unchecked].

