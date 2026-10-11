# Frame interpolation

Code: `src/recomp/interp.cpp` (tapes, pairing, blending, test mode), `src/recomp/interp_vtx.cpp`
(draw identities, vertex-array copies, CPU-built vertices), `src/recomp/interp_tex.cpp` (texture and
TLUT copies), `src/recomp/shadows.cpp` (LM: camera source, shadow light), `src/recomp/frame.cpp` (decoupled
guest, target frame rate, pacing), hooks in `src/recomp/hle_gx.cpp`, Aurora patches
`0005-capture-and-replay`, `0021-replay-state-slots`, `0025-replay-copy-generations`. Background and the original design: docs/ENHANCEMENTS.md section 3.

## Setting

F1 > Visuals > **Frame rate**: a slider from 30 to 320 fps, plus **Match display refresh
rate** (greys the slider out and uses the refresh rate of the display the window is on, queried
through SDL3 every 0.5 s, so moving the window to another monitor follows it).

- **30** is the game's own rate, drawn directly in lockstep (exactly the old "Original 30").
- **Above 30** the guest is decoupled and every game frame is captured into a tape; the host
  presents at the target and replays the newest tape with its moving values blended toward the
  previous tape's. The game still runs at 30 Hz; the picture is one game frame (33 ms) behind.
- settings.ini: `[video] target_fps=<30..320>`, `match_display_refresh=true|false`. The old
  `frame_rate` key is migrated on load: 0 -> 30, 1 -> 60, 2 (unlimited) -> match display.
- Pacing (`frame::pace`): the host loop sleeps (SDL_DelayPrecise) to the target. With VSync on
  and a target at or above the refresh rate the swap chain paces instead, so the frame rate is
  the refresh rate; the menu says "(VSync limits this to N fps)" and the tooltip explains it.

## What was wrong (stability)

Measured with the deterministic test mode below in the nursery (`LM_START_MAP=2
LM_START_APPEAR=1`, Luigi walking with scripted stick input):

1. **Vertex data read after the game had changed it (the popping/vanishing objects).** A tape
   referenced the game's vertex arrays by pointer, and Aurora read them when the tape was
   replayed, while the guest was already building the next frame. Luigi's body (vertex data the
   game rewrites every frame) was drawn from half-rebuilt memory: in the A/B run
   (`LM_INTERP_ARRAYS=0`) his body disappears entirely from replays of some frames (frame 2608:
   only the hose and Poltergust remain, ~3300 pixels wrong), and which frames are hit depends on
   timing, so in real play it flickers. Original 30 never shows it because there the frame is
   drawn before the guest continues.
2. **Matrices paired by source address and occurrence.** The game builds many matrices in
   per-frame pools (e.g. 0x814E1000), so "the n-th load from address X into slot Y" is not
   necessarily the same object from one frame to the next when the set of drawn objects changes;
   such pairs blend one object toward another's position. (The pixel metrics could not separate
   this from the other causes; the trace shows pool matrices whose partner is thousands of units
   away.)
3. **Too-tight jump thresholds and entrywise blending.** Rotation entries changing by more than
   0.35 or translations by more than 400 units were shown unblended. That snapped Luigi's limbs
   and body whenever he turned (about 20-25 degrees per game frame), and far objects whenever the
   camera turned (they move a lot in view space), while everything around them was blended:
   visible as jitter. Texture-projection matrices (the flashlight's projected texture) were
   compared relative to entries that pass through zero, so about 300 per frame snapped.
   Blending matrix entries linearly also shrinks a turning object at the in-between frames.

## What it does now

- **Vertex-array copies** (`interp_vtx.cpp`). While capturing, every draw is parsed: display
  lists at `GXCallDisplayList`, and the raw vertex stream after `GXBegin` (game code writes it
  through the write-gather pipe). For each indexed attribute the highest index gives the range
  of the array the draw reads; that range is copied at the time of the draw (when the console's
  GPU would read it), and the tape's array-base commands are pointed at the copies (with their
  size, so Aurora uploads exactly the copy). Copies are shared per address until
  `GXInvalidateVtxCache`, after which the current arrays are loaded again so later draws get fresh
  copies. Every tape starts by loading the current arrays, so it never depends on another tape.
  Indexed matrix loads (`LOAD_INDX`) extend the matrix arrays the same way. About 1.2 MB of copies
  per game frame in a room.
