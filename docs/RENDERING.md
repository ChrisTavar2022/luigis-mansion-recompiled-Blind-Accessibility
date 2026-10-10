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

*Update (EFB copies to RAM, below):* the Z16 copy is the game's shadow map (section "Shadows"), read only as an
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
| shadow buffer (heap, 0x96000) | Z16 640x480, every frame | **IA8 640x480** texture (TEV `GR16` compare against a 256x256 IA8 depth ramp: the flashlight/lightning shadow map, see "Shadows") | `fn_8005D8CC` copy, `fn_8005D41C` texture, `fn_8005DD50` TEV setup |
| `lbl_804D86BC` (0x8021E000) | RGB565 640x480, twice per frame | RGB565 same size | `fn_8005F518` / `fn_8005F8B8` |
| `lbl_804D86B8` (0x802B4000) | RGBA8 320x240 (box-filtered) | RGBA8 same size | `fn_8005F498` |
| 0x803A4D00 | RGB565, box-filtered half size of a screen region (70x100 from (250,140 140x200) in the foyer, 134x114 from (186,126 268x228) in the 2F Bathroom) | same | `fn_80025C98` |
| transition captures | RGB565 640x480 | same | `fn_800081A8` (LMDisplayUtil) |
| title/file select | 4 x RGB565 148x116 | same | `fn_800494E4` |
| event 5/6 render targets (heap, +0x28 of the objects at `r13+0x768`/`+0x76C`) | IA4 (event 5) / RGB5A3 (event 6) 128x128, every frame while the event runs | same, warped by the indirect stage (Mario painting; see "Indirect effects") | `fn_8006033C` |

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
   - The shadow map is the case that takes this path in LM: the Z16 copy is read as IA8 (unless
     "Shadow map at render resolution" is on, see "Shadows").
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
`LM_ACCURACY=<mask>` (1 integer TEV, 2 EFB color depth, 4 texture copy filter) and `LM_ACCURACY_AB=<s>` (alternate
configured/all-off every s seconds and log fps per phase, for A/B screenshots of one run).

| Area | What LM uses | Aurora before | Status |
|------|--------------|---------------|--------|
| EFB pixel format | `GXSetPixelFmt(RGBA6_Z24)`, dither toggled per draw | 8-bit target, no dither | **Fixed** (toggle "EFB color depth", on): output quantized to 6 bits/channel with a 2x2 ordered dither when the draw has dither on, expanded back to 8 bits; blended draws quantize color only (alpha stays the blend factor; the blend itself is still 8-bit). Verified A/B in the lab: mean -0.9 levels; a dark area goes from ~98 to ~25 distinct levels (dither grain/banding as on hardware) |
| TEV arithmetic | ADD/SUB, unclamped regs, scale x2, GR16 compare, K0-K3 selects, swap tables, up to 8 stages, `GXSetTevColorS10` | float lerp, [-4,4] clamp, k/8 konsts | **Fixed** (toggle "Integer-accurate TEV", on): 8-bit A/B/C (low byte of a register), s11 D/registers, C widened by C>>7, per-stage rounding, clamp 0..255 / -1024..1023, konst k/8 = k*32, HALF = 128, 8-bit vertex colors, rounded indirect texels, 8-bit fog blend. Verified: lab scene changes ~50% of pixels by exactly 1 level, unbiased, no breakage. SUB/DIVIDE_2 rounding is a model (+-1 LSB) |
| Display copy | `GXSetCopyFilter(aa=0, vf=1, {6,0,20,12,20,0,6})`, gamma 1.0, clamp top+bottom, Y scale 1.0, 640x480 | GXSetCopyFilter/Gamma no-ops; YScale returned 0 | **Fixed** (toggle "Display copy filter", on): present-time pass with the game's taps (0-1 line above, 2-4 this line, 5-6 line below = 6/52/6 of 64) in EFB lines (scales with resolution), edge clamp, gamma 1.0/1.7/2.2, before the CRT filter. `GXSetDispCopyYScale` returns the XFB line count. Texture copies: see the next row |
| Texture copies (GXCopyTex) | the same taps are set while the game copies the scene (RGB565 640x480, RGBA8 320x240 box-filtered, region and transition copies) | copied without the filter | **Fixed** (patch 0018, toggle "Texture copy filter", on; see "Copy filter on texture copies") |
| Early Z + alpha test | `GXSetZCompLoc(1)` with alpha compare GREATER 0 and Z update | Z written only for passing pixels | **Verified, unchanged**: emulating "Z written before the alpha test" (depth prepass) broke Luigi's face decals and pillars, so the hardware evidently does not write Z for alpha-rejected pixels here. Experiment removed |
| Fog | only `GX_FOG_NONE` seen; range adjust enabled (center 0), `GXInitFogAdjTable` once | range factor from a guessed NDC formula (much weaker than the table) | **Fixed model**: LUT built from the table (entry i = 256x factor at 32(i+1) EFB px from center, interpolated, 1.0 at center, last entry beyond), in EFB pixels at any resolution. Not visible in tested scenes (fog off) |
| Texture filtering | min LINEAR/NEAR, edge LOD, no mips | hw min-filter decode had NEAR_MIP_LIN/LIN_MIP_NEAR swapped | **Fixed** decode table (raw-BP texture setups). LOD bias/edge LOD fine |
| Lighting | spot (clamp diffuse), specular (shininess form), dist-attn fns, SRTG from COLOR0 | per-vertex, same formulas | **Reviewed, no change** (SRTG lit-color fix is in 0008). Open: Aurora normalizes transformed normals; hardware may not |
| Indirect | Only the Mario painting at the Secret Altar (`fn_8005FDD4`, event 6) and an invisible copy of it in event 5; J3D materials on the disc have no indirect stages | float offsets | **Fixed** (patch 0014, see "Indirect effects"): integer offsets (s1.10 x s8 >> 3, scale shift, 1/128-texel fixed-point coordinates) in integer TEV mode; ITF_5/4/3 low-bit offsets and positive-modulo wraps always. Offsets are in logical texels, so they scale with resolution |
| Lines/points | width 6 (1 px), viewport jitter field=1 only | width/6, perpendicular expansion | No change needed for LM (hardware widens along x/y; noted) |
| Logic ops, dst alpha | no logic op; dst alpha used | fine | No change |

### Toggles (F1 > Visuals > Rendering, all on by default, persisted in settings.ini `[rendering]`)

| Toggle (ini key) | What it does | Visible effect | Cost (measured below) |
|---|---|---|---|
| Integer-accurate TEV (`integer_tev`) | TEV stages evaluated in integer WGSL (8-bit A/B/C, s11 D/registers, per-stage rounding/clamp) | +-1 level on ~40-45% of pixels, unbiased (mean +0.06); nothing structural | see table |
| EFB color depth (`efb_color_depth`) | fragment output quantized to the EFB format (RGBA6 / RGB565) with the PE's 2x2 dither when the draw enables it; texture copies to RGB565/RGB5A3 keep their format's precision (patch 0014) | ~1.4 levels darker on average (LM draws much of the scene with dither off, where the PE truncates), 6-bit banding rings in smooth gradients (flashlight pool on the floor), dither grain where dither is on | see table |
| Display copy filter (`display_copy_filter`) | present-time pass (`dcopy_filter` in crt.cpp, ahead of the CRT chain) with the game's GXSetCopyFilter taps + GXSetDispCopyGamma | ~5% less vertical high-frequency detail (6/52/6 of 64), no color shift (mean 0.2 levels); only runs when the taps/gamma change the picture | see table |
| Texture copy filter (`texture_copy_filter`) | GXSetCopyFilter taps applied to color GXCopyTex copies (patch 0018, `GX_AURORA_ACCURACY_COPY_FILTER`) | the scene the game redraws from its RGB565 copy is softer vertically (nursery: vertical gradient energy -30%, horizontal -6%; differences only along horizontal edges, no shift) | a copy that was a plain texture copy becomes a shader blit with three taps |

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

Remaining: blended draws are not quantized after blending; whether hardware normalizes
transformed normals; fog range LUT with widescreen is untested (LM never enables fog).

### Copy filter on texture copies (Aurora patch 0018)

The console applies the vertical copy filter (BP 0x53/0x54, set by `GXSetCopyFilter`) to every
EFB copy, texture copies included. LM sets one filter for the whole game, `{6,0,20,12,20,0,6}`
(`LM_GX_AUDIT=1`: the only GXSetCopyFilter combination from boot to the nursery), so its scene
copy (`fn_8005F8B8`, RGB565, redrawn as the frame), the 320x240 RGBA8 copy, the half-size region
copies and the transition captures are all filtered on hardware. Ported from the SMS port (its
patch 0102), with LM specifics:

- `GXSetCopyFilter` writes BP 0x53/0x54 (pass-through taps `{0,0,21,22,21,0,0}` when vf is off),
  so the state also survives capture/replay (frame interpolation); `bp_copy_filter` keeps the
  taps in `GXState::copyVFilter`.
- `resolve_pass_into` packs the weights of the line above (taps 0+1), the line (2+3+4) and the
  line below (5+6) into `tex_copy_conv::Uniforms::filterTaps` (0 = off: pass-through taps, depth
  copies, or the toggle off) plus one EFB line in uv (`1 / logical EFB height`, so it scales with
  the render resolution). `sample_efb` applies it to RGB before the format conversion and the
  5/6/5 quantization; alpha comes from the line itself. Filtered copies that would have been a
  plain `CopyTextureToTexture` take the blit shader instead (`RenderPass::resolveFiltered`).
- Depth copies (the Z16 shadow map) are never filtered.
- Box-filtered (mip) copies sample with LINEAR at 2:1, so the centre and neighbour taps each
  average two rows: the result equals the console's order (filter each line, then average 2x2).
- `LM_FRAME_DUMP` sets pass-through taps around its dump copy (the dump stays the raw EFB) and
  restores the game's taps.
- Toggle F1 > Visuals > Rendering > "Texture copy filter" (`texture_copy_filter`, on),
  `GX_AURORA_ACCURACY_COPY_FILTER` (`LM_ACCURACY` bit 2).

Verification (US, `LM_FRAME_DUMP` with `LM_ACCURACY=3` vs `7`): title frame mean |d| 0.5
(max 4); in the nursery the static background differs only along horizontal edges (vertical
gradient energy -30%, horizontal -6%: the 6/52/6 vertical blur and less vertical dither), no
shift, no seams at copy edges. Not compared with a console capture.

## Indirect effects

Aurora patch `0014-indirect-and-copy-format-accuracy.patch`; logging and test aids in
`src/recomp/hle_gx.cpp` (`LM_IND_*`).

