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
