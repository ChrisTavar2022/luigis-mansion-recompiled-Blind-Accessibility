# Rendering notes

GX features the game needs and how the port's Aurora implements them. Aurora changes live in
`patches/aurora/*.patch`.

<!-- Sections below: flashlight cone (color texgen) and Z textures. Other agents add their own
     sections; keep each feature in its own top-level section. -->

## Flashlight cone: color texgen (GX_TG_SRTG) and missing vertex colors

**Symptom.** In gameplay the flashlight lit the walls but its visible beam did not draw. The
intro cutscene uses a different model (`opwf_cone`, drawn by the game's S3D code) and was fine.

**The draw.** Gameplay uses the J3D model `handlight_ef.bmd` with the texture-scroll animation
`handlight_ef.btk`, both from the static archive (`kt_static/`), loaded by `fn_800AA128`
(names at `0x80359750`/`0x80359764`). Its three materials (read from the BMD's MAT3 section):

| Material | Stages | Texgens | Notes |
|----------|--------|---------|-------|
| `hl_mat_hlc_a` (core) | 3 | TEX0 x TEXMTX0 (2x4, btk scroll); POS x TEXMTX1 (3x4, view projection map); **SRTG from COLOR0** | stage 0 color = lerp(C0, C1, TEXC) with C0 = (255,255,100) yellow; alpha = TEXA x TEXA x TEXA(SRTG) |
| `hl_mat_hlin` | 4 | same plus TEX1 x TEXMTX2 | |
| `hl_mat_hlout` | 4 | same | |

All three: one color channel, COLOR0 lit by light 7 only (mask 0x80) with clamp diffuse and
spot attenuation (the flashlight light), additive blend (SRCALPHA, ONE), Z compare LEQUAL
without update, Z compare before texturing, no alpha test, no fog, no indirect, K colors white.
The core material takes its material color from the vertex (`matSrc` = vertex) although the
model has no vertex colors.

Two Aurora gaps hid it:

1. **Color texgen used the wrong source.** `GX_TG_SRTG` (XF texgen types 2/3) sets s,t to the
   R,G of the *lit* color channel result (COLOR0 or COLOR1). Aurora read the raw vertex color
   attribute instead, which this model does not have, so every lookup hit texel (0,0), the
   alpha that multiplies the beam was 0 and the additive blend added nothing. Now the texgen
   reads the channel's lighting output (`out.cc0`/`cc1` in the vertex shader), the color
   channel is computed even when no TEV stage uses RASC/RASA, and, like on hardware, the
   texture matrix, post-transform matrix and normalization are not applied (they only apply to
   regular texgens). The beam's brightness now follows the spot light's falloff.
2. **Missing vertex color = black.** With `matSrc` = vertex and no color in the vertex, Aurora
   used (0,0,0,0), so the core material's lit color was 0 and the core vanished. On hardware
   the color of the last vertex that had one lingers (see Dolphin issue 13783); that history
   is not tracked, so the port now uses white, which is what Dolphin does. Checked which draws
   this changes: from boot through the foyer only the flashlight core and one 5-stage material
   whose missing COLOR1 RGB is never read by its TEV, so nothing else changes.

Before/after (`LM_TEST_STATE=poltergust LM_START_MAP=2 LM_START_APPEAR=1`, the nursery, Luigi
turned to face the camera with scripted stick-down taps): before, only the bulb glows; after,
the beam fans out with its yellow core, and from the side it is the familiar soft cone.

Cost: none (shader generation only).

## Z textures (GXSetZTexture)

`GXSetZTexture(op, fmt, bias)` writes BP `0xF4` (24-bit bias) and `0xF5` (format bits 0-1:
0 = Z8, 1 = Z16, 2 = Z24X8; op bits 2-3: 0 disable, 1 add, 2 replace). With an op set, the
texel fetched by the **last TEV stage** (whether or not the TEV uses its color) is read as an
unsigned depth value, the bias is added, and the sum either replaces the rasterized depth or
is added to it, wrapping at 24 bits. Depth testing then happens after texturing.

Implementation (Aurora patch `0008-color-texgen-and-z-textures.patch`):
- `GXSetZTexture` emits the two BP registers; `regs.cpp` decodes them (so display lists that
  write them directly work too) into `GXState::zTexOp/zTexFmt/zTexBias`.
- The pipeline config carries the op and format when the last stage has a texture and
  texcoord; the shader then samples that texture and writes `@builtin(frag_depth)`.
  The bias is a uniform (changing it does not create pipelines).
- Texel interpretation follows the hardware texture unit, which only sees the low 4 format
  bits: Z8 data is read as I8 (z = I), Z16 as IA8 (z = A << 8 | I, A being the first,
  high-order byte), Z24X8 as RGBA8 (z = R << 16 | G << 8 | B). The texture-object load maps the
  Z formats to those formats, so the data decodes like any other texture.
- The 24-bit result is converted to Aurora's depth convention (reversed Z:
  `1 - z / 0xFFFFFF`); for ADD the rasterized depth is first converted to the 24-bit value.

**Uses in Luigi's Mansion.** `LM_TRACE=801F828C` (the game's `GXSetZTexture`) and the
disassembly show one user: `fn_8000B228` (European `fn_8000B7CC`), the sub-viewport clear of
the Game Boy Horror screen (view 1). It draws a black quad with `GXSetZMode(on, ALWAYS, on)`
and `GXSetZTexture(REPLACE, Z24X8, 0)` from an all-0xFFFFFF texture, resetting that rectangle
to the far plane, then disables the Z texture (`GXInit` also disables it). The port used to
move the quad onto the far plane in a hook (`HLE(fn_8000B228)`/`palfn_8000B7CC`); that
workaround is removed, and the live GBH camera (`LM_GBH_CAM=2`, foyer) renders through the
real Z texture path.