### What uses indirect texturing (US; the European game has the same code)

- **Static search.** In the DOL, the GX indirect calls (`GXSetIndTexOrder/CoordScale/Mtx`,
  `GXSetNumIndStages`, `GXSetTevIndirect/IndWarp`) are made only from `fn_8005FDD4`, from
  `GXInit`/`GXSetTevDirect`, and from J3D (`J3DSys::drawInit` resets, `J3DInd*::load`). None of the
  J3D models on the disc (`Effect/*.bmd` are MAT2, the rest are in `game*.szp`, `Iwamoto/vr*.szp` and
  `Map/map*.szp`) has an indirect material block, so `fn_8005FDD4` is the only indirect effect.
- **Runtime sweep** (`LM_IND_LOG=1` + `AURORA_GX_AUDIT=1`, which also reports indirect state that
  arrives as raw BP writes in display lists): boot, title, file select, foyer, every map 0-13 by warp
  (with and without entry events: Bowser, Chauncey, Boolossus, Bogmire, map 12), the mansion appear
  points 3/5/7/9 (Balcony Boos, Sealed Room, King Boo at the Secret Altar), the lab, Portrificationizer,
  the fire and ice elements with L/R held in the foyer and nursery, the PAL Hidden Mansion
  (`LM_HM_FORCE=1`: foyer, appear point 3, map 12). Indirect draws only appear in
  events 5 and 6 (below). Ghosts, Boos, the vacuum vortex, elements, water, the GBH, mirrors and
  screen transitions do not use indirect texturing (the mirrors and transitions are EFB copies, below).

### The effect: `fn_8005FDD4` (warped render-to-texture)

What starts it is not a per-map table: `0x803002C4`, `0x80300478` and `0x8030062C` are three parallel
arrays of 109 function pointers (init, per-frame update, end) indexed by **event number** (followed by
the string `/Event/event%02d.szp`). Two events use the effect:

| Event | Where | Init / end | What is rendered |
|---|---|---|---|
| 6 | Secret Altar, King Boo's talk (mansion appear point 9, trigger at 2313,-550,-6457) | `fn_8002CD48` / `fn_8002CEB0` | two S3D models (`fn_800605BC`) into two 128x128 **RGB5A3** copies, cross-faded (`r13-0x7758` and 255 minus it): **Mario in the painting** |
| 5 | 2F Bathroom (trigger at -1649,550,-4889, character `fat`, flag 38) | `fn_8002CC8C` / `fn_8002CD14` | the model of the actor of type 0x5F into one 128x128 **IA4** copy |

`event05.szp`/`event06.szp` are empty placeholder scripts; the effect comes only from these code
slots. Each frame `fn_8006033C` renders the model with its own perspective into a 128x128 viewport
and `GXCopyTex`es it to the object (+0x28, format from +0x00); `fn_80060A9C` (scene draw,
`fn_8000BB98`) then calls `fn_8005FDD4(texObj, alpha)` for each object:

- TEXMAP0 = the 128x128 copy; TEXMAP1 = a 64x64 IA4 texture at `0x803CFD40` that `fn_8005FD80`
  (from `BootScene::init`) fills with random intensities 0-15 (alpha 0);
- indirect stage 0: texcoord 1 (vertex TEX1, identity), scale 1; TEV stage 0
  `GXSetTevIndWarp(signed, no replace, ITM_0)` = ITF_8, bias STU, no wrap;
- matrix `[[f f 0] [f f 0]]`, exponent 1, with `f` = `r13+0x760` = 0.01 (s1.10: 10/1024), so
  offset_s = offset_t = 2 x 10/1024 x (I - 256) texels, -5..0 texels of the 128x128 copy;
- colour = the copy, alpha = copy alpha x `alpha`, blended over the scene; a world-space quad at
  fixed coordinates (`0x80339B80`: x 2220..2375, y -180..-375, z -7306, the painting frame at the
  Secret Altar), s 0.1..0.9, t 0..1. Event 5 draws the same quad there, so nothing of it is visible
  in the bathroom; its IA4 copy is also an extra TEV stage of the draw in `fn_80027D28`
  (`fn_80027B30`, reached from `fn_80011824`).

