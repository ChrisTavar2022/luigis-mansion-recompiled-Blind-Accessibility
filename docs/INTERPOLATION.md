# Frame interpolation

Code: `src/recomp/interp.cpp` (tapes, pairing, blending, test mode), `src/recomp/interp_vtx.cpp`
(draw identities, vertex-array copies, CPU-built vertices), `src/recomp/frame.cpp` (decoupled
guest, target frame rate, pacing), hooks in `src/recomp/hle_gx.cpp`, Aurora patch
`0005-capture-and-replay`. Background and the original design: docs/ENHANCEMENTS.md section 3.

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

## Known limitations

- On the test machine presentation never exceeded the display's 60 Hz even with VSync off
  (Aurora's Mailbox or Immediate present mode, D3D12 or Vulkan): `aurora_begin_frame` waits for
  the previous present. That looks like a driver-level limit (e.g. a frame-rate cap or forced
  VSync in the GPU control panel); targets below the refresh rate are hit exactly (45 and 50 fps
  measured). `LM_BACKEND=d3d12|d3d11|vulkan` picks the backend for such tests.
- Things the game snaps itself (Luigi's instant turns, a light switching on) stay snapped.
- Particles that die or are born between two game frames vanish/appear at the first in-between
  frame instead of fading.
- Replaying a tape starts from the GX state the previous replay left (unchanged from before).