Cost: only draws with a Z texture get the extra sample and depth write (one quad per frame
while the GBH camera is on), so there is no Rendering toggle.

Not covered: Z textures built from EFB depth copies would depend on the copy's encoding.
Aurora's Z16 copy conversion stores the high byte in I and the low byte in A (the opposite of
the IA8 view the hardware gives a Z16 texture), so a game sampling such a copy as a Z texture
or as IA8 would read swapped bytes. Luigi's Mansion never feeds a copy to `GXSetZTexture`, but
it does make EFB depth copies: `fn_8005D8CC` copies Z16 (640x480, with clear) and the code
around `0x8005EA5C` copies Z8. How those textures are sampled (and so whether the Z16 byte
order matters) has not been checked yet.

*Update (EFB copies to RAM, below):* the Z16 copy is Luigi's self-shadow map, read only as an
IA8 texture by a TEV `GR16` compare. The game's own depth-ramp LUT and swap tables make that
compare consistent only if the IA8 view of the copy has I = depth bits 23..16 and A = bits
15..8, i.e. the console stores a Z16 copy low byte first (the reverse of an ordinary big-endian
16-bit texel). That is what Aurora's conversion and the copy-to-RAM encoder produce. A Z16
*Z texture* built from such a copy would then presumably compose z from I (high) and A (low);
nothing in LM does that.

## Rooms vanishing behind doorways (foyer entry flicker)

**Symptom.** In the cutscene when Luigi first enters the foyer, on the close-up of Luigi with
the staircase behind him, the upper wall and glass ceiling above the stairs disappear (black)
for about 8 frames, then pop back (user frames `docs/issues/foyer_flicker_1.webp` OK,
`_2.webp` missing). Same at Original 30 and at 4:3, so not interpolation or widescreen.

**Not a renderer bug.** A per-frame display-list log (`LM_DL_TRACE=<file>`, below) shows the
game itself stops calling `GXCallDisplayList` for every part of two models (the room behind
the stairs, `0x80EF3D00..0x80EF7D40`, and its furniture, `0x8078A180..0x8078D980`) in exactly
the frames where the block is missing. Aurora's vertex-array sizing/sharing (patch 0002),
`GX_CULL_ALL` (0004), draw merging, scissor and viewport were therefore not involved.