**Texcoord 1 is beyond the texgen count** (Aurora patch 0015). `fn_8005FDD4` sets a texgen for
texcoord 1 (vertex TEX1, identity) but never calls `GXSetNumTexGens`; the count it inherits is 1 (from
`fn_80025E9C`'s draw just before, in both versions). Aurora, like the usual emulator convention, read
such a coordinate as (0,0): every pixel read noise texel (0,0) (intensity 51 at the boot seed), so the
painting was only shifted by a uniform -4 texels and Mario looked clean.

A real-console capture of the scene (King Boo's talk, "Get me outta heeeeere!") shows Mario
scattered over the whole painting, with the grain of the 64x64 noise texture across the painting
(about 1/64 of its width per cell, white speckles in the cap emblem, a broken-up moustache) and
the displacement of a few texels. So the console produces a varying coordinate 1, not a constant.
Candidates and what they predict:

| Rule for a coordinate >= numTexGens | Painting |
|---|---|
| (0,0) / any constant (stale per-pixel value) | uniform shift, clean Mario: contradicted by the capture |
| stale rasterizer setup from an earlier primitive | depends on whatever was drawn before; would change with the camera and Mario's animation and show a seam between the quad's two triangles: not what the capture shows |
| generated from its texgen registers as if counted (TEX1 here, 0..1 over the quad) | 64 x 64 noise cells over the painting: matches |
| a copy of texcoord 0 (s 0.1..0.9) | 51 x 64 cells: also close; the capture is too soft to separate it from the previous rule |

Patch 0015 implements the third rule (`GX_AURORA_TEXGEN_BEYOND_GENERATE`, the default): a
coordinate a TEV stage samples, or an indirect stage reads, at or beyond `numTexGens` is generated
from its texgen registers when they are a matrix texgen whose source the vertex provides (TEX0-7,
POS, NRM); anything else keeps (0,0). It changes only draws that read such a coordinate:
`LM_TEXGEN_LOG=1` (host side) and Aurora's one-time "texcoord N read beyond numTexGens" log found
exactly the painting's two draws (texcoord 1, numTexGens 1, source TEX1) through boot, title, file
select, foyer, nursery with the vacuum and GBH screens, and the Secret Altar; two identical nursery
runs with the old and new rule differ no more than two runs with the same rule. The magnitude
then follows from the game's own data with no tuning: the 0.01 matrix x 2 gives 0..-5 texels of the
128x128 copy (both axes, so the smear is diagonal), and the noise texture and copy are sampled the
way the game set them up (noise point-sampled, 64x64 repeat; copy bilinear, clamp). At 4x the copy
is rendered at 512x512 and the offsets stay in its 128-texel units, so the picture is the same,
only sharper (image 7). `LM_TEXGEN_BEYOND=0|1|2` switches between (0,0), the default and
texcoord 0 for comparison (image 8).

### Hardware semantics vs Aurora (patch 0014)

| | Hardware | Aurora before | Now |
|---|---|---|---|
| Matrix | s1.10 entries (`GXSetIndTexMtx` truncates x1024), exponent 2^(n) | floats of the same entries | unchanged (exact) |
| Offset | integer `M x (s,t,u)` (s8 after bias) >> 3 = texels with 7 fractional bits, then the scale shift (right shifts floor) | float product x 2^n | integer TEV mode: the same integer steps (`floor(floor(raw/8) x 2^n)/128`); float mode unchanged |
| Base coordinate | fixed point, texel units, 7 fractional bits | float | integer TEV mode: floor to 1/128 texel; dynamic S/T matrices likewise (`uv128 x s >> 8`) |
| ITF_5/4/3 | offset = low 5/4/3 bits, high bits = bump alpha | high bits (`floor(v/8)`...) | low bits (matches Aurora's own bump-alpha masks) |
| Wrap 16..256 | masks the fixed-point coordinate (positive modulo) | WGSL `%` (negative stays negative) | `x - m floor(x/m)` |
| Units | texels of the sampled texture | texels of the game's texture object (`GXTexObj` size, not the upscaled copy) | unchanged: offsets keep their size at any internal resolution and aspect |
| Indirect texel | 8-bit components (.abg = s,t,u) | rounded in integer mode (0010) | unchanged |
| Texcoord beyond numTexGens | not documented; the console capture shows it varying (see above) | (0,0) (patch 0001) | generated from its texgen registers (patch 0015) |

Not changed (LM never uses them): `indLOD` (LOD from the unmodified coordinates), add-previous after
a direct stage (Aurora does not carry a direct stage's coordinate into `t_TexCoord`), wrap/add-prev on
stages whose indirect stage index is >= `numIndStages`.

For the painting the integer path changes each offset by at most a few 1/128 texel (e.g. -4.016
instead of -4.004 texels); no visible change. The 1x/4x and 16:9 checks below are the real verification of the scaling.

### EFB copies met along the way

- The painting copies (RGB5A3) were blitted without the format's precision; IA4 copies (event 5)
  were already quantized. Patch 0014 adds an RGB5A3 conversion (alpha >= 0xE0: RGB555 opaque,
  otherwise RGB444 + 3-bit alpha, channels expanded by bit replication) and 5/6/5 precision for
  RGB565 copies, under the "EFB color depth" toggle (`GX_AURORA_ACCURACY_EFB_DEPTH`, default on; off
  keeps 8 bits). The painting copies hold only opaque and alpha-0 texels (`LM_IND_LOG` with
  `LM_EFB_READBACK_ALL=1` counts them: about 9300 opaque, 7050 alpha 0, 0 translucent).
- RGB565 copies in LM: the full-screen scene copy that `fn_8005F8B8` redraws every frame (the PAL
  Hidden Mansion mirror flips it), the box-filtered half-size region copy at `0x803A4D00`
  (`fn_80025C98`; 268x228 -> 134x114 in the 2F Bathroom), transition captures (`fn_800081A8`) and
  the title/file-select thumbnails. Since the scene copy is redrawn, every frame now has 5-bit red
  and blue as on the console: mean |d| ~1 level per channel, at most 8-21 levels where the copy
  filter and blending meet a 5-bit step (image 6); no structural change.
- The upscaled fast path keeps copies at render resolution; a box-filtered (mip) copy averages the
  matching render pixels.

### Verification (US, `LM_FRAME_DUMP` native frames, `LM_INPUT_CLOCK=frames`)

Images in `build/indirect_fx/` (not in git):
1. `1_painting_warp_off_on_diff.png`: `LM_IND_OFF=1` / default / |difference| x4, same frame of
   two deterministic runs; only the painting differs (Mario 4 texels right/down, top edge rows).
2. `2_exaggerated_warp_1x_vs_4x.png`: `LM_IND_EXP=3` (32 texels) at internal resolution 1x and 4x,
   same frame: identical displacement; `2b_..._16x9.png` the same at 4x 16:9.
3. (replaced by 7 and 8)
4. `4_painting_before_after_diffx16.png`, 6. `6_copy_precision_before_after_diffx16.png`: main build
   (before) vs patch 0014, same frame: RGB5A3/RGB565 precision only (difference x16).
5. `5_event5_bathroom_mirror.png`: event 5 running in the bathroom (no visible warp).
7. `7_console_vs_port_1x_vs_4x.png`: the console capture (left; a low-resolution video capture,
   so softer) next to the port at internal resolution 1x and 4x, same frame (window captures
   `cap_1x.png`, `cap_4x.png`). Images 1, 2 and 4 predate patch 0015 (painting without scatter).
8. `8_console_vs_rules_zero_generate_tc0_generate4x.png`: console, then `LM_TEXGEN_BEYOND` 0, 1,
   2 and 1 at 4x (native frame dumps, same frame).

### Test aids

- `LM_IND_LOG=1`: each frame's draws with indirect stages, grouped by guest call chain and state
  (indirect/TEV stages, texmaps with image address/size/format and the first indirect texture bytes,
  texgen matrix, indirect matrices, `numTexGens`, RGB5A3 texel classes), printed when a frame's set
  changes. Combine with `AURORA_GX_AUDIT=1` for state from display lists.
- `LM_IND_OFF=1` (indirect never enabled), `LM_IND_EXP=<n>` (matrix exponent + n),
  `LM_IND_PATTERN=1` (left half of the indirect texture intensity 15, right half 0),
  `LM_TEXGEN_BEYOND=0|1|2` (texcoords beyond numTexGens: (0,0) / generated (default) /
  texcoord 0), `LM_TEXGEN_LOG=1` (each draw that reads a texcoord beyond numTexGens, once).
- Reaching the effects: event 6 `LM_DEV_WARPS=2:9@2800` with `[debug] warp_skip_events=0` in
  settings.ini; event 5 `LM_TELEPORT=-1648.7,550,-4889,200` from the foyer of a new game.

## Shadows

**Symptom (user).** No real-time shadows anywhere: the flashlight cast no shadows of furniture,
bears or Luigi onto walls and floors, and lightning flashes lit rooms without the big shadows
they throw. In Dolphin with a widescreen hack, shadows stop at the 4:3 area.

### How the game does it (US addresses; one technique for every dynamic shadow)

All real-time shadows come from **one depth shadow map** per frame, rendered from the current
light and compared per pixel in the TEV. Code in `auto_03_8005D3A4` (shadow object
`lbl_803CFC0C`, functions `fn_8005D41C`..`fn_8005DF84`):

1. **Light selection** (`fn_80029050`/`fn_80029488`, `fn_800293A8`): normally the flashlight
   (eye/target from Luigi's hand, fovY/near/far into the camera); during a **lightning strike**
   the room's thunder light. The room update `fn_800168EC` starts a strike when the room info's
   `Thunder` field (`fn_8002D7C8`) is non-zero and a timer (`fn_8000FC60`) passes 1.87
   (`lbl_804D9B30`); `fn_80029250` then switches the light for 60 frames and sets the shadow
   strength to 150 (K3 alpha). Cutscenes (`fn_80114144`) can set the light from keyframes
   (`fn_80116460`).
2. **Caster pass** (`fn_8005EF84`, every gameplay frame while the shadow is enabled):
   `fn_8005D984` turns the main camera `lbl_803A3820` into the light's camera (perspective
   `MTXPerspective(fovY +0xA0, aspect +0x94, near, far)`, or orthographic), viewport 640x480,
   scissor (1,1)-(639,479), sets `lbl_804D8080 = 1`, and builds the receivers' matrices:
   `+0xB0` = `MTXLightPerspective` (y flipped) x light view, `+0xE0` = a depth-ramp matrix whose
   s/q is the light-space window depth and t = 256 s (`fn_8005D7B8`). Furniture, actors and Luigi
   are then drawn into the EFB from the light (`fn_80011824`, `fn_80060D44`, `fn_801588C8`,
   `fn_80072230`).
3. **Copy** (`fn_8005DBE4` -> `fn_8005D8CC`): `GXCopyTex` **Z16** 640x480 with clear (color 0,
   Z = far) into the shadow buffer, camera restored, `lbl_804D8080 = 0`. The buffer is read as an
   **IA8** 640x480 texture (`fn_8005D41C`, clamp, LINEAR): I = depth bits 23..16, A = 15..8.
4. **Receivers** (rooms `fn_80015CF4`/`fn_8001566C`, actors `fn_80060E70`, Luigi/J3D
   `fn_8005A550`, cutscene objects `fn_801142C4`/`fn_801144C4`): `fn_8005DD50` (plus
   `fn_8005DCA0` for room geometry, `fn_80058A60` adds per-vertex `TEX0/1MTXIDX` for skinned
   models) sets
   - texcoord 0 = POS x (ramp matrix) -> 256x256 IA8 **depth ramp LUT** (`fn_8005D560`: texel
     (x,y) = A x, I y; s clamped, t repeating), texcoord 1 = POS x (light projection) -> the
     shadow map; both projective 3x4 (dual-tex post matrices PT18/PT19 for actors);
   - TEV stage 0: PREV = ramp texel through swap table 1 (R = I, G = A);
   - TEV stage 1: shadow map through swap table 2 (R = A, G = I); **alpha op `GR16_GT`**:
     REG0.a = (PREV.GR16 > TEX.GR16) ? K3.a : 0, i.e. "receiver is deeper than the map" ->
     shadow strength (0xA4, 0x96 for lightning);
   - the material's own stages follow; two final stages (`fn_800154E4` for rooms) multiply the
     strength by the light's **ALPHA1** channel (lit only by the shadow light, spot attenuation)
     and lerp the color toward K3 (near black): shadows only appear where that light shines.
   Other "shadow" assets (`/model/shadow.szp`, `bshadow*.szp`, `gshadow.bmd`, `kage_*.mdl`) are
   ordinary ghost/boss models drawn as regular geometry; no other projection or stencil
   technique exists.

### Root causes and fixes

| # | Cause | Effect | Fix |
|---|-------|--------|-----|
| 1 | **TEV color and alpha units did not read their inputs simultaneously.** Aurora wrote the stage's color result to its register before generating the alpha op; the shadow stage's alpha `GR16_GT` compares the *color* inputs A = PREV and B = TEXC, and the color op of that same stage had just overwritten PREV with 0 (its own compare result). The alpha compare saw 0 > map and was never true. Same bug in the integer and float TEV paths. | No shadow anywhere (flashlight, lightning) | Patch 0011 (`shader.cpp`): both units compute into temporaries (`tevcN`, `tevaN`) and store afterwards, as on hardware. Verified with a shader debug view: compare result now equals (ramp key > map key) on every pixel. |
| 2 | **Widescreen/FOV projection applied to the light pass.** The host GX layer scales every full-viewport perspective projection (Hor+, FOV slider; orthographic: centered 4:3). The light's view got squeezed while the receivers projected the map with the game's unmodified matrices. | Shadows misplaced/missing at 16:9+ | `hle_gx.cpp` `drawMode`: while the game's shadow-pass flag `lbl_804D8080` is set, projections stay plain. |
| 3 | **The light frustum only covers the 4:3 picture** (game design; the cause of the Dolphin widescreen-hack symptom). | No shadows near the left/right edges at 16:9..32:9 (e.g. nursery table/lamp in a lightning flash at 32:9) | `shadows.cpp` hooks `fn_8005D984` (US + PAL): during the setup call the camera aspect (+0x94) is divided by the widescreen factor, so the light projection, the receivers' `MTXLightPerspective` and the light's culling planes are all built from the same wider frustum by the game's own code (consistent by construction; 4:3 output unchanged pixel for pixel). `ws::setShadowLightSetup` keeps the view's Hor+/FOV culling adjustment off the light camera. Toggle **Widescreen shadows** (`shadow_widescreen`, on). |
| 4 | **Shadow-map resolution.** The Z16 copy is read as IA8, so EFB-copies-to-RAM decoded it from the console bytes at 640x480 even at high internal resolutions (blocky, stair-stepped shadow edges and coarse self-shadow stripes). | Low-res shadows at 2x-8x | `efb_ram.cpp`: a Z16 copy read as IA8 of the same size/address uses Aurora's copy texture at the internal resolution (it holds exactly the IA8 view: I = bits 23..16, A = 15..8). Toggle **Shadow map at render resolution** (`shadow_render_resolution`, on; off = console 640x480 bytes). Without "EFB copies to RAM" the map always follows the render resolution. |
| 5 | **MSAA crashed** at the first depth copy ("Depth tex copies from multisampled EFB targets are not supported"; the game copies Z16 every frame, so any MSAA setting aborted at boot). | Crash with MSAA | `tex_copy_conv.cpp`/`encoding.cpp`: depth-copy pipelines for multisampled depth (sample 0). |

Not causes (checked): projective texgen with post-transform matrices and per-vertex texture
matrix indices, the q divide, swap tables, the ALPHA1 spot channel, the LUT/shadow-map texture
decode and the Z16 copy byte order (I = high byte, matches the game's swap/compare setup),
interpolated frame rates (the shadow pass is replayed with the rest of the frame; shadows match
Original 30), the copy-to-RAM decode path.

Accuracy notes: the game samples both packed textures (LUT and map) with LINEAR filtering,
which mixes the high/low depth bytes independently; the faint diagonal stripes this causes on
self-shadowed curved surfaces (the nursery bears) are part of the hardware technique and become
finer at higher shadow-map resolutions. Aurora filters in float and the integer TEV rounds the
result to 8 bits (hardware: fixed-point weights), a +-1 LSB difference in the low byte.

### Test aid: `LM_LIGHTNING=<first>[,<period>[,<type>]]`

`src/recomp/lightning_test.cpp` (US): counts room updates (`fn_800168EC`, one per gameplay
frame) and forces a strike on update `<first>` and every `<period>` after it (lowers the timer
threshold for that call); with `<type>` a room without thunder uses that thunder type. Logged as
`[lightning] forcing a strike at room-update call N`. Example (nursery, flashes every ~2 s):
`LM_LIGHTNING=0,60,1 LM_TEST_STATE=poltergust LM_START_APPEAR=1 LM_INPUT_CLOCK=frames`.

### Verification (US unless noted; frame dumps via `LM_FRAME_DUMP`, window captures)

- Nursery, flashlight on the crib (frame 2950 of the deterministic nursery script): before, the
  lit wall has no shadows; after, the bears, crib bars and Luigi's head cast shadows onto the
  wall and the bears self-shadow. Forced lightning: Luigi, crib and bears throw long shadows
  across floor and walls.
- 4:3 / 16:9 / 21:9 / 32:9 lightning frames: the shadows in the 4:3 area are identical; at 32:9
  the lamp, vase and chair at the right edge now cast shadows (missing without the widened light).
  Flashlight at 21:9: shadows only with fixes 1+2.
- 4x internal resolution: render-resolution map vs 640x480 map (window captures).
- Original 30 vs Interpolated 60 (2x): same lightning shadows. MSAA 4x: shadows as without MSAA.
- PAL build (GLMP01) at 21:9: flashlight shadows in the nursery (hooks and `VA()` addresses map).

Cost: fix 1 none; fix 2/3 none (a different projection/aspect); render-resolution shadow map
saves the per-frame 640x480 encode+decode (a full-resolution IA8 sample instead); MSAA depth
copy: one extra full-screen pass only with MSAA.

Remaining: the orthographic shadow light (fixed extents in `fn_800085D8`) is not widened (not
seen in tested scenes); no reference capture from real hardware to compare shadow darkness or the
LINEAR-filter stripe pattern exactly; "Shadow map at render resolution" off still uses the scaled
copy when "EFB copies to RAM" is off.

### Shadows at any distance (enhancement) and the room shadow light

**Original behaviour.** Which light casts the shadow map is chosen per frame by `fn_80029050`:
in a dark room (room light object `lbl_803A3CA0` +0x24, dark flag +0xAC set, mode +0x94 == 5)
it is the flashlight (its frustum is the beam: shadows only show where it shines). In a lit room
(after it is cleared, hallways, the training room) and during lightning it is a light that
follows Luigi (`fn_80028CF0`/`fn_80028B74`: target 300 above Luigi, eye at a table distance
toward the upper part of the room, `fn_80029488`) with fov/near/far from the light table
(`fn_800293A8`; e.g. fov 17, near 3000, far 5000 in the 1F rooms and hallways; lightning fov 31,
near 619, far 2322). Shadow strength is K3 alpha: 0x5C lit rooms, 0x96 lightning, 0 when the
light object's +0x89 flag disables shadows. Only what lies inside that small frustum casts and
receives shadows, so in big rooms the shadows of objects away from Luigi (the ballroom's chairs
and curtains at the back wall) are missing and appear/disappear as he walks: the game's own
limit, not a port bug. (`LM_SHADOW_LOG=1` logs the light camera and strength.)

**Enhancement** (`src/recomp/shadows.cpp`, hook on `fn_8005D984`; F1 > Visuals > Rendering
"Shadows at any distance", `[rendering] shadow_any_distance`, on by default; off leaves every
value as the game set it). For the gameplay room/lightning light (detected through
`fn_800293A8`; the flashlight and cutscene lights are untouched) the light keeps its position
and direction, and its fov, aspect and near/far are refitted so the frustum contains its own
frustum plus the receivers that matter: the player's view frustum sampled from 1/4 to 4x the
camera-target distance, **clamped to the current room's bounds** (room table `lbl_804D8170`,
0x30 per room, int min/max xyz at +0x8/+0x14), so the map is no wider than the visible part of
the room (texel size stays close to the original; near is never below 1/4 of the original for
Z16 precision). The game then builds the light projection, the receivers' light/ramp texture
matrices (`+0xB0`/`+0xE0`, used by `fn_8005DCA0`/`fn_8005DB90`) and the light's culling from those
fields itself, so the map and the TEV compare stay consistent.

Why no cascades/second map: the case that looked uncovered (foyer, light "near the camera")
turned out to be the flashlight's beam frustum (dark foyer) and the lightning light, both
handled; in every lit room tested the room light sits far out in front/above (about 3000-4000
units), so one perspective map fitted to the room covers everything visible.

**Test aid** `LM_ROOMS_LIT=1` (US): rooms behave as lit for the shadow light choice (the dark
flag is cleared for the `fn_80029050` call; ambient and lamps are not changed). With
`LM_TELEPORT=x,y,z,frames` (room centres from the room table: ballroom 1800,0,-1400; dining room
-1600,0,-1400; 1F hallway -2400,0,-985; foyer 0,0,200) and a scripted walk.

**Verification** (16:9, frame dumps, `build/shadows2/`): a temporary coverage overlay (green =
inside the shadow map, red = outside) in the lit ballroom shows the back wall, curtains and chairs
outside the map with the option off and the whole room inside with it on (`cov_rooms.png`
rows 1-2, `cov2.png`); the dining room has a small uncovered sliver off, none on; the lit foyer
and the 1F hallway are fully covered either way (`hcov.png`). Rendered frames: ballroom chairs at
the back gain their shadows, the long shadows near Luigi are unchanged except edge pixels
(`rooms_onoff2.png`: off / on / difference, red = darker with the option on);
`ballroom_walk_off_on.png` is a walk through the room (left off, right on).

### Shadow polish (shadows-3)

Checked in the nursery (flashlight, `LM_LIGHTNING`), the ballroom (dancing ghosts, ghost lights,
lightning), lit rooms (`LM_ROOMS_LIT`: ballroom, dining room, foyer, 1F hallway), with enhanced
lighting off and Ultra, 30/60/120 fps, render scale 1x/3x, 16:9/21:9, MSAA 4x and the 2-player
mode (`LM_COOP=1`); images in `build/shadows3/`.

- **Interpolation**: `LM_INTERP_TEST=4` at 60 and 120 fps through flashlight sweeps and the
  flashlight -> lightning switch: every late replay is identical to its 30 Hz frame (0 stale
  pixels), the in-between steps show the shadow map and its receivers consistently (the caster
  pass is part of the same tape as the receivers), no popping spikes in the frame-to-frame change
  (`interp60_lightning_switch.png`). The light switch itself snaps from one light to the other, as
  in the original.
- **MSAA 4x, 21:9, 2-player split screen** with Ultra: shadows drawn, no errors
  (`matrix_base_msaa_21x9_coop.png`).
- **Shadow look** (user report: ghost-light shadows overpower the colored lighting, near-black
  blocks): see "Shadow look" in the parts table. Ballroom and nursery before/after:
  `ballroom_shadowlook_old_new.png`, `ballroom_ghostlights_old_new.png`,
  `nursery_shadowlook_old_new.png` (left 1.0/0 = previous look, right defaults 0.9/0.25): the
  ghosts' orange/white light now shows inside shadows, shadows are a little lighter, the rest of
  the picture is unchanged. The exact close-up in the user's screenshot (camera next to the
  dancers) was not reproduced with the test aids; it most likely comes from the ballroom's
  cutscene camera, where the shadow light is the scripted light of the cutscene.
- **Any-distance fit stability**: the refitted room-light frustum is snapped to 6% steps in
  field of view and 10% in depth (rounded outward), so the shadow map's texel size and depth
  scale do not change every frame with the camera (edge shimmer); the light itself still follows
  Luigi as in the original.
- **Build**: CMake's Aurora patch re-apply now drops the submodule index before checking files
  out again: `checkout-index -f` left stat-clean CRLF files from an older autocrlf checkout
  untouched and every patch then failed to apply.

Not changed / remaining: shadows on translucent ghost bodies are the original behaviour (the
actor pass gives ghosts the shadow stages too); the jagged edge of a grazing light's shadow is
the shadow map's own resolution along the receiver (raise "Shadow map size"); the 2-player mode's
second view uses the shadow map fitted to player 1's view.

### Stable shadows with many lights (shadows-4, ballroom dance)

User report: in the ballroom dance (ghost lights, Ultra) shadows flickered, overlapped and
vanished, and the flashlight on the ghosts made everything "wonky". Measured with consecutive
frame dumps (`LM_TELEPORT=1800,0,-1400,100`, Ultra, ghost lights, ambient 0.35, 120 fps target,
frames 2720-2840, standing still and with a scripted flashlight sweep across the dancers;
`LM_SHADOW_LOG=2` logs the shadow light every frame). The game's own shadow light there is the
flashlight the whole time (no switching except lightning); the popping came from the colored
light side:

| Cause | Fix |
|---|---|
| Ghost light intensity jumped between game frames (dance visibility toggles, appear flares, a per-frame random "nervous" flicker while in the beam), so pools, their shadows and the bloom popped; the room's mean brightness jumped by 10% in single frames | `ghost_lights.cpp`: submitted intensity smoothed per ghost (attack ~3, release ~5 frames); the beam/struggle flicker uses smooth noise (keys every 6 frames, smoothstep in between) |
| The two shadow-casting ghost lights were re-chosen every frame by a score that changes as the dancers move: shadows appeared and vanished | Hysteresis (a light that casts a shadow keeps it unless another is 35% stronger) and per-light shadow weights that fade over ~5 game frames, sent in `ExtraLight::flags` bits 8..15 (0 = full); `interp.cpp` blends the weight between game frames; Aurora uses up to 4 slots so fading shadows fit (patch 0020) |
| Screen-space light shadows: a binary per-pixel occlusion test with a step jitter that changed every rendered frame (crawling, noisy edges, different on each interpolated replay) | Patch 0020: the jitter is fixed per pixel, occlusion is soft (ramps over the thickness and toward the light), and the receivers sample the half-resolution mask with four bilinear taps (4x4 tent) |
| The game's shadow light switching (flashlight <-> room light / lightning / cutscene light) popped the new light's shadows in | `shadows.cpp` (enhanced lighting only): after a switch (or a jump of the light by more than 1500 units) the shadow strength (K3 alpha, read by `fn_8005DD50` after the pass) fades in over ~6 game frames. The old light's shadows still end at once (one shadow map per frame in the game) |

Per-light combination: each extra light's shadow removes only that light (mask × weight on its own
term), the game's shadow keeps the extra lights and part of the ambient (0019), so overlapping
shadows never double-darken a pixel lit by another light.

Results (`build/shadows4/`): temporal flicker (mean |L(t-1) + L(t+1) - 2 L(t)| / 2 over the room,
HUD excluded) with the sweep 4.57 -> 4.11 (dark pixels 2.50 -> 2.24, pixels above 8 levels
11.5% -> 10.3%); standing still 3.28 -> 3.21 (the dancers' own motion dominates); the room's mean
brightness no longer jumps (sweep: 44.4 -> 27.0 -> 48.0 before, 43.9 -> 44.7 -> 48.2 after).
Strips of consecutive frames, top before / bottom after: `strip_sweep.png`, `strip_still.png`,
`strip_lightning.png`. Master switch off: frame-identical to the previous build (nursery
lightning, 10 frames).

Not done: separate shadow maps per light with a true cross-fade of the game's light (the old
map is gone the moment the game renders the new one); a temporal filter of the screen-space mask
(the static pattern + blur made it stable enough at 30 and 120 fps).

## Debug draw overrides (Aurora patch 0012)

`patches/aurora/0012-wireframe-override.patch`, used only by the model viewer
(docs/UNUSED_CONTENT.md, "Model viewer"): `GXSetWireframeAurora(enable, rgba)` draws every
triangle-type draw (also the display-list optimiser's `GX_AURORA_DRAW_INDEXED` draws) as a
line list of its triangle edges in one colour (no culling, no depth bias, no draw merging
while on; WebGPU has no polygon fill mode), and `GXSetCullOverrideAurora(twoSided)` forces
GX_CULL_NONE. Both are FIFO commands (`GX_AURORA_SET_WIREFRAME` 0x0050,
`GX_AURORA_SET_CULL_OVERRIDE` 0x0051), so they stay in order with the draws and in the
interpolation tape. The game never issues them: pipelines and shaders are unchanged unless a
viewer turns them on (`ShaderConfig::wireframe` uses a former padding bit, so the pipeline
cache keys of existing pipelines do not change).

## Pipeline precache (Aurora patch 0013): no first-run flicker

**Symptom (user):** on the first boot many things flickered in and out while playing; later
runs were fine.

**Cause.** Aurora builds a WebGPU render pipeline (WGSL generated from the GX TEV/vertex/
indirect state, plus blend/depth/cull state) for every distinct GX state the game draws with,
lazily, on a background thread. `gx::render` skipped any draw whose pipeline was not ready
yet (`get_pipeline` returned false and the draw was dropped), so for the frames it took to
compile (about 50 ms per pipeline with a cold driver cache, 99 pipelines from boot to the
foyer) objects, effects and HUD parts were missing. Measured on a cold run boot -> foyer with
the old behaviour (`LM_PIPELINE_WAIT=0`): **5118 dropped draws**. Later runs were fine
because Aurora remembers every config in `pipeline_cache.db` and rebuilds them all at startup,
and Dawn's blob cache (`dawn_cache.db`) makes that fast (about 2 ms per pipeline). Both caches
were persisted correctly and keyed by content (xxh3 of the config struct; no pointers), so
they hit on the next run.

**Fix** (`patches/aurora/0013-pipeline-precache.patch`, `src/recomp/precache.cpp`):

- **Draws never drop.** `get_pipeline` waits for a pipeline that is queued or compiling (moves
  it to the front of the queue) instead of skipping the draw; only a pipeline whose creation
  failed is skipped (`droppedDraws` stat). Worst case is a hitch, never a missing object.
- **Several compile threads** (half the hardware threads, 1..8; `LM_PIPELINE_THREADS`). The
  shipped list compiles in about 3 s with 8 threads vs 13 s with one (warm driver cache).
- **Shipped precache list** `data/pipelines.bin` (copied next to the exe, installed with it):
  1999 pipeline configs (15 KB, zstd) recorded by an automated sweep. It holds Aurora's
  config structs only (generated metadata, no game data). `aurora_pipeline_cache_seed` adds
  them to the known set on every launch; Aurora's own `pipeline_cache.db` adds whatever this
  PC used beyond it. The known set is compiled in the background from the **first frame**
  (Aurora used to start before the application could add a list or set its accuracy flags),
  in first-use order (title screen first).
- **Setup compiles it with a progress bar** ("Compiling shaders", docs/SETUP.md), with a
  "Play now" button that leaves the rest to the background. On later launches whatever is
  missing (deleted cache, driver update invalidating Dawn's cache, new game version) compiles
  in the background while the title runs, with a small "Compiling shaders n / N" note in the
  corner; draws that need a pipeline before it is done wait for it.
- **Settings that change shaders.** The F1 > Visuals > Rendering accuracy toggles (integer
  TEV, EFB colour depth) are baked into each shader config. Aurora derives their variants from
  the recorded configs (`intTev` from the flag; EFB depth off clears `efbFmt`/`dither`/
  `efbBlend`, exactly as `populate_pipeline_config` does) and rebuilds the set in the
  background when a flag changes, so the list (recorded with both on, the defaults) covers
  every combination. MSAA only changes the render target layout, which Aurora already
  rebuilds the whole set for. The CRT filter is a post-process chain outside the pipeline
  cache; the wireframe override (patch 0012, model viewer) is not precached.
- The Dawn cache prune (deletes blobs not used in a session) now runs only after the first
  full rebuild was queued; with several threads and the deferred rebuild it could otherwise
  fire while the queue was momentarily empty and delete blobs still needed.

**Measurements** (US, dev build, RTX-class GPU, Original 30, 8 threads; "worst frame" is the
worst frame time per 5 s window, first window excluded since it contains window creation):

| Run | Pipelines created on demand | Draws that waited | Dropped draws | Worst frame |
|-----|------|------|------|------|
| Cold, old behaviour (`LM_PIPELINE_WAIT=0`, no list), boot -> foyer | 99 | 0 | **5118** | n/a |
| Cold, no list, waiting (`LM_PRECACHE=0`), boot -> foyer | 99 | 35 (1.4 s total, max 111 ms) | 0 | 224 ms |
| Warm (second run, own cache), boot -> foyer | 0 | 0 | 0 | 81 ms |
| Fresh AppData with the list, boot -> foyer | 0 | 0 | 0 | 83 ms |
| Fresh AppData with the list, all rooms in reverse order with the water element (not a sweep route) | 4 | 1 (0.2 ms) | 0 | 77 ms |
| Fresh AppData with the list, accuracy toggles off (`LM_ACCURACY=0`) | 0 | 0 | 0 | 36 ms |
| `LuigisMansionRecomp` fresh install (setup -> game) | 0 | 0 | 0 | 51 ms |

### The sweep (`tools/pipelines/sweep.py`)

Regenerate the list after Aurora changes that alter `gx::PipelineConfig`/`ShaderConfig`
(bump `GXPipelineConfigVersion` when the layout or meaning changes; rows of another version
or size are ignored, so a stale list only costs coverage, never correctness):

```
python tools/pipelines/sweep.py --work <scratch dir> --jobs 4 [--merge data/pipelines.bin]
```

It runs `lmrecomp` with a scratch APPDATA per scenario (in parallel), `LM_PRECACHE=0`, and
merges each scenario's `pipeline_cache.db` with `tools/pipelines/pipelines.py build`:
boot (title, menus, file select, the opening unskipped, foyer), the same with cutscene skip,
a teleport tour of every mansion room (points from the map's `jmp/furnitureinfo`, computed from
the disc dump at sweep time: `tools/pipelines/rooms.py`) with vacuum/blow/A/stick input and
the Game Boy Horror opened every 8 rooms, the tour with fire/water/ice, the Hidden Mansion
with USA and PAL rules (mirrored), the unused-content shader variants (early ghost shading,
GBH live camera), and maps 1, 3-13 (lab, training, Portrificationizer, unused 5/7/8, gallery,
roof/Bowser, Chauncey, Boolossus, graveyard/Bogmire). About 20 minutes; per-scenario counts:
boot 113, tour 757 (the tour ends at the roof and loads Bowser's map), fire 726, water 950,
ice 445, Hidden Mansion 660/664, unused 280, maps 75-145 (graveyard 677).

`pipelines.py info <list|db>` summarises a list; `aurora_pipeline_cache_export` writes the
current known set in the same format.

**Not covered yet** (these compile on first use, without dropping draws): the PAL game as the
base (its rendering code is the same, but it was not swept), the full story path (boss fights
past their intros, ending/credits, the gallery's portrait battles, Boo radar/King Boo), every
ghost type's capture/death effects (rooms were visited, not cleared), menus reached only from
a save with progress, MSAA levels (built from the same configs at the new layout, at runtime),
and the model viewer. Merging pipeline_cache.db files from longer play sessions
(`pipelines.py build -o data/pipelines.bin data/pipelines.bin <db>...`) closes the gaps.

### Test aids

- `LM_PIPELINE_LOG=1`: every pipeline creation (precache/demand, compile time), every draw
  that waited, every MISS (needed but not precached), and a summary line every 5 s
  (precache progress, demand-created, stalls, dropped draws, worst frame time).
- `LM_PRECACHE=0`: no shipped list (cold-cache testing); `LM_PIPELINE_LIST=<file>`: another list.
- `LM_PIPELINE_WAIT=0`: Aurora's old behaviour (skip draws whose pipeline is not ready), to
  reproduce the flicker.
- `LM_PIPELINE_THREADS=<n>`: compile threads.
- A fresh scratch `APPDATA` (or `LM_CACHE_DIR`) gives empty `dawn_cache.db`/`pipeline_cache.db`;
  the GPU driver's own shader cache is system-wide and stays warm, so first-ever runs on a new
  machine are somewhat slower than measured here.

### Cache robustness (Aurora patch 0017)

**Problem:** a crash once left a bad pipeline config in `pipeline_cache.db`; every later launch
rebuilt it at startup and crashed again. Aurora trusted whatever the caches held: the shader
generator aborts (FATAL) on enum values it does not know and indexes fixed arrays with
stage/texcoord/texmap ids, a Dawn error is an uncaptured error (FATAL), and Dawn even throws
(terminating the process) when a pipeline is created from a shader module that failed to
compile.

**What happens now** (`lib/gfx/pipeline_cache.cpp`, `lib/webgpu/gpu_cache.cpp`, a few lines in
`lib/gx/pipeline.cpp`):

- **Every cached config is validated before use.** `pipeline_cache.db` schema 2 (older caches
  are dropped): the stored `hash` (xxh3 of the config bytes seeded by the shader type, the cache
  key) is recomputed as a per-entry checksum, then every field the generator switches on or
  indexes with is range-checked (`validate_config`: blend/depth/cull, TEV args/ops/konst/order/
  swap/indirect state, texgens of the texcoords the shader reads, vertex attributes, colour
  channels, alpha compare, fog, padding bytes). Invalid rows are skipped and deleted with a
  `pipeline cache: skipping invalid entry ...: <reason>` line.
- **Pipeline creation is guarded** (`guarded_create`): Validation/Internal/OOM error scopes
  around each creation (Dawn error scopes are per thread, so the compile threads do not see each
  other's errors), and the GX shader module is checked before a pipeline is made from it. A
  failure gives an empty pipeline (its draws are skipped, `droppedDraws`), and a validation/
  compile failure records the config in the `pipeline_bad` table: it is never built again and is
  removed from the cache.
- **Crash journal** (`pipeline_cache.journal` next to the caches, 4 KB, memory-mapped): every
  thread writes the key of the config it is building into its slot while it builds it, and the
  file holds the session phase (startup until the first precache pass finished and 300 frames
  were shown, then running, clean after `aurora_shutdown`). Stores to a shared mapping survive a
  crash of the process, so the next launch knows:
  - configs that were being built when the process died become **crash suspects**: not precached
    any more, still built when a draw needs one (the quarantine is lifted when that works),
    refused after a second crash;
  - a death **during startup** with nothing in flight (e.g. a damaged Dawn blob) deletes
    `pipeline_cache.db` and `dawn_cache.db`; a second consecutive startup death deletes them
    too; from the third, the shipped list is also ignored for that run ("safe mode"). Reaching
    the running phase or a clean exit resets the count.
  Caveat: killing the process during startup (test scripts) counts as a startup crash; it
  costs a cache rebuild or the precache of the few configs that were in flight, never
  correctness.
- **Damaged database files** (not a database, truncated, damaged pages: `PRAGMA quick_check`
  at open, `SQLITE_CORRUPT`/`NOTADB` later) are deleted and recreated, for both
  `pipeline_cache.db` and `dawn_cache.db`, instead of running without a cache.
- **`dawn_cache.db`** schema 3 stores an XXH3 checksum of each uncompressed blob. Dawn asks for
  an entry's size before reading it; the entry is decompressed and verified on that size query,
  so a damaged blob is deleted and reported as a miss (Dawn rebuilds it) instead of being handed
  to the driver.
- **Shipped list** `data/pipelines.bin` format 2: CRC-32 of the payload and of every row
  (`tools/pipelines/pipelines.py` writes it; Aurora still reads format 1). Rows with a bad CRC
  or failing `validate_config` are skipped with a log line; the header's raw size is capped at
  64 MB.

**Tested** (scratch `APPDATA`/`LM_CACHE_DIR`, `LM_PIPELINE_LOG=1`, boot -> opening, each run
closed cleanly unless noted; the corruption scripts are not in the repo): random bytes in a
`pipeline_cache.db` entry (checksum mismatch, deleted); entries with an invalid blend mode, fog
type and depth function and a recomputed hash (validation, deleted); an entry with a 7-component
position attribute and a recomputed hash (valid ranges, WGSL fails to compile: recorded as bad,
not rebuilt next launch; before the fix this terminated the process inside Dawn); wrong schema
version (dropped); a row of another config version (ignored); the db truncated to 40 %, overwritten
with random bytes, or 200 random bytes flipped in its pages (recreated); 40 Dawn blobs damaged
(checksum mismatch or decompression failure: deleted, rebuilt), `dawn_cache.db` truncated /
random / page damage (recreated); shipped lists with 20 rows flipped (CRC), an invalid enum, a
non-compiling row, a truncated file, and a format 1 list; startup kills: one with configs in
flight (suspects recorded, no reset), then repeated kills at 3 s (reset after the first with
nothing in flight and after the second, safe mode from the third, back to normal after a run
that reached the running phase). The real list (2148 configs) has 0 rows rejected by the
validator.

Test aids: `LM_PIPELINE_LIST=<file>` (a damaged list), `LM_CACHE_DIR=<dir>`; the journal is
`<cache dir>/pipeline_cache.journal` (u32 magic "AUJN", version, phase 0 clean / 1 startup /
2 running, consecutive startup crashes, then 64 slots); the `pipeline_bad` table lists recorded
configs (`reason` 1 = failed creation, 2 = crash suspect, `crashes`).

## Enhanced lighting

Optional effects layered on top of the accurate GX rendering: F1 > Visuals > Enhanced lighting
(`[enhanced_lighting]` in settings.ini). **Master switch off by default**; off, the renderer
emits nothing new (no draw-class commands, no scene-end marker, no shader variants), and frame
dumps are byte-identical to the plain renderer (nursery: master off vs the plain frames of an
`LM_ENHANCED_AB` run, and earlier against the main build).
Every part has its own switch and strength/quality control; the Quality preset
(Low/Medium/High/Ultra/Custom) sets soft-shadow, volumetric and colored-light-shadow quality.
Code: Aurora patch `0016-enhanced-lighting.patch` (`lib/gfx/enhanced.cpp/.hpp`,
`include/aurora/enhanced.h`, `include/aurora/lights.h`, shader generator changes in
`lib/gx/shader.cpp`), port side `src/recomp/enhanced_lighting.cpp/.h` (settings → Aurora,
draw classes, scene bracket, test lights), `shadows.cpp` (caster-pass class, shadow map size),
`unused_content.cpp` (ghost pass), `ghost_lights.cpp` (the ghost light provider, below).

### How it hooks in

- **Draw classes** (`GXSetDrawClassAurora`, FIFO command `GX_AURORA_SET_DRAW_CLASS`, so it is
  ordered with the draws and recorded in interpolation tapes) are set only at hook boundaries,
  never between the game's own GX commands: WORLD inside the main 3D view (`fn_8000BB98(0)`),
  GHOST inside its ghost pass (`fn_80060E70` pass 0x30), OTHER inside the shadow-map caster pass
  (`fn_8005D984` .. `fn_8005D8CC`) and everywhere else (HUD/2D, GBH, menus). Aurora additionally
  treats non-perspective draws as OTHER (2D quads inside the view: screen copies, ghost-layer
  composite). Only sent while the master switch is on. (A first version decided per draw in
  `hle_gx.cpp`'s draw preparation; inserting commands there between raw FIFO writes was fragile.)
- **Shader variants** (`ShaderConfig::enhClass`, chosen in `enhanced::shader_class`): 1 for
  WORLD, 2 for GHOST, 0 (the plain shader) for OTHER, for additive draws (light effects: the
  flashlight beam is built on GX vertex lighting and a color-texgen lookup), and for untextured
  lit draws (effect volumes whose color/alpha *is* the lighting result; the nursery's beam haze
  depends on one). Which parts run inside a variant is a uniform (`enh_flags`), so toggling a
  part never rebuilds pipelines; only the master switch does (one-time compile hitch).
- **Scene bracket**: `fn_8000BB98(0)` (main 3D view, US+PAL hook) is wrapped: before it the
  frame's extra lights are sent (`aurora::gfx::set_extra_lights`), after it `GXSceneEndAurora`
  marks the end of the 3D scene, before the HUD: bloom, volumetric beam and ambient occlusion
  are composited onto the EFB there, so the HUD never glows and the CRT filter comes after.
- **A/B test aid** `LM_ENHANCED_AB=<n>`: game frames alternate in blocks of n between enhanced
  and plain (draws classed OTHER, no lights, no scene end: the plain renderer), so consecutive
  `LM_FRAME_DUMP` frames of one deterministic run compare both (with n = 1, odd dump frames are
  enhanced). The plain frames are byte-identical to a run with the master switch off.

### The parts

| Part (setting) | What | How |
|---|---|---|
| Per-pixel lighting (`per_pixel_lighting`) | The game's own GX lights evaluated per fragment with the same channel math (ambient/material sources, diffuse fn, spot/distance attenuation, clamps) | View-space position/normal interpolated to the fragment (`enh_pos`/`enh_nrm`; face normal from derivatives when the draw has no normals); WORLD draws only (ghost-pass draws keep vertex lighting: the ghost layer is composited from its own copy); channels that feed a color texgen keep their vertex result |
| Flashlight quality (`flashlight_quality`, softness, beam highlights) | Round beam pool with a smooth penumbra; glossy highlights where the beam hits | Spot color channels evaluated per pixel; below the cone's full strength the GX angular term is remapped through smoothstep (0 = GX curve, 1 = smooth). Alpha channels stay exact (the shadow light's ALPHA1 scales the shadow strength: smoothing it weakened shadows). "Beam highlights" (`flashlight_specular`, 0.6) adds a Blinn-Phong term (exponent 28) per spot light, tinted by the material color |
| Soft shadows (`soft_shadows`, quality, softness, contact hardening) | Soft edges for the game's shadow map (flashlight, lightning, room light) | The TEV `GR16_GT` compare stage (receiver key vs packed Z16 map) becomes a filtered compare: PCF 8/16 taps on a rotated Vogel disk, or PCSS (blocker search + penumbra from receiver-blocker distance) 16/32; receiver key computed analytically from the depth-ramp texcoord where it agrees with the TEV's key (no LINEAR byte-wrap stripes); capped slope-scaled bias on the off-centre taps only and a penumbra capped at 1.5x the radius (with a grazing shadow light, e.g. a ballroom ghost's own light near the floor, a full receiver-plane bias and wide penumbras erased thin casters' shadows); early out when the centre and 4 outer taps agree. Integer-TEV path included |
| Shadow look (`shadow_darkness` 0.9, `shadow_ambient_keep` 0.25: "Shadow darkness", "Colored shadow tint") | Shadows only remove the light they stand for: colored extra lights (ghost lights, test lights) keep lighting a shadowed pixel, a part of the ambient is kept, so shadows take on the surrounding color instead of going black | Aurora patch `0019-shadow-look.patch`: the game lerps a shadowed pixel toward the near-black K3 color by the shadow strength, whatever lights it. In the shader variants the shadow fraction from the compare stage is scaled by `enh_shadow_scale`: darkness × (lit − keep × ambient) / (lit + extra), with lit/ambient the luminance of color channel 0's GX lighting result and its ambient part, extra the extra lights reaching the pixel. 1.0 / 0 with no extra light = the game's darkening exactly; master switch off = untouched |
| Shadow map size (`shadow_map_size`) | 2048x1536 / 4096x3072 shadow map independent of the render resolution | `shadows.cpp`: the caster pass (`fn_8005D984` … `fn_8005D8CC`, new US+PAL hook) renders into an Aurora offscreen target (`GXCreateFrameBuffer`) of that size; the Z16 copy comes from it. Needs "Shadow map at render resolution" |
| Dynamic colored lights (`extra_lights`, intensity) | Up to 16 point lights per pixel on world geometry | `aurora/lights.h` interface below; added to the lit color channel (× material color) so the game's TEV decides the response; smooth (1-x²)² falloff to the radius, half-Lambert on ghost draws, optional specular |
| Colored light shadows (`extra_light_shadows`, quality) | Lights flagged `ExtraLightCastShadows` (up to 4; ghost lights flag the two strongest) are blocked by furniture/characters | Screen space, current frame: the opaque world draws are re-rendered depth-only (same pipelines, depth layout) into a half-resolution pre-pass **before** the EFB pass that draws them (pass pre-op), a ray-march pass (8/16/32 steps, thickness 160 units) writes an RGBA8 mask (one channel per light), which is copied into a reserved storage-buffer range; the world shaders read it (`enh_mask`, bilinear) |
| Volumetric beam (`volumetric_beam`, intensity 2.0, quality) | Dusty haze in the flashlight beam, stopped by walls | Scene end: ray march along each view ray (16/24/48 steps, 1/4 or 1/2 resolution) through the flashlight's GX spot attenuation (the flashlight = the bright angular spot light nearest the camera among the world draws' lights), forward-scattering phase, depth-limited; density modulated by slowly drifting 3D value noise (dust motes) |
| Bloom / glow (`bloom`, intensity, light sources, colored light glow, bright surfaces, threshold, radius, flashlight-lit walls) | Light sources glow, the HUD never | Scene end, HDR (RGBA16F) chain at ≤ 720 lines: bright pass = **glow buffer** (emissive) + soft-knee scene highlights × "bright surfaces" (suppressed where the flashlight is the only reason a wall is bright) + **light glows**; 6-level 13-tap downsample / tent upsample; composited additively onto the EFB |
| Ambient occlusion (`ssao`, intensity, radius; off by default, experimental) | Contact darkening | Scene end, half resolution, 12 hemisphere samples from the depth buffer, depth-aware blur, multiplied onto the EFB |

**Glow buffer (emissive-aware bloom).** Additive draws (blend SRCALPHA/ONE etc.: glows, lens
sprite, sparkles, fire, dust, lightning and nozzle effects; not the beam volume itself, whose
alpha is a color-texgen lookup: glowing it again washed the picture out when it points at the
camera) are recorded with a copy of their pipeline for a RGBA16F target and re-rendered right
after the EFB pass they were drawn in (before a later EFB copy could change textures), depth
tested (inclusive, no write) against that pass's depth, at bloom level-0 resolution. The copy
takes the draw's final state (Aurora merges consecutive draws into the recorded command later;
the first version re-rendered only the first fragment of the beam cone, a wedge pointing the
wrong way). The game draws the 3D scene, copies it (RGB565 640x480, color clear, depth kept),
draws the ghost layer and pastes the scene back before the HUD: glow draws survive that copy;
a color + depth clear (a new scene) drops them. Ordinary lit surfaces only glow through "bright
surfaces" (0.25 by default), so walls do not bloom; the glow buffer is Reinhard-compressed.
**Light glows**: each extra light adds a small screen-space halo at its position (core =
3% of its radius), hidden when the depth buffer shows a surface in front of it.

### Extra light interface (`include/aurora/lights.h`)

```cpp
namespace aurora::gfx {
struct ExtraLight {
  float pos[3];       // VIEW space (GX eye space, same space as GXLoadLightObjImm positions)
  float color[3];     // linear RGB, may exceed 1
  float radius;       // world units; smooth falloff to 0 at radius
  float intensity;    // multiplier
  uint32_t flags;     // bit0: cast shadows, bit1: affects ghosts/translucent, bit2: specular
  uint32_t id;        // stable per-source id for interpolation pairing (0 = none)
};
void set_extra_lights(const ExtraLight* lights, uint32_t count);  // once per game frame
}
```

Port side: `recomp::submitExtraLights(lights, count)` (guest thread, once per game frame,
strongest first, up to 16); `enh::worldToView` converts world positions with the current
camera. The set is written into the GX FIFO (`GX_AURORA_SET_EXTRA_LIGHTS`) at the start of the
main view, so it is part of interpolation tapes: `interp.cpp` blends position, color, radius and
intensity between game frames by `id` (unpaired lights are shown as captured; jumps > 600 units
are not blended). "Translucent" (bit1) lets a light reach ghost-pass draws; ghost lights leave it
off. Test aid: `LM_TEST_LIGHTS=<count>[,<radius>[,<intensity>]]` (colored lights orbiting Luigi,
the first two cast shadows).

### Verification (US unless noted; `build/enhanced/*.png`; native 640x480 frame dumps of the
deterministic nursery script `LM_START_MAP=2 LM_START_APPEAR=1 LM_TEST_STATE=poltergust
LM_SKIP_CUTSCENES=1 LM_SKIP_AUTO=30 LM_INPUT_CLOCK=frames` with `LM_ENHANCED=1 LM_ENHANCED_AB=1`)

- Master off vs the AB run's plain frames: identical (5 frames); earlier against the main build.
- `nursery_beam_off_on.png`, `nursery_beam_crop_off_on.png`: flashlight on the door: dusty
  volumetric haze in the beam, smoother pool, lens glow.
- `nursery_lightning_off_on.png` (`LM_LIGHTNING=0,60,1`), `lightning_shadows_crop_plain_hard_soft.png`
  (plain / enhanced with soft shadows off = identical hard shadows / PCSS).
- `colored_light_shadows_off_on_mask.png` (`LM_TEST_LIGHTS=4`): colored pools on walls and floor,
  shadows of crib and bears, and the shadow mask (`AURORA_ENH_GLOWDBG=8`).
- Ballroom shadows (`LM_TELEPORT=1800,0,-1400,100`; the shadow light there is a dancing
  ghost's own light near the floor, so the long shadows change every frame with the dance):
  `ballroom_shadows_per_part.png` (enhanced frames of AB runs that are frame-aligned with each
  other, rows: everything off, per-pixel, flashlight quality, bloom, volumetric, soft shadows,
  default minus soft shadows) and `ballroom_shadows_alloff_default_ghostlights.png`: the game's
  shadows are kept in every configuration. The earlier report of missing shadows came from
  comparing different game frames (consecutive AB frames differ in the dance) plus two real
  issues fixed since: the penumbra smoothing was applied to the shadow light's ALPHA1 mask, and
  the soft-shadow bias/penumbra erased thin casters with a grazing light. Remaining visible
  difference: bloom brightens the white floor (bright surfaces 0.12 by default).
- `ghost_layer_ghostlight_ab_diff.png` (gold ghosts, room 42, ghost lights A/B with difference):
  changes only at the lit ghost and its light pool, no duplicate ghost shapes (the faint shapes
  in the ghost-lights agent's capture images are other ghosts at different times).
- Showcase: `ballroom_dark_showcase.png` (+ `_full_2560x1440`, `showcase2`, contact sheet):
  Ultra, ghost lights, bloom, ambient_scale 0.35, 16:9, render scale 3, window captures.
- `ws21x9_off_on.png` (21:9, render scale 2), `msaa4x_scale4_16x9_off_on.png` (MSAA 4x, scale 4,
  16:9), `pal_base_off_on.png` (European game via LuigisMansionRecomp setup),
  `hidden_mansion_off_on.png` (`LM_HM_FORCE=1`).
- Interpolation at 120 fps (`LM_INTERP_TEST=4`, walking, test lights): the in-between replays
  converge monotonically on the next game frame, the last step is identical to the captured
  frame; extra lights move smoothly (`interp_120fps_steps.png`).

### Cost

RTX 3090 Ti, nursery gameplay with 4 test lights, render scale 8 (5120x3840), uncapped at
320 fps target (the 120 Hz display caps at ~120), Ultra preset + AO + 4096 shadow map.
`LM_ENH_PERF_AB=6,<part>` switches one part off/on every 6 s within a run and `LM_FPS_LOG=1`
logs presented fps with the phase; numbers are the mean over ~18 two-second samples per phase
(other agents' game instances shared the GPU, so +-0.5 ms is noise):

| Part | Cost per frame at 5120x3840 |
|---|---|
| Everything (master switch) | +0.6 ms (after the fixes below; was +3.8 ms) |
| Per-pixel lighting | below noise (-0.3 ms) |
| Flashlight quality + highlights | +0.1 ms |
| Soft shadows (PCSS 32) | +0.45 ms (was +5 ms before the early out) |
| Colored lights (4) | +1.6 ms |
| Colored light shadows (2 lights, 32 steps) | below noise |
| Volumetric beam (High) | +0.05 ms (was +4.4 ms before capping its target at 720 lines) |
| Bloom | +0.6 ms |
| Ambient occlusion | +0.1 ms |
| Shadow map 4096x3072 | below noise |

At 1x-3x render scale every part is well below a millisecond. Turning the master switch on
compiles the enhanced shader variants once (a short hitch with the pipeline precache warm).

### Look

One shared look for colored light: "Light color vibrance" (`extra_light_saturation`, 1.6)
pushes every extra light's color away from its gray (keeping its brightest channel), for every
provider (all ghost lights, test lights) and their bloom halos; the flashlight, lamps, candles,
fireplaces, lightning and element effects get the same treatment through per-pixel lighting,
highlights and the emissive glow buffer. Each part keeps its own toggle and slider.

### Debug aids (Aurora, env)

`AURORA_ENH_LOG=1` (scene-end/flashlight/glow/light-shadow log), `AURORA_ENH_DRAWLOG=<n>`
(+`AURORA_ENH_DRAWLOG_ALL`: every draw of the n-th scene with class, blend, lighting, TEV),
`AURORA_ENH_GLOWDBG=1` (show the glow buffer), `=2` (one glow draw per frame), `=3/5/6` (all
world draws into the glow buffer / without depth test / inverted test), `=7` (glow pass depth),
`=8` (light-shadow mask), `=9/10` (mask pass depth / light distance), `AURORA_ENH_HIDE_ADDITIVE=1`
(drop additive world draws), `AURORA_ENH_NOVARIANT=1` (no shader variants),
`AURORA_SHADER_DUMP=<dir>` (every generated WGSL shader). Port: `LM_ENHANCED=0|1`,
`LM_ENHANCED_AB=<n>`, `LM_TEST_LIGHTS`, `LM_SHADOW_MAP=0|1|2`, `LM_LIGHT_TRACE=1` (each distinct
`GXLoadLightObjImm` call with its guest chain).

### Ghost lights

F1 > Visuals > Enhanced lighting > "Ghost lights" (`[ghost_lights]` in settings.ini; needs the
Enhanced lighting master switch and "Dynamic colored lights"; off by default). Every ghost the
game draws becomes a coloured point light in the renderer's extra-light list
(`aurora/lights.h`), so ghosts light the floor, walls and furniture around them in their own
glow colour. Code: `src/recomp/ghost_lights.cpp` (US + PAL).

**Where the data comes from** (USA addresses; `VA()` / `VOFF()` for the European game):

| What | Source |
|---|---|
| Ghost actors | live character slots `lbl_803D48A0` (count `lbl_804D8750`), the list the actor draw pass `fn_80060E70` walks. A ghost = type (actor +0x3C) with type flag 8 in `lbl_8033B590` (0x1C per type: +0x08 u16 flags, +0x14 s16 class 1 small / 2 normal / 3 portrait / 4 boss, +0x18 name) |
| Drawn this frame | the draw pass's own test: actor +0x40 flags (0x6000 not drawn, 0x100 hidden from the normal passes, 0x200 any room) and room byte (+0xB4 >> 24) visible through `fn_8001CE1C(room, -1)` |
| Position | actor +0x44, lifted 40/90/110/160 units (class 1/2/3/4): +0x44 is where the game puts the small GX light some ghosts carry (`EnZako::fn_800C1FFC` → `fn_80016C98(0, actor+0x44, BrightColor × alpha, 40, 80)`), but walking ghosts have it near the floor (ballroom shy-guy ghosts at y 60), where a point light only grazes the floor |
| Colour | the ghost's glow colour, EnemyParam "BrightColor" (`/param/ctp/<type>.prm`, `Koga::EnTypesManager::sManager`+4, 0x218 per type, value at +0x1FC; same layout on PAL), which the game copies to actor +0xE4 and uses for the ghost's light effects. The actor copy is read; parameter and a per-class table are fallbacks |
| Element ghosts | EnemyParam Element (+0x198): 1 fire, 2 water, 3 ice (from the weakness test at 0x800C75F8). They all share the pale glow C1DCF7, so their light is fire orange / water blue / ice cyan instead |
| Visibility | actor +0xE3, the alpha the ghost layer is drawn with (0 invisible .. 255) |
| HP | actor +0xFC (s16), maximum = EnemyParam life (+0x1C) |
| States | `Koga::CharacterEvent` messages to the ghost's AI (`fn_800C0F08`, hooked in gameplay_hooks.cpp; message → observer slot from `receiveMessage`'s jump table): 2 in the flashlight beam (every frame), 3 capture begin, 4 capture end (also sent when a ghost vanishes/escapes), 9 fishing begin (hooked by the Poltergust), 12 collided with Luigi, 13/14/15 fire/ice/water spray, 16/17/18 fire/ice/water ball. The Poltergust capture update `fn_80081BCC` (ghost at capture +0x18) runs every frame a ghost is held |
| Camera | GX view matrix `lbl_803A3820`+0x18C (+0x198 on PAL), the matrix `fn_80057CFC` uses to put the S3D lights in eye space for `GXLoadLightObjImm`; identical to `enh::worldToView` (checked) |

Per-type glow colours (US, `LM_GHOST_LIGHT_LOG` dumps the table): gold ghost `mgyapoo1`
F19726, Chauncey `bbaby` CCFF99, Boolossus/Boos `btelesa`/`telesa` white, Mr. Luggs `eater`
CC74F0, waiters A3DDE0, ballroom shy-guy ghosts `heypo1..8` red/green/white/brown/orange/
yellow/pink/purple, blue/ice ghosts `iyapoo*` 5ACBE1, gold mice F5C50F, Bogmire `bshadow`
white, Neville `situji` 0032FF.

**Behaviour** ("Dramatic ghost light effects", on by default): gentle two-frequency pulse; a
flare when a ghost fades in or collides with Luigi; a nervous flicker that brightens while the
flashlight beam is on it; while the Poltergust holds it the light ramps with lost HP
(0.35 + 0.65 × (1 − HP/max)) with a fast tremor and a larger radius; capture (message 3, or HP
reaching 0 while held) gives a ~3× burst that fades over 1.5 s and follows the ghost into the
nozzle (and stays at its last position if the actor leaves the list); element hits tint the
light with the element for 20 frames. Invisible ghosts give no light ("Invisible ghost glow"
0..0.3 adds a faint hint). Lights are sorted by strength / distance; "Max ghost lights"
(default 8, renderer limit 16) keeps the strongest, the strongest two get the cast-shadows flag
when "Shadows from ghost lights" is on (the renderer does not implement extra-light shadows
yet). Radius by class 380/560/720/1100 × "Ghost light radius"; ids = actor pointer, so
interpolation pairs them. Actors are pooled and reused, so capture marks expire.

The lights do not carry `ExtraLightTranslucent`: only draws of the ghost pass (`fn_80060E70`
pass 0x30) are ghost draws for the renderer, blended room geometry (much of the ballroom floor
and walls) is ordinary world geometry and gets the light, and a ghost's body does not light
itself; its halo comes from the bloom's light glow.

Built at the start of the main view `fn_8000BB98(0)` (enhanced_lighting.cpp's hook calls
`ghostlights::onMainView` before `enh::beginScene`) and submitted with
`recomp::submitExtraLights`; frames without the game scene have none.

**Verification** (US unless noted; build/ghostlights/*.png): `LM_GHOST_LIGHT_AB=1` sends the
lights only on every other game frame so consecutive `LM_FRAME_DUMP` frames compare with/without
in the same scene (frame brightness then alternates by ~7.5e6 per frame in the ballroom).
Ballroom (`LM_TELEPORT=1800,0,-1400,100`): coloured pools around the shy-guy ghosts on the floor
and the chandelier (`ballroom_ab.png`); Chauncey (`LM_START_MAP=10`): green light on the
balcony floor and balustrade (`chauncey_ab.png`); Boolossus (map 11): white light, collide
events flare it (`boolossus_ab.png`). Logged in game: gold ghost in the training room
(`LM_START_MAP=3`; appear flare, in-beam events, flee/despawn with message 4), Mr. Luggs and the
waiters in the dining room, the ballroom ghosts. PAL base (map 11): same ghost, colour, view
positions and events.

**Stun → vacuum → capture, in play** (gold ghosts, map 2 room 42 = enemyinfo room 39,
`LM_TELEPORT=0,550,-3300,100`; the ghosts spawn as soon as Luigi is in the room). What the
game does (US): the gold ghost strategy (`lbl_8035B6E4`) cycles AI states 4 appear → 5 approach
→ 8 hover in front of Luigi (~50 frames) → 0xA attack (fully visible) → vanish → 4 (9 is a
later hover). Its beam handler (`fn_800C7824`) only reacts in states 5/8/9 (`fn_800C8F88`): it
starts the light-bind sub-strategy at strategy +0x504 (`fn_800C8FE8`, pointer at +0x590), which
counts frames the ghost is still lit (message 21 = EnThought +0x908 > 0; message 2 sets +0x908)
against `fn_800DBE8C`: the type's LightBindFrame (gold ghost 25) scaled by distance between the
Min/MaxLightBindRange 100..400 (closer = more frames). In state 5 it also starts fleeing.
Sub-state 3 = stunned: heart and HP counter shown, and it fades away within ~15 frames unless
the vacuum hooks it.

Why the first attempts never vacuumed: each time a ghost appears Luigi is startled for ~77
frames (Poltergust mode, Luigi +0x1180, = 0; `LM_NOZZLE_DUMP=1`), and that window covers the
first hover/attack. A stun inside it ends before R can do anything. R itself works under
`LM_TEST_STATE` once gameplay has started. The hunting aid therefore stuns only while the mode
is non-zero and holds R from the moment it aims (`LM_GHOST_HUNT_R=0`), nozzle tilted up (C stick)
and pulling away once hooked.

Run that completes the whole sequence (deterministic): `LM_GHOST_HUNT=9 LM_GHOST_HUNT_BEAM=89
LM_GHOST_HUNT_DMAX=300 LM_GHOST_HUNT_R=0` plus the teleport above, `LM_TEST_STATE=poltergust`,
`--input "600:START*10,1000:A*6"`: aim at f2546 (state 9, 216 units), R → Poltergust mode 1,
message 9 (hooked) at f2556, HP 10 → 0 by f2626 (capture update holding the ghost every frame),
message 3 (capture begin) f2634, message 4 (inside the Poltergust) f2662. Ghost light intensity
over it (`LM_GHOST_LIGHT_LOG`): in the beam ~2 (flicker), held 3-7 rising with lost HP plus
tremor, capture burst peaking at ~6 at f2629 and fading to 0.3 by f2659, off once captured.
The beam stun is the stand-in (`LM_GHOST_HUNT_BEAM` delivers the game's own message 2 in
states 8/9; aiming the real flashlight up at the hovering ghost from a script did not work);
everything after it is the game's own vacuum and capture.

Frame dumps with `LM_GHOST_LIGHT_AB=1` (off / on / difference; presents 4990..5101 of that run):
- `gold_ghost_stun_ab.png`: lit and stunned (heart shown): orange light on the wall and floor.
- `gold_ghost_vacuum_ab.png`: hooked, HP 10 → 4 → 2: the pool on the floor and walls grows
  brighter and wider as the HP drains.
- `gold_ghost_capture_ab.png`: capture burst (the room lit orange), fading while the ghost is
  pulled into the nozzle, nothing once it is inside.
The first capture peaked at ~17× (burst multiplied by the vacuum struggle) and washed the room
out; the struggle is no longer applied during the capture.

Element ghosts and element hits have not been exercised in play.

Test aids: `LM_GHOST_LIGHTS=1` (on regardless of the settings; with `LM_ENHANCED=1`),
`LM_GHOST_LIGHT_LOG=1|N` (every ghost each N frames: id, type, class, element, state, alpha,
HP, flags, colour, world and view position, intensity, radius; the lights sent; every event;
vacuum holds; the type colour table once), `LM_GHOST_LIGHT_DIFF=1` (changed bytes 0..0x200 of
drawn ghosts, research aid), `LM_GHOST_LIGHT_AB=1`, and the hunting aids (US, research):
`LM_GHOST_HUNT=1|<AI state hex>` steers Luigi (after `--input`: faces away from the target
ghost while it hovers, turns to it in the given state, holds R and closes in with the nozzle
tilted up once it is stunned, pulls away when hooked), `LM_GHOST_HUNT_BEAM=<states>`,
`LM_GHOST_HUNT_DMAX`, `LM_GHOST_HUNT_DELAY`, `LM_GHOST_HUNT_R`, `LM_GHOST_HUNT_TURN`,
`LM_GHOST_HUNT_CY`, `LM_GHOST_HUNT_DIV`, `LM_GHOST_HUNT_FLIP`. The log line's `ai` field is the
ghost's strategy state (EnThought +0x800 → +0x0E; decorator inner state in the high half), and
the first AI of each type logs its strategy vtable.
