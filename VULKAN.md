# Our own Vulkan renderer (replacing DXVK)

Handoff for the move from DXVK to a Vulkan renderer of our own (DESIGN.md decision 11; GAME-GOALS'
2026-10-29 exception: switchable in the game's options, DXVK the default until Ant has played ours).
Kept current at every milestone: a later session or round carries on from here. The renderer is
larush-native's (its VULKAN.md is the longer record of the shared parts: read it for the design's
background), brought over with the fixed-function pipeline Enter the Matrix needs and LA Rush never
did.

**Status (round 1, 2026-10-29):** `renderer = vulkan` draws the whole test route (front end, film,
in-engine scenes, the lobby fight) through our renderer, all of it Direct3D's fixed function; every
picture of the image check is identical to DXVK's on the Win Max at 640x480 and 1280x720, and within
the limits on the T7's RTX 4070 (single edge pixels); the Khronos validation layer is silent on both;
tests/ab.sh says "same" under both renderers and the two renderers' hash files are identical [3].
DXVK stays the default; speed is not measured yet (round 2).

How to run and look:
- `--option renderer=vulkan` (or `renderer = vulkan` in options.txt, or Options > Video > Renderer:
  on restart) runs ours; `dxvk` (the default) DXVK. Ours falls back to DXVK if it cannot start
  (logged: "our renderer could not start; DXVK instead").
- `ETM_VK_VALIDATE=1`: Khronos validation layer on, its messages in the game's log ("vulkan ERROR" /
  "vulkan warning"). `ETM_VK_DEVICE=<name part>`: pick a GPU by name.
- `ETM_D3D_INV=from-to[,from-to...]`: the Direct3D inventory (1), one file a window in the user
  folder, and the shaders the game makes in <user>/shaders/. `tests/d3d_inventory.sh` runs the
  route's three windows into work/inv/; `tools/d3dinv_report.py work/inv` sums them up.
- `ETM_DUMP_CAPS=1`: the device's answers at the start (caps, adapter, format and multisample checks,
  modes) in the log, from whichever renderer runs (diff the two logs: 1.6).
- `ETM_VK_LOG_PIPES=1`: every pipeline made logged with its time. The user folder's
  vk_pipelines.bin and vk_pipeline_cache.bin: the pipelines made and the driver's cache (deleting
  them gives a cold start); pipelines listed there are made by worker threads at the next start.