**How the game picks rooms.** `fn_80012EE0` walks the portal graph each frame: for every
doorway of a visible room, `fn_8001333C(portal, outRect, parentRect)` gets the doorway's four
corners (`fn_80018238`), multiplies them by the camera's projection*view matrix
(`lbl_803A3C98 + 0x258`, built by `fn_80014228`), divides by w, maps to the screen rectangle
at `lbl_803A3C98 + 0..6`, takes the bounding box (one pixel margin) and intersects it with the
parent rectangle (`fn_80013200`). A non-empty rectangle adds the room behind the doorway to
the draw list with that scissor (`fn_800130BC` -> `fn_8001792C`; drawn by `fn_8001798C`).

**The bug (original game code).** There is no near-plane clipping. The only check is that
not all four corners are behind the camera. With the camera close to the doorway into the
room behind the stairs, two corners have w < 0; dividing by a negative w mirrors them, all four
projected corners land right of the screen (x = 750..1300) and the intersection with the
parent is empty, so the whole room is skipped. Logged in the bad frames: corners
`(985,211) (967,-45) (751,456) (1191,-175)`, 1/w = `0.001, -0.003, -0.002, 0.002`.
The recompiled code computes what the console would (`fctiwz` saturation and the 16-bit
stores match, and the inputs are plain game math), so this most likely happens on hardware
too; it is fixed as an enhancement.

**Fix.** `src/recomp/portal_fix.cpp` wraps `fn_8001333C` (hook `us pal`; the PAL function
differs only in relocations). After the original runs, if the doorway straddles the near plane
(some corners with z + w < 0 in GX clip space, but not all), it replaces the rectangle with the
box of the doorway clipped to z + w >= 0 (corners in front plus the plane crossings of every
corner pair), mapped and intersected with the parent the same way. Doorways fully in front or
fully behind keep the game's result, and a closed door (`fn_8001B278`) stays empty. Setting:
F1 > Enhancements > General > "Fix rooms vanishing near doorways" (`portal_clip_fix`, on by
default); `LM_PORTAL_FIX=0/1` overrides it for A/B tests. Cost: negligible (only doorways that
straddle the camera are recomputed).

Verified: the whole foyer-entry cutscene captured frame by frame (no missing room in any
frame; with `LM_PORTAL_FIX=0` the 8-frame gap returns), and title, file select, a training
map, foyer gameplay and the Nursery look the same with the fix on and off. Patch 0008 (color
texgen / missing vertex color) does not affect it: with 0008 merged and `LM_PORTAL_FIX=0` the
room still drops out for the same 8 frames (the newly visible flashlight beam on that shot
makes the black block easy to miss). The other display-list gaps left in that cutscene are
frames where the clipped doorway is genuinely off screen.

**Debug aid.** `LM_DL_TRACE=<file>` writes one line per frame (`F<n>`) listing every
`GXCallDisplayList` as `addr:size@caller<caller...` (guest stack back chain). Diffing the sets
of consecutive frames finds draws that drop out for a few frames.

**Side observation (not the cause, not changed).** Patch 0002 shares dynamically sized array
uploads per frame by pointer (`sDynamicUploads`); `GXInvalidateVtxCache` clears each array's
`cachedRange` but not that map, so an array the CPU rewrites in place mid-frame would reuse the
frame's earlier upload. Nothing in this cutscene does that.

## EFB copies to RAM

Toggle: **EFB copies to RAM (accurate)** (`[rendering] efb_copy_to_ram`, default on;
`LM_EFB_RAM=0/1` overrides). Code: Aurora patch `patches/aurora/0009-efb-copy-to-ram.patch`
(`lib/gfx/efb_ram.{hpp,cpp}`, C API `include/aurora/efb_ram.h`) and `src/recomp/efb_copy.cpp`.

### Console behaviour

`GXCopyTex` writes the EFB rectangle into main memory as a texture in the format given to
`GXSetTexCopyDst` (tiled: 4 bpp in 8x8 blocks, 8 bpp in 8x4, 16 bpp in 4x4, RGBA8/Z24X8 as
4x4 blocks of 64 bytes with the AR pairs followed by the GB pairs; big-endian texels). Anything
that later reads that memory, a texture of any format/size/offset, the CPU, or DMA, sees those
bytes; anything that writes it replaces them.