- **Pairing by draw.** The values noted before a draw (position/normal/texture matrices,
  projection, lights) and the draw's own CPU-built vertices form a *bundle*, identified by what is
  drawn (display-list address and size, or primitive + vertex layout + position array for
  immediate draws) and by the kinds and sizes of its values. Bundles of the same identity are
  paired between frames, closest values first (rotation weighted against translation, a sprite's
  texture spin weighted strongly), then their values one by one. XF slots are deliberately not
  part of the identity: J3D hands out matrix slots per frame. Where many draws share an identity
  (particles), the common motion (the camera's, in view space) is taken out first and a draw may
  move at most 12 units plus 1% of its distance on its own: a particle that died and one born
  elsewhere are never paired, they just disappear/appear. What is left is paired by (kind, source
  address) only where that is unambiguous. Pairing runs once per tape on the guest thread (about
  0.6 ms including the copy bookkeeping). `LM_INTERP_PAIRING=source` uses only address pairing,
  for comparison.
- **Blending.** Position and normal matrices are split into rotation (quaternion, slerp) and an
  upper-triangular scale/shear part (Gram-Schmidt), so turning objects keep their shape and
  sheared matrices from non-uniformly scaled parents still blend. A pair is shown unblended
  (snapped) when the rotation is over ~150 degrees in one game frame (no telling which way it
  turned), a scale changes over 2x, or the translation jumps by more than half its distance from
  the camera (at least 200 units): teleports, respawns, wrong pairs.
- **Camera cuts.** If more than half of a tape's paired position matrices jump, the whole tape is
  shown as is. LM hides room changes and warps behind fades, so no smear was seen there.
- **Spawn/despawn.** A value with no partner is drawn as captured.

## Interpolating everything

- **Lights**: `GXLoadLightObjImm` position and direction (view space) are blended per light
  slot, the direction keeping its length (spot-cone falloff).
- **Texture matrices**: projection-style ones (flashlight projector, shadows) are blended
  row-wise; the rows are compared by their rotation/scale part only. Matrices that alternate with
  zero matrices are never blended toward zero. 2x4 matrices that spin texture coordinates in their
  plane (particle sprites) are blended by angle and scale about their pivot (up to ~150 degrees per
  game frame). Other small texture-coordinate matrices snap on a wrap (an entry jumping by more
  than 0.5).
- **Billboards and particles**: camera-facing sprites (the particle system's one-quad display
  lists, J3D billboards such as the spiderwebs) have no rotation in view space; slerp keeps it that
  way, so they stay camera-facing at every in-between frame while their positions, sizes and
  spins are blended.
- **CPU-built geometry**: immediate-mode draws under a perspective projection with direct float
  XYZ positions are noted as vertex blocks and paired by layout, vertex count and the nearest
  first vertices; positions are blended (a block moving more than 200 units snaps).
- **EFB-copy effects** are replayed with the rest of the tape, so they show the interpolated
  scene.

## Test mode and evidence

`LM_INTERP_TEST=<steps>` with `LM_FRAME_DUMP=<dir>,<first>,<last>`: the guest waits after each
game frame N while the host renders, with guest memory as it is at the end of N:
`fN_fresh.ppm` (tape N as captured = the 30 Hz frame) and `fN-1_s1..s<steps>.ppm` (frames N-2 to
N-1 at alpha s/steps; `s<steps>` is tape N-1 replayed one game frame late, as in real play).
With `LM_INTERP_TRACE=1` it also logs, per frame, what was snapped (with values) and pairing
statistics. Scripts used for the numbers below (pixel metrics on these dumps) are not in the
repo; the checks are:

- *stale*: pixels where `fN_s<steps>` differs from `fN_fresh` (a late replay must look exactly
  like the 30 Hz frame). 100 walking frames: 3308 with `LM_INTERP_ARRAYS=0` (Luigi's body
  missing), **0** with the copies. On main after the shadows merge (703f378) frame 2608 shows
  3161 again, with or without the copies: there Luigi's body is missing from the *fresh* replay
  and present in the late one, and the same body dropout happens in Original 30 (frames
  2608-2609, also with `LM_EFB_RAM=0` and the shadow options off). So it is not interpolation
  but GX state the frame inherits from the previous one (a fresh replay starts from the previous
  frame's end state, like the console; the late replay starts from its own end state); to be
  looked at with the shadow/GX accuracy work.
- *billboard tracking* (`LM_INTERP_TRACE` "match" lines): camera-facing sprites followed over
  three tapes; "jerky" = the middle position is far off the line through its neighbours (a wrong
  pair). 100 walking frames in the nursery, ~33,000 sprites tracked: 7,709 jerky with address
  pairing, 5,274 with the first per-draw pairing (large groups paired in order), **827** now.
- *snapped*: blobs that jump all at once between fN-1_fresh and fN_s1 and then stay. Over 100
  walking frames these are now almost only Luigi's instant 180-degree turns (the game turns him
  within one frame: nothing to interpolate) and the flashlight switching on. Position matrices
  shown unblended per frame went from ~90 (entrywise thresholds) to ~25 (rotation/shear
  blending); texture matrices from ~300 to ~50 (the remaining ones are 2D texture rotations of
  more than 90 degrees per frame). With per-draw bundles and sprite spins (October 2026) the
  nursery walk is at ~6 position and ~6 texture matrices per frame, and the pixel blobs that
  snap at the start of an in-between dropped from 166,000 to 18,000 pixels over 100 frames (what
  is left is Luigi's instant turns).
- Teleport (`LM_SKIP_TEST=2700:pos:...`) and warp (`LM_DEV_WARPS=2:3@2700`): no smearing; the
  room change happens under the fade.

Performance (RTX 3090 Ti, i9-12900KS, nursery, walking): 60.0 fps presented at a 60 target,
game 29.6-30.1 fps; tape ~600 KiB + ~1.2 MiB array copies per game frame; 1.1 ms guest time per
tape, 0.4 ms host time per present for the blended replay.

Test aids added with interp-everything: `LM_INTERP_NOBLEND=<mask>` shows the value kinds of the
mask (bit = `interp::Kind`: 0 position, 1 normal, 2 texture matrices, 3 projections, 4 lights,
5 CPU-built vertices, 6 colors) as captured, to find which kind causes an artifact;
`LM_INTERP_POISON=<item>,<phys>,<size>` (hex) fills that
guest memory with garbage during one `LM_INTERP_TEST` replay (e.g. `f2762_fresh`): if the replay
changes, it still reads that memory live (bisect the range); the trace logs a self-check of the
camera-factored and shadow-receiver values at alpha 1, the values without partner (with their draw
identity) and the camera cuts with eye move/turn.

## Dust and spiderwebs (October 2026)

User report: with interpolation on, the dust Luigi kicks up and the spiderwebs misbehaved.
`LM_INTERP_DRAWS=<first>[,<last>]` (frame numbers as in `LM_FRAME_DUMP`) logs every draw of
those tapes with its guest call chain, the values noted before it, and what each was paired
with.

- **Spiderwebs** (nursery, foyer) are J3D draws (`draw__8J3DShapeCFv`) of camera-facing
  billboards. J3D loads their matrices into whichever XF slots are free that frame (slots 6/9/12/15
  in one frame, 15/18/21/24 in the next), and pairing used the slot as part of the key, so they
  never found a partner and were drawn as captured: they moved in 30 Hz steps while the room
  around them moved smoothly (template tracking in the dumps: web +4 px at once, wall +1 px per
  in-between; now both +1 px per in-between).
- **Dust and other particle effects** come from the game's particle system (`fn_8012FD14`): every
  particle is the same 32-byte quad display list with a billboard position matrix and a spinning
  2x4 texture matrix, all loaded from one scratch address. About 180 such draws per frame were
  one group, too large for the nearest-value pairing then (it fell back to pairing in draw
  order, which shifts whenever a particle dies or spawns), so puffs were blended toward other
  puffs (flying across the cloud, stretching, doubling). Their texture spins (30-90 degrees per
  frame) also snapped. Now particles are paired as whole draws with the camera motion taken out,
  spins are blended by angle, and new/dead particles are left unpaired.
- A field-order bug in the value records also gave CPU-built vertex blocks a stride of 1, so they
  were never blended (they always snapped as "too far"); fixed.
- The scanner now sizes immediate-mode vertices with the layout as it was at `GXBegin` (the game
  changes it right after `GXEnd`); the "unparsed stream" (a full-screen 2D quad) is gone.

## Fixes from the SMS port (October 2026)

- Aurora 0022: `GXEnd`'s "vertex data not evenly divisible" check measures the draw in the
  calling thread's recording, not the FIFO (which the host replaying tapes appends to and
  resets). The draws were fine; the warnings were noise (LM: sporadic, 4-5 in some 60 fps runs,
  none in the A/B runs, so the reduction is not measured here).
- Immediate draws with direct positions: the draw identity no longer includes the bound
  `GX_VA_POS` array (stale for direct positions). In LM's nursery the CPU-built vertex blocks
  already paired (7.2 of 7.9 per tape before and after), so this changes nothing there; it keeps
  LM safe if a direct-position draw follows indexed ones with changing arrays.
- Light jump limit relative to distance (at least 400 units, else half the light's distance),
  and non-finite light values never blended or rescaled.
- Camera cuts to a nearby viewpoint: a tape is also a cut when the median relative move of its
  paired position matrices exceeds 0.08 and four times the previous tape's (`Tape::camStep`;
  `LM_INTERP_TRACE` logs "scene step"). In the opening cutscene it catches two shot changes the
  half-of-matrices test missed (scene step 0.34 / 0.39 after ~0.003); gameplay walking in the
  nursery and foyer stays at 0.04 or less (no false cuts).

Measured with `LM_INTERP_TEST` (nursery walk at 60 and 120 fps, 210 frames each; foyer walk at
60 fps, 160 frames): 0 stale pixels before and after; pairing statistics (position matrices,
billboards, vertex blocks, jerky counts) within run-to-run noise of main.

(Fixed by interp-everything, texture copies and copy generations: 0 stale pixels over frames
1880-2120.) Was open: in the attract-mode opening (crows on the branch, the second-to-last frame of that shot)
the late replay of one tape draws the lit models darker than its fresh replay (88,685 px; the sky
and grass are unchanged). It is the same with `LM_NO_SHADOW=1`, `LM_EFB_RAM=0` and with or
without the replay state slots, so it is neither the shadow pass, copies to RAM nor carried GX
state; display lists and vertex arrays are copied into the tape. Remaining suspects: data Aurora
still reads by pointer at replay time (textures and TLUTs via `GX_AURORA_LOAD_TEXOBJ`), which the
game may already have replaced for the next shot.

## Cost of closing a tape (guest thread)

Pairing runs on the game thread when a tape is closed, so it eats into the 33 ms game frame.
The 5-second `[interp]` stats line reports it as `tape close X ms (guest: arrays, copy, draws,
rest, cut)`: vertex-array bookkeeping, copying the tape, pairing draws, pairing leftover values by
source, and the camera-cut check.

In the foyer (~3,300 values per tape) the per-draw pairing took 13.4 ms: groups of 448, 224 and
125 identical particle draws were compared all-pairs (up to 200,000 candidate pairs per group,
every one sorted, including the ones rejected by the motion limit), and every draw's position was
re-read from the tape for every comparison. Now (paired/unpaired counts unchanged within
run-to-run noise):
- each draw's position is read once; the common motion is the median over at most 64 sampled
  draws instead of all of them;
- in groups of more than 1,024 pairs, only predecessors within the motion limit are visited
  (sorted along the axis the group is most spread out on, then a range scan), so a draw has a
  handful of candidates instead of hundreds; rejected pairs are never stored;
- particles (one CPU-built vertex block per draw) have their first vertices decoded once per
  group, and candidates are sorted as packed integer keys;
- the per-draw index lists are one flat array (one allocation instead of one per draw).

Foyer: tape close 13.7 ms -> 1.05 ms (draws 13.4 -> 0.8 ms; copy 0.1-0.2 ms); opening cutscene
and title 0.1-0.3 ms.

## Everything interpolates (interp-everything, October 2026)

Goal: every visible thing moves smoothly between the 30 Hz game frames, a replay never shows
memory the game has already changed, and shadows are both smooth and stable. Parts marked
**generic** carry over to other games on the same runtime (SMS); the rest is LM glue.

### Test scenarios and metrics

`tools/interp/scenarios.sh <out dir> <scenario> [fps] [steps]` runs one with a scratch
APPDATA/`LM_CARD_DIR`, `LM_INTERP_TEST=3` (fresh replay + two in-betweens + late replay per game
frame, see below), `LM_INTERP_TRACE=1` and its deterministic `--input`, and prints
`tools/interp/metrics.py`'s summary (`LM_DISC`, `LM_PYTHON` with numpy + Pillow):

| Scenario | What happens | Frames |
|----------|--------------|--------|
| A nursery (`LM_START_MAP=2 LM_START_APPEAR=1`, `LM_LIGHTNING=0,90,1`) | walking, fast turns (stick reversals), nozzle aiming (C stick), flashlight, lightning strikes with shadows, GBH open/close (Y), dust, spiderwebs, door to the next room | 2580-2960 |
| B gold ghosts (`LM_TELEPORT=0,550,-3300,100`, `LM_GHOST_HUNT=9 ...`, see RENDERING.md) | ghosts appear, hover, attack, vanish; stun, vacuum, capture | 3280-3600 |
| C opening (title, `--input "600:START*10,1000:A*6"`, nothing skipped) | the attract/opening cutscene: crows, camera cuts, gate, foyer entrance | 1880-2120 |
| D nursery, fire element (`LM_TEST_STATE=poltergust+fire`) | fire blown (L), vacuum (R) with its air stream and dust, turns | 2580-2920 |
| water | as D with the water element | 2580-2920 |
| ballroom (`LM_TELEPORT=1800,0,-1400,100`) | ballroom, quick left/right turns, nozzle aiming, walking | 2580-2900 |

Per game frame (from the dumps; pixel threshold 40/255, blobs = after a 2-pixel erosion so
edges that legitimately move one pixel per in-between are not counted):
- **stale**: pixels where the late replay differs from the fresh one (must be 0);
- **snap blobs**: regions that change completely between the previous frame and the first
  in-between and then stay (something shown unblended), or only at the last step;
- **back-and-forth**: pixels whose brightness goes up and then down (or the reverse) by more than
  24 within one interval (inconsistent blends, e.g. a shadow sliding away and back);
- per in-between, the values shown unblended (snapped) and the values without partner.

### What changed

1. **Textures and TLUTs are copied at capture time** (`interp_tex.cpp`, generic). A replay read
   texel data by pointer when it first needed a texture; a late replay could find the memory
   already reused. Every `GXLoadTexObj`/`GXLoadTlut` in a tape now points at a capture-time copy
   (shared by texture object + data version, else by content hash, so a texture is copied once).
   Textures go through Aurora patch 0025's new *content* pointer, so they keep their address for
   EFB-copy lookups. A texture over memory an EFB copy wrote is left to Aurora's copy paths only
   while that memory still holds what it held at the copy (hashed at the copy and at the load;
   copy-to-RAM writebacks update the hash); the GBH, for one, later loads ordinary textures into a
   buffer it once copied the screen into.
2. **Normals bound through `GX_VA_NBT`** were not tracked by the vertex-array copies (the slot was
   computed from the attribute number, 25), so bump-mapped models (the GBH body, ...) were drawn
   from live memory. Found by poisoning guest memory during a replay and bisecting the range
   (`LM_INTERP_POISON`). Generic.
3. **EFB copies per replay** (Aurora 0025 `GX_AURORA_COPY_GENERATION`, generic). A replayed tape
   re-runs its EFB copies; they used to overwrite the same copy textures the tape's own early draws
   read (copies made in the previous frame), so every replay after the first showed the frame's
   own copies where the previous frame's belonged. Copies now go into one of four generations
   (the tape's sequence number), and the replay state slots (0021) save/restore the copy-texture
   map and Aurora's copy-to-RAM entries with the rest of the state. During replays copies are not
   dropped for memory the CPU changed (the guest is on a later frame by then).
4. **Copy-to-RAM readbacks only from the first replay of a tape.** Every replay wrote the frame's
   copies back into guest memory again, up to a frame later, over memory the game may have reused
   meanwhile (the GBH's screen-capture buffer holds GBH model data once it closes): guest memory
   corruption, visible as the GBH vanishing while it closes. Generic (Aurora 0025, bit 6).
5. **Colors blend** (generic): channel ambient/material colors, light colors, TEV color and
   konst registers (also when a display list sets them), fog color. They belong to their draw and
   are paired through the draw pairing (the n-th write of a register for that draw). Lightning
   flashes and fades now fade at the presented rate instead of in 30 Hz steps.
6. **Camera factoring** (generic; LM supplies the view matrix address, `interp::viewMatrixSource`
   in shadows.cpp: the game camera's GX view matrix, `lbl_803A3820 + 0x18C`). Every position/normal
   matrix and light is noted with the camera it was drawn under (sampled from guest memory at the
   load, so the shadow pass is factored by the light's camera). A model-view matrix is blended as
   blended camera x blended world matrix: the scene moves as one rigid whole (each product used
   to follow its own path in view space), billboards stay exactly camera-facing, far objects keep
   their distance while the camera turns. Draws are paired by their world-space positions, and
   draws of different passes (main, shadow caster) are kept apart.
7. **Shadows: smooth and stable** (LM; `LM_SHADOW_SNAP=3`, the new default). The caster pass is
   factored by the light camera LV and blended like everything else, so the shadow map is
   re-rendered at every presented frame from the blended light and casters. The receivers' shadow
   texture matrices T = K x G (K = the game's light projection or depth-ramp matrix x LV, read from
   the shadow object after the caster setup; G = the receiver's world matrix, or the inverse view
   the game built the actors' post-transform matrices with) are rebuilt as K(t) x G(t) with
   K(t) = P(t) x LV(t) from the same blended light. G is solved from the projection matrix of the
   same draw (the depth-ramp K is singular). In test mode a self-check rebuilds every factored
   value at alpha 1 and compares it with the captured one (`self-check: 0 of N ... differ`).
   `LM_SHADOW_SNAP=1` keeps the old 30 Hz shadows.
8. **2D is interpolated** (generic): HUD panes, screen transitions and dialogue boxes are paired
   per draw like 3D (not camera-factored); a 2D matrix that jumps more than 96 units snaps.
9. **Camera cuts from the game camera** (generic): a tape is a cut when the camera the most
   position matrices were drawn under moves its eye by more than 250 units or turns by more than
   30 degrees in one game frame (the matrix-based tests stay as a fallback). In the opening
   cutscene this catches all five shot changes; walking and turning stay far below.
10. **Lights stay themselves**: a GX light slot holds whichever light object the game loaded for
    that draw; a light is blended only with the same light object of the previous frame, and its
    color only when its position was blended (a lamp's color at the flashlight's position made the
    in-between frames brighter than both neighbours in the water scenario). Values drawn from a
    camera that jumped (the shadow pass when another light takes over: flashlight, room light,
    lightning, a spray) are shown as captured, and so are the receivers' shadow matrices.
11. **Pairing within a draw**: a skinned model that loads its joints in another order the next
    frame (index pairing would make them jump) is paired nearest-first instead. Scale changes blend
    up to 16x (particles growing, eyelids closing); environment/reflection texture matrices blend
    like rotations unless their translation wraps.

### Results

Before (main at the start of this work, 55748e4) and after, same scenarios and frames (snapped and
unpaired are averages per in-between frame over all value kinds; "snap blobs" include the
game's own instant events: lightning strikes, the GBH opening/closing cut, the flashlight or room
lights switching on, a fire burst appearing):

| | stale px | snap blobs | back-and-forth | snapped / in-between | unpaired / in-between |
|---|---|---|---|---|---|
| nursery, before | 83,150 (GBH closing frame) | 1,015,870 | (not measured) | 1.8 | 23.2 |
| nursery, after | **0** | 640,861 (412,806 are 3 instant events: lightning strike, GBH open/close cuts) | 63,810 (snapped shadows: 68,317) | 4.3 | 32.4 |
| ghosts, after | **0** | 135,006 (52,681 the flashlight going off/on for the vacuum) | 28,592 | 3.8 | 38.2 |
| opening, after | **0** (the crow frame had 88,685) | 118,614 (the shot changes, all detected as cuts) | 11,087 (texture detail sliding under the moving camera) | 0.0 | 4.2 |
| fire, after | **0** | 599,103 (room lights on, fire bursts appearing) | 30,076 | 14.9 | 45.0 |
| water, after | **0** | 708,590 (room lights on, spray starting) | 63,009 | 19.6 | 70.1 (13 are spray droplets moving too fast to pair) |
| ballroom, after | **0** | 254,740 | 35,786 | 2.0 | 42.3 |

More values are interpolated now (colors, 2D, the shadow caster pass), so the snapped/unpaired
counts per frame are not comparable to before one to one; per tape about 1.5% of ~5,300 values
have no partner (spawns, despawns, the frame a light or effect switches) and ~0.1% are shown
unblended.

Shadows: with the old blending (`LM_SHADOW_SNAP=0`) the flashlight shadows slid back and forth
(documented in RENDERING.md); snapped (`=1`) they were stable but moved at 30 Hz (all of a
shadow edge's change lands on the first in-between: counted in the snap blobs); now (`=3`) they
move with the presented frame and the back-and-forth count over scenario A is at the snapped
level (63,773 vs 68,317; the first attempt, with the actors' receivers rebuilt from the sampled
camera, had 400,650 and the self-check showed 124 mismatching values per frame, now 0).
Original 30 is byte-identical to main (17 dumped frames of scenario A at 30 fps, incl. lightning,
GBH and the door, compared pixel by pixel with main's executable: 0 pixels differ).

### Cost

Nursery walk (scenario A without test mode), RTX 3090 Ti, 120 Hz display with VSync: target
120 -> 120.0 fps presented (240 -> 120: the display), game 29.8-30.2 fps. Per game frame: tape
~630 KiB + vertex-array copies ~360 KiB; texture copies are made once per texture (4 KiB per
frame while walking, a few MiB when a room loads). Tape close (guest thread) 1.4-2.6 ms, of which
pairing 1.1-2.3 ms (up from ~1 ms: colors, more values per draw); replay 0.7-0.8 ms per presented
frame (host). About 5,300 values paired per tape, 60-130 without partner, 1-7 shown unblended per
presented frame.

## Known limitations

- On the test machine presentation never exceeded the display's 60 Hz even with VSync off
  (Aurora's Mailbox or Immediate present mode, D3D12 or Vulkan): `aurora_begin_frame` waits for
  the previous present. That looks like a driver-level limit (e.g. a frame-rate cap or forced
  VSync in the GPU control panel); targets below the refresh rate are hit exactly (45 and 50 fps
  measured). `LM_BACKEND=d3d12|d3d11|vulkan` picks the backend for such tests.
- Things the game snaps itself (Luigi's instant turns, a light switching on) stay snapped.
- Particles and objects that die or are born between two game frames vanish/appear at the first
  in-between frame instead of fading (no partner to blend with; fading would need a per-draw
  alpha override in Aurora's pipelines).
- 2D/HUD interpolation is verified only for what the scenarios show (GBH, health, element meter,
  room name); dialogue boxes and the pause screen were not exercised.
- Shadows: when the "shadows at any distance" fit steps its near/far planes, the caster depth and
  the receivers' depth ramp are blended separately for that one interval (the ramp is not linear
  in near/far), which could show for a single in-between frame.
- The `back-and-forth` metric also counts fine texture detail sliding under a moving camera, so
  it never reaches 0 in textured scenes.
- (Fixed) Replaying a tape used to start from the GX state the previous replay left; each
  replay now starts from the end state of the tape before it (Aurora patch 0021 replay state
  slots, `replayPair`; from the SMS port).