- Checks of ours: `tests/vk_shots.sh [TAG] [SIZE...]` (the image check, 2.8), `tests/ab.sh --renderer
  vulkan` and `tests/rate.sh --renderer vulkan` (the logic checks under ours; `--serial` runs their two
  games one after the other), `tests/vk_compare.sh DXVK_DIR VULKAN_DIR` (larush-native's comparison).

## 1. Inventory: what Direct3D 9 the game uses

Measured with runtime/d3d9_inv.c (ETM_D3D_INV, hooked in main.c run_thunk: every Direct3D call the
program makes, its arguments, the objects' descriptions, and at each draw without shaders the
fixed-function state), one hidden --test run of the lobby route (tests/ab.sh's keys, fresh user,
640x480, every default, DXVK) with three windows: front (frames 1-2000: intro movies, language and
title menus, difficulty, the opening film), scenes (2283-4600: the jack-in construct, the in-engine
intro, the save prompt), lobby (4700-6910: the Post Office lobby, the HUD, a guard fighting Ghost).
`tests/d3d_inventory.sh`; work/inv/report.txt. [verified: that run, 2026-10-29]

### 1.1 Headline numbers
- **No shaders at all.** The route never calls CreateVertexShader / CreatePixelShader; every
  SetVertexShader / SetPixelShader is NULL. Everything is Direct3D's fixed-function pipeline:
  SetFVF draws (2D: XYZRHW | DIFFUSE | TEX1, stride 28; XYZ | DIFFUSE | TEX1, stride 24) and seven
  vertex declarations (position, normal, texture coordinates 0-1, a COLOR0 from stream 1). The game
  sets vertex shader constants c0-c13 for every object (4,020 calls a lobby frame) with no shader
  bound: staging nobody reads. (DESIGN.md decision 11 guessed "~5 vertex shaders and ~8 pixel
  shaders for the actors' and shadow passes": wrong for this route.) The game has D3DX linked in
  and makes shaders from text at run time (Shaders/SH_BackgroundShadow.vsh / .psh: vs_1_1 / ps_1_1);
  some other level may: tools/d3d2spv.py covers them (2.2).
- **Calls a frame** (lobby / scenes / front end): DrawIndexedPrimitive 1,188 / 1,437 / 0 (TRIANGLESTRIP
  only), DrawPrimitiveUP 49 / 239 / 119 (TRIANGLESTRIP), SetTextureStageState 7,959 / 4,415 / 11,
  SetRenderState 4,693 / 2,512 / 14, SetTransform 1,412 / 751, SetMaterial 367 / 168, SetLight 41 /
  8.5, LightEnable 64 / 20, SetTexture 1,226 / 677 / 117, SetStreamSource 746 / 347, SetIndices 380 /
  181, SetSamplerState 45 / 36 / 4.7, Clear 4 / 3.3 / 1, SetRenderTarget 3 / 2, Present 1.
- **Pipeline keys**: the inventory's simple key: 15 a lobby frame, 22 in the whole run; our renderer
  made 35 pipelines over the route (its key holds the fixed-function state too) [verified: vfull run's
  log, 2026-10-29].

### 1.2 States and the values seen
- Render states: ZENABLE 0/1, ZWRITEENABLE 0/1, ALPHATESTENABLE with ALPHAFUNC GREATER (ref 0) and
  GREATEREQUAL (ref 128, 8), ALPHABLENDENABLE with SRCALPHA / INVSRCALPHA and SRCALPHA / ONE, CULLMODE
  NONE / CW / CCW, ZFUNC LESSEQUAL, DITHERENABLE, FOGENABLE 0 (never on), SPECULARENABLE 0,
  STENCILENABLE 0, TEXTUREFACTOR (20 values), LIGHTING 0/1, AMBIENT (5 values), COLORVERTEX 1,
  NORMALIZENORMALS 0/1, DIFFUSE / SPECULAR / EMISSIVE MATERIALSOURCE material, AMBIENTMATERIALSOURCE
  material or COLOR1, DEPTHBIAS 0 / -1e-6 / -1e-5 / -0.0006, CLIPPING 1, VERTEXBLEND 0,
  CLIPPLANEENABLE 0, FOGVERTEXMODE 0.
- Sampler states (slots 0-1): ADDRESSU/V WRAP / CLAMP, MAG/MIN/MIPFILTER POINT / LINEAR.
- Texture stages (0-2): COLOROP MODULATE / SELECTARG1 / MULTIPLYADD / DISABLE, ALPHAOP MODULATE /
  SELECTARG1 / DISABLE; arguments TEXTURE, DIFFUSE, CURRENT, TFACTOR (no TEMP, SPECULAR, CONSTANT,
  modifiers); stage 1 often MODULATE(CURRENT, TFACTOR) with no texture; TEXCOORDINDEX 0 or
  CAMERASPACENORMAL (0x10000), TEXTURETRANSFORMFLAGS 0 or COUNT2 (texture matrices 0-1).
- Fixed-function vertex side at draws (FFVS lines): pre-transformed 2D; lit with up to three
  directional lights (actors) or up to three point lights (props), ambient from COLOR1 or the
  material; unlit with COLOR0 from stream 1 (the world's baked lighting).
- Transforms: VIEW, PROJECTION, WORLD, TEXTURE0-1. Materials: 367 a frame, most with a specular power
  (unused: SPECULARENABLE 0).

### 1.3 Formats, pools, resources
- Textures: DXT1 / DXT5 (MANAGED, 1-6 levels), A8R8G8B8 (MANAGED: menus, fonts, HUD), A4R4G4B4 (one),
  X8R8G8B8 DYNAMIC DEFAULT 640x448 / 640x256 (the movie frames: locked 0.8 a frame in the front end),
  X8R8G8B8 render targets 256x256 (x2), 512x512.
- Render targets a frame: the back buffer (A8R8G8B8, the scene drawn straight into it with the device's
  D24X8 depth buffer) and two 256x256 X8R8G8B8 targets without depth. No CreateRenderTarget,
  CreateDepthStencilSurface, StretchRect or queries from the game.
- Buffers: 3,690 MANAGED vertex buffers (stride 32 + a stride-4 colour stream), 969 MANAGED index
  buffers (INDEX16), one DYNAMIC | WRITEONLY vertex buffer locked with DISCARD 15 a frame.
- Clears: target + depth, target only (the 256x256 targets), depth only.
- One BeginStateBlock / EndStateBlock at the start, never applied; SetGammaRamp once (a near-identity ramp: 4).

### 1.4 What our own runtime adds (from the code)
- d3d9_msaa.c (antialiasing twins, 4x by default): **never active on the route** [verified: no
  "antialiasing" line in either renderer's log]: it doubles render targets drawn with the device's
  depth buffer, and Enter the Matrix draws its 3D straight into the back buffer, which it leaves
  alone. So the Anti-Aliasing option has no effect in this game today under either renderer (a
  finding for DESIGN.md; not changed in this round). Our renderer keeps larush-native's multisampled
  targets and resolves for it.
- d3d9.c: the 2D layout (ui2d_layout: DrawPrimitiveUP vertices moved), SetViewport (ui_viewport), the
  shots (GetBackBuffer, CreateOffscreenPlainSurface SYSTEMMEM, GetRenderTargetData, LockRect), buffer
  and texture locks through the program's copies.

### 1.5 The device's answers: the game's memory the same under both renderers
The memory and draw-stream hashes must not depend on the renderer (2.5), so every answer the game
gets must be DXVK's. Found by diffing both renderers' answers (ETM_DUMP_CAPS) and then the first
frames' memory and calls (`--hash --hash-every 1`, `--hash-calls`, memdiff.py, the inventory's
check lines with their answers) [verified: work/vk/capd.txt / capv.txt, hc_*, cl_*; 2026-10-29]:
1. CheckDeviceFormat said yes to D32 and D24X4S4 (DXVK maps them nowhere) and to depth formats as plain
   surfaces: the game took D32 for its depth buffer where DXVK's gave D24X8.
2. NONMASKABLE multisampling's quality levels: DXVK gives the sample counts there are (4).
3. The mode list: SDL lists 2560x1440 twice on this machine; DXVK drops exact duplicates, lists the
   first display SDL gives, truncates the refresh rate and sorts by width, height, refresh down.
4. CheckDeviceType: DXVK's adapter / back buffer pairs (full screen: X8R8G8B8 with X8R8G8B8 or
   A8R8G8B8, R5G6B5 with R5G6B5...); CheckDeviceFormat and CheckDepthStencilMatch its adapter format
   and format rules.
5. GetDeviceCaps of a software device (D3DDEVTYPE_SW): DXVK has none. Given caps, the game walked its
   formats a third time (70 more calls before the first frame).
6. The device's own caps: DXVK's device sets MaxVertexBlendMatrixIndex to 8 (255 with software vertex
   processing) where the adapter's say 0; the game keeps them. GetCreationParameters gives the
   creation flags.
7. Our side, not an answer: larush-native's path called d3d_classes_init at every Direct3DCreate9 (the
   game makes two), allocating the classes' guest vtables twice and moving every later program
   object; and it handed out one IDirect3D9 object, so the second Direct3DCreate9 had d3d9.c drop the
   first's program object as stale. Now the classes are made once and each call gets a new object
   (the device holding a reference to its parent, as DXVK's).
Caps (larush-native's dump of DXVK 3.1.1's) and the adapter identity were the same from the start.
Now the first frames' memory and draw hashes are identical, and over the whole route too (3).

## 2. Design

### 2.1 Interface
As larush-native's phase 1: the Direct3D 9 the game uses implemented in C as **host COM objects with
Direct3D's own vtables** (runtime/vk_d3d9.c over vk_core.c). Direct3DCreate9 in d3d9.c returns our
IDirect3D9 when `renderer = vulkan`; everything that called DXVK (the generated wrappers, d3d9.c's
special methods, d3d9_msaa.c, the shots) calls ours unchanged. runtime/decomp's rewritten drawing
functions call the device through game_call to the vtable methods as before (no direct calls in
this round). larush-native's native passes (bloom, AO: vk_pass.h) are left out.

### 2.2 Shaders (programmable)
tools/d3d2spv.py (larush-native's, unchanged in its compiler) compiles Direct3D 9 bytecode to SPIR-V
at build time into gen/spirv_gen.c, keyed by the bytecode's FNV-1a 64. Enter the Matrix's shaders are
made at run time by its D3DX from text, so the table's sources are: the game's Shaders/*.vsh / *.psh
assembled by tools/d3dasm.py [guess: the same bytes as D3DX makes: no level of the route makes them,
so not compared], the bytecode the game made in earlier runs (SHADER_CAPTURE, default
~/.local/share/etm-native/shaders: written by ETM_D3D_INV), and data/shaders. Two shaders today
(SH_BackgroundShadow). A shader without SPIR-V: its draws are skipped and logged.

### 2.3 Fixed function (new for this game)
Two uber-shaders written in GLSL, **runtime/shaders/ff.vert and ff.frag**, compiled by glslangValidator
at build time (the Makefile: SPIR-V validated with spirv-val, then C arrays gen/ff_vert.h,
gen/ff_frag.h); nothing is compiled at run time. They are DXVK 3.1.1's d3d9_fixed_function_vert.vert /
_frag.glsl (the parity reference: the pictures are compared with DXVK's) with DXVK's arithmetic kept
line for line, altered (and marked so, DXVK's zlib notice kept) so that everything DXVK reads from its
data buffer and spec-data buffer at run time is a **specialization constant** from our state key: the
driver drops every path a pipeline does not use.
- A draw takes the vertex uber-shader when no vertex shader is bound or the declaration has
  POSITIONT (DXVK's UseProgrammableVS), the pixel uber-shader when no pixel shader is bound; either can
  pair with a programmable one (the varyings are our renderer's: COLOR0-1 at 0-1, TEXCOORD0-7 at 2-9,
  FOG at 10; inputs at vk_input_loc's locations). Pseudo-hashes 1 and 2 stand for them in pipeline keys.
- **The vertex key** (7 constants, vk_abi.h VK_FFV_*; vk_d3d9.c ff_key_vs from DXVK's
  UpdateFixedFunctionVS / UpdateFog / UpdatePointMode): the declaration's POSITIONT, COLOR0, COLOR1,
  PSIZE, FOG; lighting, normalize normals, local viewer, range fog, specular, point scale, tweening,
  fog on and the vertex fog mode, a shader model 3 pixel shader, points; the four material sources
  (D3DMCS_*, masked by COLORVERTEX and the colours present, as DXVK); the enabled lights' types in index
  order (2 bits each, up to 8: point, spot, directional); per stage the texture coordinate set, the
  TCI_* generation (camera-space normal, position, reflection vector, sphere map) and the transform's
  count (D3DTTFF_COUNT1-4); per coordinate set its components in the declaration; the projected stages.
- **The pixel key** (VK_SPEC_ALPHA, _FOG, _KINDS, _PROJ shared with the programmable pixel shaders,
  plus VK_FFP_OPS / _ARGS / _FLAGS; ff_key_ps from DXVK's UpdateFixedFunctionPS): the stages in use (to
  the first DISABLE, or a stage without a texture whose colour reads TEXTURE), each stage's colour and
  alpha operation and three arguments with their modifiers packed as DXVK packs them (with its rules:
  the first stage's alpha from DIFFUSE when its result goes to TEMP and alpha is off; the last stage
  writes CURRENT), result to TEMP, the samplers' kinds (2D, cube, 3D, depth compare) and empty stages
  (they sample zero, DXVK's null type), the alpha test function (8-bit compare for 8-bit targets, as
  DXVK), fog (vertex, exp, exp2, linear; Z fog for non-perspective projections), specular add, point
  sprites. All 26 texture operations are there (DXVK's code); the route uses MODULATE, SELECTARG1 and
  MULTIPLYADD.
- **Per-draw values** in the frame's ring: the vertex block (ff.vert FFVsData, ~1.9 KB, std140) as the
  vertex constants' chunk 0 (set 1 binding 0), uploaded when a value in it changed (dirty_ffv: a
  transform, the material, a light, the viewport, the depth test, ambient, fog and point values):
  World x View, its inverse (normals), Projection, World x View x Projection, the eight texture
  matrices, the viewport's window-to-clip factors for pre-transformed vertices (DXVK's: z kept only
  with the depth test on), up to eight lights in view space, the material, ambient, fog and point
  values; the pixel block (texture factor and fog colour as D3DCOLORs decoded in the shader, the stage
  constants, the bump matrices and luminance) after the extra block (set 1 binding 1). The CPU-side
  matrices are multiplied and inverted in DXVK's order of operations (its Matrix4 operator* and
  tryInverse, the lights' transform and normalize, util_matrix.cpp), so the GPU gets the same bits.
- SetFVF's declarations are made from the FVF as DXVK makes them (fvf_decl, cached), classified as
  DXVK's Classify does; materials, lights (64 indices kept), GetLightEnable's 128, transforms
  starting as identities: Direct3D's / DXVK's behaviour.
- **Not covered** (none used on the route; each logged once if met): vertex blending (VERTEXBLEND
  weights, WORLDMATRIX 1-255), user clip planes, flat shading (SHADEMODE FLAT: colours stay
  interpolated), SetMaterial / light values beyond index 63. A recorded state block (BeginStateBlock /
  EndStateBlock) holds the whole state at EndStateBlock rather than only what was set in between: the
  game records one and never applies it.

### 2.4 State, pipelines, resources, passes (larush-native's)
Unchanged from larush-native's round 4 unless noted (its VULKAN.md 2.3-2.10): pipelines by content key
(plus the fixed-function constants: PipeKey.spec[18], vspec[7]; the list on disk is "ETVKPL01"), made
on the game thread or by worker threads from the list; dynamic state (viewport with Direct3D's half
pixel, scissor, cull, depth, stencil, blend constants); bindless textures and samplers, constants in a
per-frame ring; vertex and index buffers as ranges of 32 MB arenas with the program's own copy as the
host copy (d3d9.c hands it over at the first lock: vk_buf_host; its copies onto itself skipped);
textures one image each with uploads recorded in order; dynamic rendering with clears as load
operations, barriers by need; the back buffer's passes straight into the swapchain image; two frames
in flight; multisampled targets and resolves for d3d9_msaa.c. Changed for this game: locked rows'
pitch aligned to 4 as DXVK's (the program allocates its copies by the pitch: same memory under both).

### 2.5 The check method
- The game's logic does not depend on the renderer: tests/ab.sh (the rewritten drawing code against
  the translation, memory and draw-stream hashes) must say "same" under both, and the hash files of
  the two renderers' translation runs must match each other (the device answers the same: 1.5).
- Pictures against DXVK's, the same frames of the same deterministic --test runs (tests/vk_shots.sh):
  2D frames (movies, menus, the Video page) identical; 3D frames at most 0.5 % of pixels with a
  channel more than 5 levels off, PSNR >= 45 dB, no 16x16 block whose mean is more than 4 levels off
  (larush-native's limits). The Video page's Renderer value shows each run's own renderer: its box is
  painted out in both before comparing. When a frame fails: ETM_LOG_FRAME on that frame under both,
  then the first differing draw.
- The T7 (RTX 4070) runs the image check too; its pictures are compared DXVK against ours on the same
  machine (NVIDIA may round differently from RADV); the Win Max's are the final word on the look.

## 3. Round 1 results (2026-10-29)

- Files: runtime/vk.h, vk_abi.h, vk_core.c (Vulkan), vk_d3d9.c (Direct3D objects, the device, the
  fixed function), runtime/shaders/ff.vert, ff.frag (the uber-shaders), tools/d3d2spv.py (programmable
  shaders), runtime/d3d9_inv.c + tests/d3d_inventory.sh + tools/d3dinv_report.py (the inventory),
  tests/vk_shots.sh + tests/vk_compare.sh (the image check). d3d9.c: the renderer switch, ETM_DUMP_CAPS,
  vk_buf_host; menu.c: the Video page's Renderer row (thirteen rows now 25 apart); options.c: the
  option's help line; ab.sh / rate.sh: --renderer, --serial.
- Validation layer (ETM_VK_VALIDATE=1): **silent** on both routes at 640x480 and 1280x720, on the Win
  Max (RADV) and the T7 (NVIDIA 595, RTX 4070) [verified: tests/vk_shots.sh's counts, work/shots/
  winmax.txt, T7 job 20261029-110545]. One error was met and fixed on the way: a depth-only format in
  AMD's D32_SFLOAT_S8_UINT (D24X8, the game's depth buffer) needs both aspects in its layout transition.
- **Pictures**, tests/vk_shots.sh (hidden --test runs, DXVK and ours on the same machine, final code
  3f94a25 + the menu/docs):

  | frame | what | Win Max 640x480 | Win Max 1280x720 | T7 640x480 | T7 1280x720 |
  |---|---|---|---|---|---|
  | 1100 | language menu (2D) | identical | identical | identical | identical |
  | 1380 | title menu (2D) | identical | identical | identical | identical |
  | 1790 | Options > Video (2D) | identical* | identical* | identical* | identical* |
  | 1850 | Video page, Renderer row (2D) | identical* | identical* | identical* | identical* |
  | 1900 | the opening film (2D) | identical | identical | identical | identical |
  | 2700 | jack-in construct (3D) | identical | identical | 0.017 % >2 %, 56.8 dB, block 0.6 | 0.031 %, 54.8 dB, block 1.6 |
  | 3500 | in-engine street (3D) | identical | identical | 0.003 %, 67.8 dB, 0.2 | 0.003 %, 67.1 dB, 0.3 |
  | 4700 | lobby, HUD + message (3D) | identical | identical | 0.008 %, 67.2 dB, 0.3 | 0.008 %, 62.6 dB, 2.0 |
  | 5600 | lobby (3D) | identical | identical | 0.008 %, 66.2 dB, 0.5 | 0.028 %, 61.8 dB, 1.1 |
  | 6500 | lobby fight (3D) | identical | identical | 0.012 %, 61.5 dB, 1.2 | 0.028 %, 60.1 dB, 1.8 |
  | 6900 | lobby fight (3D) | identical | identical | 0.002 %, 72.9 dB, 0.2 | 0.002 %, 64.9 dB, 1.5 |

  (* the Renderer value's box painted out in both: it shows each run's own renderer.) Every frame
  passes; on the Win Max every frame is identical to DXVK's, 2D and 3D. On NVIDIA the 3D frames differ
  in single pixels along triangle edges (the construct's hair and collar outlines: work/vk/
  t7d2700_over.png) [guess: the last bits of vertex positions, NVIDIA's compiler treating the
  specialized shader's precise dot products otherwise than DXVK's shader with run-time values; not
  chased in round 1]. Also on the Win Max: the whole route every 100 frames from 1000 (60 frames),
  640x480: all identical [verified: work/vk/full_cmp.txt, before the device-answer fixes].
- **Logic checks** (Win Max): tests/ab.sh --serial "same (691 hashes to frame 6910)" with renderer =
  dxvk and with renderer = vulkan; and the two renderers' runs' hash files (memory and draw stream,
  691 checkpoints each, the translation's and the rewrites') **identical to each other** once the
  answers of 1.5 were fixed [verified: work/vk/checks.txt].
- Pipelines: 35 over the route on a cold start (each 10-160 ms with the validation layer, on the game
  thread; the list on disk has the next run's workers make them at the start).
- How the T7 ran this branch: the worktree's own folder (the repository's .claude/worktrees/
  agent-a690bbad773124c5b, inside the projects folder, so `t7 sync` / `t7 run` take it as the project path and
  copy only its git files; the main checkout's sync skips .claude/), with third_party/dxvk-native and
  sdl3 copied from this machine's and gen/ copied and dated after the sources (no translation and no
  venv needed there); `t7 run <that path> --wait -- make JOBS=16 && tests/vk_shots.sh`.

## 4. Round 2 (proposed) and what is not done
- Speed: frame times, game-thread CPU and GPU time against DXVK on the Win Max (quiet), then
  larush-native's cuts (direct device calls from runtime/decomp, the fixed-function vertex block in
  smaller pieces: only the lights and matrices that changed, pipelines made at load).
- The rest of the game: driving and hovercraft levels, other beats (need saves); shaders any of them
  make (SH_BackgroundShadow: check d3dasm's bytes against D3DX's, ETM_D3D_INV captures them).
- Threads: the game draws from a second thread at two moments, one frame at 0.85 s (thread 2) and the frames during a level's load (thread 21, 9 frames in the city drive), and the main thread draws no frames meanwhile [verified: frame logs, work/drive/rt/ant/log.txt; the Deck's run logs the same warning]. The renderer's device code has no locks. Whether the main thread makes device calls (textures, buffers) while the other draws is unchecked [guess: it may, during a load]; no crash from it so far, on the Win Max or on the Deck's one 40 s run to the menu. Log every device call's thread during a load before making the Vulkan renderer the default.
- Fixed-function features not met yet (2.3's list): vertex blending, clip planes, flat shading.
- Gamma: SetGammaRamp is kept and given back, not applied. The game sets one at the start that is the
  identity but for rounding (entry 64 is 16447, the identity's 16448; 128: 32895 for 32896; 255:
  65535) [verified: the log line of our SetGammaRamp]; DXVK applies any ramp that is not exactly the
  identity in its present blit, through a lookup texture, which at most moves a value by 1/65535
  before the 8-bit swapchain rounds it [guess: so the screen is the same; not measured: shots read the
  back buffer, before either's present, so they cannot show it]. The Video page has no gamma rows
  on the PC (decision 10). If a ramp that matters turns up, apply it in Present (a pass where the
  back buffer's passes now draw straight into the swapchain image).
- Antialiasing: d3d9_msaa.c never engages in this game (1.4); a fix belongs to the drawing side (the
  scene drawn into a multisampled back buffer, or the twin idea applied to the back buffer).