### What Aurora did

Copies stay GPU-only: the copy is resolved into a texture (at the upscaled render resolution)
keyed by the destination pointer, and any texture whose data pointer equals that key samples it,
whatever format and size it was created with. Memory is never written, and the association lives
until the pointer is copied to again, even after the game frees the buffer and loads something
else there.

### What Luigi's Mansion does with its copies (US, measured with `LM_COPY_LOG=1`)

| Destination | Copy | Read as | Code |
|---|---|---|---|
| shadow buffer (heap, 0x96000) | Z16 640x480, every frame | **IA8 640x480** texture (TEV `GR16` compare against a 256x256 IA8 depth ramp: Luigi's self-shadow) | `fn_8005D8CC` copy, `fn_8005D41C` texture, `fn_8005DD50` TEV setup |
| `lbl_804D86BC` (0x8021E000) | RGB565 640x480, twice per frame | RGB565 same size | `fn_8005F518` / `fn_8005F8B8` |
| `lbl_804D86B8` (0x802B4000) | RGBA8 320x240 (box-filtered) | RGBA8 same size | `fn_8005F498` |
| 0x803A4D00 | RGB565 70x100 from (250,140 140x200) | same | `fn_80025C98` |
| transition captures | RGB565 640x480 | same | `fn_800081A8` (LMDisplayUtil) |
| title/file select | 4 x RGB565 148x116 | same | `fn_800494E4` |

- The shadow buffer is the one copy read with a different format. It and the two screen
  buffers are also reused as scratch memory for ARAM/DVD loads (Game Boy Horror archive,
  `fn_80121884`; `fn_80066DB8`, `fn_8005336C`), i.e. written by DMA/CPU between copies.
- After the screen-buffer copies the game calls `GXPixModeSync` + `DCInvalidateRange` on them
  (cache hygiene before reusing the memory, no CPU reads of the pixels were found). No GXPeek
  calls exist.
- The 148x116 title buffers are freed after the menu and the memory is reused for other textures
  (e.g. Luigi's IA8 textures at +0x540/+0xD60) at different pointers.

### What the port does now

1. **Every copy** of a supported format (all of Aurora's copy conversions plus RGBA8/RGB5A3) is
   recorded as a memory range `[dest, dest + GXGetTexBufferSize)` with a hash of the memory
   there (`efb_ram::on_copy`, FIFO thread).
2. **Textures** (`resolve_sampled_textures`): a texture overlapping a recorded copy
   - exactly matching it (pointer, format, size) keeps Aurora's fast copy texture (upscaled:
     the enhancement on top of the accurate base);
   - otherwise (different format, size or offset, or spanning several copies) is **decoded from
     the copy's GX bytes**: a compute pass encodes the copy texture into the exact console bytes
     (native resolution, tiling, byte order: `EncodeShader`), and a fragment pass decodes them
     as the requested format (`DecodeShader`, same conventions as Aurora's CPU decoders). Both
     run before the render pass that samples the texture, in the same frame, with no CPU sync.
     Partial overlaps start from the memory under the texture and lay each copy's bytes over it
     in copy order. Palette (C4/C8/C14X2) and CMPR reads keep Aurora's existing paths.
   - Luigi's shadow map is the case that takes this path in LM: the Z16 copy is read as IA8.
3. **CPU/DMA writes**: when a texture is resolved over a recorded copy, the memory is re-hashed
   (once per frame per copy); if it changed, the copy is dropped and Aurora's pointer association
   evicted, so the texture comes from memory. (Fixes stale copies shown for reused heap memory
   with the same pointer.)
4. **CPU reads**: copies into ranges the game `DCInvalidateRange`s (how GameCube code makes
   GPU-written memory visible to the CPU) are encoded and read back into guest memory. The
   readback is asynchronous: the bytes are written at the next guest frame boundary after the GPU
   finishes the frame (1-2 frames after the copy), and only if the memory is unchanged since the
   copy (or still holds our previous writeback), so a CPU write after the copy is never clobbered.
   A same-frame synchronous readback would need a mid-frame GPU submit/stall, which Aurora's
   single-encoder frame model doesn't support; LM never reads copy pixels with the CPU, so this
   is not needed here. `LM_EFB_READBACK_ALL=1` reads back every copy (test aid).

Encoded copy semantics follow Aurora's copy conversions (`tex_copy_conv.cpp`): e.g. Z16 = depth
bits 23..16 in the IA8 intensity byte and 15..8 in the alpha byte, which is how LM's shadow TEV
(swap tables + `GR16` compare) expects it.

### Results

- Bit-exactness check: in the intro (copies on vs off, 2x internal resolution, 550 frames dumped
  at native resolution) the frames are identical, i.e. the GPU encode/decode of the shadow map
  gives the same image as Aurora's direct Z16 sampling there. (Feeding the decode deliberately
  shifted bytes also left the intro door scene unchanged: the self-shadow receiver doesn't affect
  those shots.) The mouth flicker itself is rare and random and was not reproduced; this feature
  removes the copy-related differences from hardware (native-resolution, byte-exact reinterpretation
  of the shadow map, stale-copy eviction). Re-check if the flicker is still reported.
- Regression run (title, file select, intro, foyer with HUD, Game Boy Horror map screen) renders
  correctly with the toggle on.

### Cost (foyer gameplay, 2x internal resolution, Original 30)

- CPU in efb_ram (hashing, resolves, writebacks): ~0.6 ms per guest frame (~5 MB hashed per
  frame: 5 copies + writeback checks), on the FIFO/host threads. Whole-process CPU over 25 s:
  33.6 / 33.2 % of a core on vs 30.4 / 33.1 % off (within noise).
- GPU: one compute encode + one fullscreen decode of the 640x480 shadow map per frame, plus
  encodes of the two DCInvalidateRange'd screen buffers (~1.5 MB/frame read back). Negligible next
  to the scene.

### Test aids

- `LM_COPY_LOG=1`: every GXCopyTex (frame, dest, rects, format, caller LR), textures created
  over copied memory (same/different format), and efb_ram counters every 300 frames.
- `LM_FRAME_DUMP=<dir>,<first>,<last>[,<step>]`: writes guest frames `<first>..<last>` as
  native-resolution `.ppm` files, captured through the copy-to-RAM path (works with the toggle
  off too). Frame numbers count GXCopyDisp calls; runs drift a few frames apart, so align by
  content when comparing.
- `LM_EFB_RAM=0/1`, `LM_EFB_READBACK_ALL=1`.

## GX accuracy audit (TEV, EFB format, copy filter, fog, lighting, textures)

Aurora patch `0010-gx-hardware-accuracy.patch` (incremental on 0001..0009); host side in `src/recomp/hle_gx.cpp`
(`gx::applyAccuracySettings`, display copy state) and `src/recomp/crt.cpp` (present chain);
toggles in F1 > Visuals > Rendering. What the game uses was traced with `AURORA_GX_AUDIT=1`
(each distinct pipeline feature, logged once) and `LM_GX_AUDIT=1` (state calls with arguments)
from boot through the title, the foyer (dark, flashlight) and the lab/training room. Test aids:
`LM_ACCURACY=<mask>` (1 integer TEV, 2 EFB color depth) and `LM_ACCURACY_AB=<s>` (alternate
configured/all-off every s seconds and log fps per phase, for A/B screenshots of one run).

| Area | What LM uses | Aurora before | Status |
|------|--------------|---------------|--------|
| EFB pixel format | `GXSetPixelFmt(RGBA6_Z24)`, dither toggled per draw | 8-bit target, no dither | **Fixed** (toggle "EFB color depth", on): output quantized to 6 bits/channel with a 2x2 ordered dither when the draw has dither on, expanded back to 8 bits; blended draws quantize color only (alpha stays the blend factor; the blend itself is still 8-bit). Verified A/B in the lab: mean -0.9 levels; a dark area goes from ~98 to ~25 distinct levels (dither grain/banding as on hardware) |
| TEV arithmetic | ADD/SUB, unclamped regs, scale x2, GR16 compare, K0-K3 selects, swap tables, up to 8 stages, `GXSetTevColorS10` | float lerp, [-4,4] clamp, k/8 konsts | **Fixed** (toggle "Integer-accurate TEV", on): 8-bit A/B/C (low byte of a register), s11 D/registers, C widened by C>>7, per-stage rounding, clamp 0..255 / -1024..1023, konst k/8 = k*32, HALF = 128, 8-bit vertex colors, rounded indirect texels, 8-bit fog blend. Verified: lab scene changes ~50% of pixels by exactly 1 level, unbiased, no breakage. SUB/DIVIDE_2 rounding is a model (+-1 LSB) |
| Display copy | `GXSetCopyFilter(aa=0, vf=1, {6,0,20,12,20,0,6})`, gamma 1.0, clamp top+bottom, Y scale 1.0, 640x480 | GXSetCopyFilter/Gamma no-ops; YScale returned 0 | **Fixed** (toggle "Display copy filter", on): present-time pass with the game's taps (0-1 line above, 2-4 this line, 5-6 line below = 6/52/6 of 64) in EFB lines (scales with resolution), edge clamp, gamma 1.0/1.7/2.2, before the CRT filter. `GXSetDispCopyYScale` returns the XFB line count. Not applied to texture copies (EFB copy code) |
| Early Z + alpha test | `GXSetZCompLoc(1)` with alpha compare GREATER 0 and Z update | Z written only for passing pixels | **Verified, unchanged**: emulating "Z written before the alpha test" (depth prepass) broke Luigi's face decals and pillars, so the hardware evidently does not write Z for alpha-rejected pixels here. Experiment removed |
| Fog | only `GX_FOG_NONE` seen; range adjust enabled (center 0), `GXInitFogAdjTable` once | range factor from a guessed NDC formula (much weaker than the table) | **Fixed model**: LUT built from the table (entry i = 256x factor at 32(i+1) EFB px from center, interpolated, 1.0 at center, last entry beyond), in EFB pixels at any resolution. Not visible in tested scenes (fog off) |
| Texture filtering | min LINEAR/NEAR, edge LOD, no mips | hw min-filter decode had NEAR_MIP_LIN/LIN_MIP_NEAR swapped | **Fixed** decode table (raw-BP texture setups). LOD bias/edge LOD fine |
| Lighting | spot (clamp diffuse), specular (shininess form), dist-attn fns, SRTG from COLOR0 | per-vertex, same formulas | **Reviewed, no change** (SRTG lit-color fix is in 0008). Open: Aurora normalizes transformed normals; hardware may not |
| Indirect | ghost layer (`fn_8005FDD4`, `GXSetTevIndWarp`; only drawn while `r13+0x768` holds its 128x128 warp texture, which the per-map setup table at `0x803002C4` creates for one map), J3D indirect materials | float offsets | Texel rounding added in integer mode; the rest of the indirect path is unchanged. **Not exercised by LM in any tested scene**: `AURORA_GX_AUDIT=1` logged no indirect stage and `LM_TRACE=8005FDD4` no call through boot, title, file select, lab, training room (gold ghost), Portrificationizer, foyer, nursery, gallery (map 6), GBH scan/map/inventory screens. Ghosts (gold ghost in the training room) are not drawn with indirect warping |
| Lines/points | width 6 (1 px), viewport jitter field=1 only | width/6, perpendicular expansion | No change needed for LM (hardware widens along x/y; noted) |
| Logic ops, dst alpha | no logic op; dst alpha used | fine | No change |

### Toggles (F1 > Visuals > Rendering, all on by default, persisted in settings.ini `[rendering]`)

| Toggle (ini key) | What it does | Visible effect | Cost (measured below) |
|---|---|---|---|
| Integer-accurate TEV (`integer_tev`) | TEV stages evaluated in integer WGSL (8-bit A/B/C, s11 D/registers, per-stage rounding/clamp) | +-1 level on ~40-45% of pixels, unbiased (mean +0.06); nothing structural | see table |
| EFB color depth (`efb_color_depth`) | fragment output quantized to the EFB format (RGBA6 / RGB565) with the PE's 2x2 dither when the draw enables it | ~1.4 levels darker on average (LM draws much of the scene with dither off, where the PE truncates), 6-bit banding rings in smooth gradients (flashlight pool on the floor), dither grain where dither is on | see table |
| Display copy filter (`display_copy_filter`) | present-time pass (`dcopy_filter` in crt.cpp, ahead of the CRT chain) with the game's GXSetCopyFilter taps + GXSetDispCopyGamma | ~5% less vertical high-frequency detail (6/52/6 of 64), no color shift (mean 0.2 levels); only runs when the taps/gamma change the picture | see table |

The fog range LUT and the texture min-filter decode are plain correctness fixes with no
toggle (no runtime cost: the LUT is rebuilt only when the fog range state changes; LM never
enables fog).

### Verification (merged build, US, `LM_FRAME_DUMP` at 1x internal resolution)

Deterministic runs (`LM_INPUT_CLOCK=frames`, nursery `LM_START_APPEAR=1`,
`LM_TEST_STATE=poltergust`, flashlight sweeps, X/B for the GBH) with all toggles off, TEV only,
EFB only and all on produce frame-identical scenes, so the same dump frame can be diffed:
- TEV only vs off: mean |d| 0.2; 0.3-2% of pixels differ by more than one level, at most
  3-12 levels on a handful of isolated pixels at the flashlight beam's edges; flashlight
  cone, beam core, Luigi's self-shadow (Z16 copy read as IA8, GR16 compare), GBH HUD, coins,
  bear/ball/door models identical by eye.
- EFB only vs off: mean |d| 1.4, max 8-11 levels; 93-97% of dumped pixels are
  exact 6-bit expansions (the rest come from blended draws, whose blend result is still 8-bit:
  WebGPU cannot quantize after fixed-function blending).
- Title screen, file select, lab dialogue, foyer (web, flashlight), Portrificationizer, training
  room with a gold ghost, gallery, GBH scan view (sub-viewport 3D + EFB copies), GBH map and
  inventory screens: rendered with all toggles on, no shader errors, no broken colors.

### Performance

RTX 3090 Ti, 5120x3840 internal (resolution scale 8), Original 30, nursery gameplay;
`LM_ACCURACY_AB=10` alternates each configuration with everything off and GPU utilization is
sampled with nvidia-smi (other test instances shared the GPU, so these are averages over ~5
on/off phase pairs). **Provisional: measured under load** (several other game instances and
builds were running on the machine); to re-measure on an idle machine.

| Configuration vs all off | Median paired GPU-utilization delta | Paired deltas (pts) | Frame rate on / off |
|---|---|---|---|
| Integer TEV only | -2.3 pts | -6.7, +6.7, -2.6, -2.3, +3.9 | 29.9 / 29.8 |
| EFB color depth only | -0.3 pts | +16.0, -21.7, +0.5, -14.4, -0.3 | 24.4 / 27.2 (machine load, both phases) |
| Display copy filter only | -6.3 pts | +2.0, -6.3, -8.4, -6.8, +1.6 | 29.9 / 29.9 |
| All on | +6.9 pts | +8.4, -2.4, -3.3, +6.9, +7.1 | 29.8 / 29.8 |

The spread between pairs (up to +-20 points) is far larger than any toggle's effect, so the
honest reading is: each toggle's cost is below the noise floor of this setup. Even at
5120x3840 every configuration held the game's 30 fps in both phases, and an earlier run at
2880p with uncapped interpolation stayed at 60 fps with and without the toggles. Expected
costs from the implementation: integer TEV and EFB depth are a few extra ALU ops per pixel
(no extra passes; changing either rebuilds pipelines once), the copy filter is one extra
full-screen pass with three texture taps.

Remaining: blended draws are not quantized after blending; copy filter not applied to texture
copies (GXCopyTex); whether hardware normalizes transformed normals; fog range LUT with
widescreen is untested (LM never enables fog).
