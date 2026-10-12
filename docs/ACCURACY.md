# Hardware accuracy research (vanilla game)

Research pass, 2026-10-11, branch `accuracy-research`. **Research only: no rendering, audio,
Aurora patch or settings code was changed.** Everything below is documentation of what the
GameCube hardware does, what Luigi's Mansion (LM, US) uses, and how the port's current code
(Aurora with patches 0001-0032 + 0116, the port's HLE GX, the DSP stand-in) compares. Candidate
fixes are listed for the user to decide on later (section 5).

Earlier accuracy work this builds on: docs/RENDERING.md ("GX accuracy audit", "Indirect effects",
"EFB copies to RAM", "Shadows") and docs/AUDIO.md. Those sections are not repeated here; this file
checks them against outside sources and adds what they did not cover.

## 1. Method and sources

**Evidence from the game** (vanilla settings: Original 30, 4:3, render scale 1, portal fix,
"Shadows at any distance" and "Widescreen shadows" off, enhanced lighting off; scratch APPDATA
and card; nursery with `LM_TEST_STATE=poltergust LM_START_MAP=2 LM_START_APPEAR=1
LM_SKIP_CUTSCENES=1 LM_SKIP_AUTO=30 LM_INPUT_CLOCK=frames`, title -> file select -> gameplay with
flashlight, vacuum, Game Boy Horror):

- `AURORA_GX_AUDIT=1`: every distinct pipeline feature (section 2.1).
- `LM_GX_AUDIT=1`: SDK state calls with their arguments (light setup, copy filter, fog, dither...).
- `LM_TRACE=801F8B5C,801F823C,801F8AE4` (GXSetZCompLoc, GXSetAlphaCompare, GXSetZMode): caller
  and arguments of every call, grouped (section 3.5).
- `LM_FRAME_DUMP` native EFB frames 2500-3300, analysed for pixels that are not RGBA6 values
  (section 3.7).

**Outside sources** (all read as untrusted data; no code copied; Dolphin and libogc are GPL and
were only read to understand the hardware):

| Ref | Source | What it is |
|-----|--------|-----------|
| [D-PS] | Dolphin `VideoCommon/PixelShaderGen.cpp` | hardware-tested TEV arithmetic, alpha test, early Z, dither, fog |
| [D-VS] | Dolphin `VideoCommon/VertexShaderGen.cpp` | normals, emboss, colour texgen, pixel centre, depth |
| [D-LS] | Dolphin `VideoCommon/LightingShaderGen.cpp` | lighting equation in integers |
| [D-SW] | Dolphin `VideoBackends/Software/SWEfbInterface.cpp`, `TextureSampler.cpp` | blending, dither, RGBA6 storage, YUV coefficients ("determined by hardware testing"), bilinear weights |
| [D-TD] | Dolphin `TextureDecoder_Util.h` + PR #4656 (CMPR compared on a real Wii) and PR #4512 | CMPR decode |
| [D-TC] | Dolphin `TextureConversionShader.cpp` | EFB copy encoders, copy filter overflow, gamma |
| [D-Z] | Dolphin `DSPHLE/UCodes/Zelda.cpp` | HLE of the DSP microcode family LM uses (LM = CRC 0x42F64AC4) |
| [D-PC] | Dolphin PRs #306 (pixel centre 7/12, "hardware test"), #12972 (open, proposes removing it) | rasterizer pixel centre |
| [D-12903] | Dolphin issue 12903 (LM intro: Luigi's mouth flickers/see-through) | closed as dup of 7143 "DualCore Timing Mega Issue" |
| [SDK] | `decomp/libs/dolphin/src/gx/*.c` (the SDK as built into LM, in this repo) | exact SDK behaviour of GXInitLight*, GXSetChanCtrl, GXInitTexObjLOD |
| [OGC] | libogc `gc/ogc/gx.h` doc comments | GXSetZCompLoc, GXSetDither, GXSetLineWidth... |
| [PAT-Z] | US 6,664,958 "Z-texturing" | Z texture adders do not clamp; alpha compare can mask Z writes |
| [PAT-TEV] | US 7,034,828 "Recirculating shade tree blender" | TEV datapath widths (8-bit A/B/C, signed 10-bit D/output), clamp ranges |
| [HW-TEV] | Dolphin hwtests `gxtest/tev.cpp` (neobrain, 2014; run on a console) | randomized TEV combiner test incl. DIVIDE_2, bias, SUB, clamp; compare mode |
| [HW-RAS] | Dolphin hwtests `gxtest/rasterization.cpp` (neobrain, 2014) | where a pixel counts as covered: left-edge threshold, tiny-viewport probe |
| [D-12844] | Dolphin issue 12844 (Scorpion King / Spawn: line artifacts), pokechu22's hardware FIFO-player comparison | 1:1 LINEAR blits of an EFB copy are sharp and do not wrap on hardware; bisected to PR #306's 7/12 offset |
| [PAT-AA] | US 6,999,100 "Method and apparatus for anti-aliasing" | 12x12 subpixel grid per pixel, 3 programmable samples per pixel in 1/12 units, Z per sample from the quad centre plane |
| [OGC-SO] | libogc `GX_SetScissorBoxOffset` doc | "the dither matrix is 4x4"; the scissor offset displaces the image relative to the matrix |

## 2. What LM uses (vanilla, measured)

### 2.1 Pipeline features (AURORA_GX_AUDIT, nursery run)

| Area | Seen |
|------|------|
| EFB | `RGBA6_Z24`, dither on and off per draw |
| Blending | NONE (ONE/ZERO) with and without dst alpha; BLEND SRCALPHA/INVSRCALPHA; SRCALPHA/ONE (additive: flashlight beam, glows); ONE/ZERO in blend mode. No DSTALPHA factors, no logic ops, no subtract |
| Alpha test | `GREATER 0 AND GREATER 0`, `GREATER 0 OR GREATER 0`, `GEQUAL 0` (always passes); Z compare before **and** after texturing; Z update on and off |
| TEV | 1-5 stages; ops ADD, SUB, `COMP_GR16_GT` (shadow, alpha unit); scale x1 and x2 only (**no DIVIDE_2, no bias**); clamp on/off; konst K1-K3 colour, K0/K3 alpha; swap tables 1-3 |
| Lighting | COLOR0/ALPHA0/ALPHA1: `diff=CLAMP attn=SPOT` (flashlight, spot/dist fns COS, COS2, FLAT / GENTLE, MEDIUM, STEEP), `diff=NONE attn=SPEC` (specular via `GXInitSpecularDir`, shininess via `GXInitLightAttn(0,0,1, k0,0,1-k0)`), `diff=NONE attn=NONE`; ambient and material from registers and vertex |
| Texgens | MTX2x4/3x4 from TEX0, POS, NRM (env map); **emboss BUMP0 from TEX0**; colour texgen SRTG from COLOR0 (flashlight cone); post matrices; no normalize |
| Indirect | only the Mario painting (events 5/6), see RENDERING.md |
| Fog | `GXSetFog(type NONE)` always; range adjust enabled once with centre 0 (no effect without fog) |
| Lines/points | `GXSetLineWidth(6)` (= 1 pixel) |
| Copies | display copy filter `{6,0,20,12,20,0,6}`, at one point `{16,0,10,12,10,0,16}` (both sum to 64); copy clamp 3; gamma 1.0; texture copies Z16, RGB565, RGBA8, RGB5A3, IA4 (RENDERING.md) |
| Textures | min/mag LINEAR or NEAR, LOD 0..0 or 0..12.5 with edge LOD, bias 0, no mip filtering seen |

## 3. Area by area: hardware vs port

Status legend: **match** (port agrees with the documented behaviour), **mismatch** (documented
behaviour differs; LM-relevant unless noted), **open** (sources disagree or no reference exists).

### 3.1 TEV arithmetic

Hardware [PAT-TEV, D-PS]: A/B/C are unsigned 8-bit (a signed 10/11-bit register used as A/B/C is
truncated to its low 8 bits), D and outputs are signed (11-bit range -1024..1023 unclamped, or
0..255 clamped). Lerp `A*(256-C') + B*C'` with `C' = C + (C>>7)`; scale x2/x4 is applied inside
(to D and the lerp) before the divide by 256; rounding constant +128 for ADD, +127 for SUB, none
for DIVIDE_2 (Dolphin marks the DIVIDE_2 case "TODO", i.e. unverified). Compare ops: `D + (A op B ?
C : 0)`, bias/scale not applied, clamp applied; R8/GR16/BGR24 compare the colour inputs even in the
alpha unit. The PE takes the low 8 bits of the last stage.

Port (patch 0010 integer TEV, 0011): ADD and SUB give exactly Dolphin's results (checked
algebraically: `floor((128 - x)/256) == -floor((x + 127)/256)`). DIVIDE_2 rounded (+256 before
`>>9`) where Dolphin truncates; **settled on `hw-details` (section 9.3, Aurora 0180): truncation,
as Dolphin's hardware test expects**. LM never uses DIVIDE_2; SMS does (Bianco Hills' alpha-tested windmill blades).
Compare ops, simultaneous colour/alpha input reads, konst k/8 values, final `& 255`: **match**.

### 3.2 Lighting

Hardware [SDK, D-LS, D-VS]:
- `GXSetChanCtrl` writes diffuse NONE whenever attn is SPEC. Port: same (**match**).
- SPOT: `ldir = normalize(lpos - pos)`, `cos = max(0, ldir . dir)`, `attn = max(0, a0 + a1 cos +
  a2 cos^2) / (k0 + k1 d + k2 d^2)`. SPEC: `attn = (N . ldir >= 0) ? max(0, N . dir) : 0`, then the
  a/k polynomials in that value (k normalized only with a diffuse fn). Port: same (**match**;
  0116's zero-coefficient rule only removes NaN).
- `GXInitSpecularDir`: position `-n * 2^20`, half-angle `normalize(-n + (0,0,1))`. Port:
  `GX_LARGE_NUMBER = -1048576` (GameCube build), **match**.
- Accumulation: Dolphin models the hardware in integers: each light adds `round(attn * diff *
  colour)`, the sum starts at the ambient colour and is clamped to 0..255, result
  `(mat * (lacc + (lacc >> 7))) >> 8`. The port computes in float (`mat * clamp(sum)`) and the
  integer TEV rounds the rasterized colour to 8 bits per pixel. Difference: at most about 1 level
  per vertex, unbiased. **Minor mismatch**, not visible.
- Normals: hardware (and Dolphin) normalize the transformed normal; tangent/binormal are not
  normalized (their scale sets emboss strength). Port: same (**match**; this answers the open
  question in RENDERING.md "whether hardware normalizes transformed normals").
- Emboss (BUMP0..7): `texcoord = source texcoord + (ldir . T, ldir . B)` with `ldir =
  normalize(lpos - pos)`. Port: same (**match**).
- Colour texgen (SRTG): s,t = lit COLOR0/1 R,G (0..1). Port (0008): **match**.

SDK light-init functions reimplemented by Aurora, compared with LM's own SDK [SDK]:

| Function | SDK | Aurora | LM impact |
|---|---|---|---|
| `GXInitLightSpot` GX_SP_SHARP | `a0 = cr(cr-2) / (1-cr)^2` | `a0 = cr(cr-2)` (**missing the divide**) | none (LM uses OFF/FLAT/COS/COS2) |
| `GXInitLightSpot` GX_SP_RING1 | `a0 = -4cr / (1-cr)^2` | `a0 = +4cr / (1-cr)^2` (**wrong sign**) | none |
| `GXInitLightSpot` cutoff test | `cutoff <= 0 or > 90` -> OFF | same | - |
| `GXInitLightDistAttn` | OFF if `ref_dist < 0` or `ref_br <= 0` or `>= 1` | OFF if `ref_br < 0` (so `ref_br == 0` divides by zero: k1/k2 = inf) | none (LM's brightness 0.2-0.99) |
| `GXInitSpecularDir`, `GXInitLightDir`, `GXSetChanCtrl`, `GXInitTexObjLOD` | | same as SDK | - |

These are SDK bugs in Aurora, not hardware questions: any game using SHARP or RING1 spots gets
wrong cone shapes. **Relevant for SMS** if it uses them (check its `GXInitLightSpot` calls).

### 3.3 Texture decoding and filtering

- CMPR [D-TD]: interpolants `(3 c_a + 5 c_b) >> 3` (5/8 toward the nearer endpoint), 3-colour
  mode midpoint floored with the 4th entry = midpoint with alpha 0, endpoints expanded by bit
  replication. Dolphin PR #4656 compared this on a real Wii. Aurora `S3TCBlend`/`HalfBlend`:
  identical (**match**).
- I4/IA4/RGB565/RGB5A3 (3-bit alpha `(a<<5)|(a<<2)|(a>>1)`): bit replication, **match**.
- Bilinear filtering: Dolphin's software renderer (reference model, not stated as hardware
  tested) uses 7-bit sub-texel weights (1/128 texel) and truncates the weighted sum. PC GPUs use
  8-bit weights; the port rounds the filtered sample to 8 bits in integer TEV mode. Difference:
  up to 1 level on filtered texels, average +0.5 level (rounding vs truncation). **Minor
  mismatch**; also the "bilinear filtering of the packed depth/ramp textures" shadow suspect
  (section 4) differs by at most 1 in the low depth byte (1/65536 of the depth range).
- LOD: LM uses no mip filtering in the scenes measured; LOD bias/edge LOD encodings match the SDK.

### 3.4 EFB copies, copy filter, gamma

- Copy filter [D-TC, D-SW]: rows above/centre/below, sum >> 6, overflow masked to 9 bits then
  clamped to 255. LM's two filters both sum to 64, so overflow never occurs. Port (0018, crt.cpp):
  **match**.
- Gamma 1.0 only. **Match**.
- Z16 copy byte order [D-TC, Dolphin's Z16 encoder notes the reversed byte order]: the IA8 view
  has I = depth bits 23..16 and A = 15..8. Port: same (**match**; consistent with the CLAUDE.md
  "EFB depth copies" note and the game's swap-table setup).
- Intensity copies (I4/I8/IA4/IA8) use Y = `(66R + 129G + 25B + 128) >> 8 + 16` [D-SW, "determined
  by hardware testing"]. Aurora uses the float BT.601 form (0.257/0.504/0.098 + 16/255), then
  truncates for 4-bit formats. Difference at most 1 level. LM's only intensity copy is the
  invisible event-5 IA4 copy. **Minor mismatch, not visible in LM.**
- RGB565/RGB5A3 copy encoders truncate; RGB5A3 opaque at alpha >= 0xE0. Port: **match**.

### 3.5 Alpha test and Z compare location (ZCOMPLOC)

Hardware [OGC, PAT-Z, D-PS]: with Z compare before texturing (`GXSetZCompLoc(GX_TRUE)`) the Z
test and the Z **update** happen before the TEV and the alpha test. libogc's doc: "when alpha
compare is used, Z buffering must be done after texturing"; the Z-texturing patent says the alpha
compare result can mask Z writes "if desired", i.e. only with late Z. Dolphin implements early Z
by forcing early fragment tests whenever the alpha test can fail, so the depth of alpha-rejected
pixels is written.

LM (trace, distinct callers): early Z with an alpha test that can fail and Z update on is set by
- room geometry (`fn_80015CF4`: `GXSetZCompLoc(1)` at 0x80015D64 next to `GXSetAlphaCompare(GREATER
  0 OR GREATER 0)`; the same function also sets late Z at 0x80015D70 for other materials),
- Luigi/J3D draws (0x80059894, 0x80059A90 early; ZMode update at 0x8005A4C4/0x8005A584; alpha
  compare GREATER 0 at 0x8005A5AC),
- a few others (0x8012CBF4, 0x80130534, 0x80118F5C (Z update off: harmless));
- 0x801BF8D4 (J3D default, 111k calls) sets early Z but its alpha compare is ALWAYS: no effect.

Port: Z is written only for pixels that pass the alpha test, in every mode (like late Z).
RENDERING.md records an experiment that wrote Z before the alpha test and "broke Luigi's face
decals and pillars", from which it concluded the hardware does not do it.

**Open, conflicting evidence.** The documented behaviour (and Dolphin, which renders LM correctly)
says early-Z draws write Z for alpha-killed pixels. The port's earlier experiment says that looks
wrong, but it was a depth pre-pass whose exact scope is not recorded (a pre-pass over more draws
than the early-Z ones, or ahead of earlier draws, would break decals without saying anything about
the hardware). If the game authors set early Z only on materials without transparent texels, the
two models give identical pictures, which would explain why nobody notices on either. A decisive
test is cheap (section 5, item 2). Visibility if it does matter: holes/halos where a cut-out
early-Z draw occludes things drawn after it.

### 3.6 Blending, dither, 6-bit EFB, destination alpha

Hardware [D-SW]: blend factors are widened `f + (f >> 7)` and the result `(src*sf + dst*df) >> 8`
is truncated, then stored truncated to 6 bits per channel (RGBA6); dither (when on) is added to
colour only before the truncation; dst alpha replaces the stored alpha after blending; Dolphin
notes that hardware lets alpha = 1 pass the alpha test but "does nothing in blending" (factor
1+0 = 1/256, truncated away).

Dither pattern: Dolphin (both renderers) uses a 2x2 matrix `0 2 / 3 1` (in 1/4 of a 6-bit step).
libogc's doc for `GX_SetDither` says "A 4x4 Bayer matrix is used". **Settled on `hw-details`
(section 9.1): both are right.** A 4x4 Bayer matrix applied to an 8-bit value that loses 2 bits
(RGBA6) produces exactly the 2x2 pattern `0 2 / 3 1`; the two only differ for RGB565's 5-bit red
and blue. Aurora 0180 uses the 4x4 matrix (bit-identical output for RGBA6).

Port: shader output is quantized to RGBA6 with the 2x2 dither when the draw is unblended (0010);
**blended draws are blended at 8 bits by the GPU and never truncated to 6 bits**; alpha of blended
draws stays 8-bit (SMS's patch 0113 quantizes the replace-blend cases and alpha nobody reads).

Measured (frame dumps 2500-3300, nursery, flashlight, GBH): 4.2-7.7% of the pixels are not RGBA6
values, and they are **all in the 2D overlays drawn last** (Game Boy Horror, the heart/health
counter, the room-name caption); the 3D scene is 100% RGBA6 because LM redraws the whole scene
from its own RGB565 copy every frame (`fn_8005F8B8`). So in LM the 8-bit blending shows only as
up to 3 levels on the edges and translucent parts of HUD elements. **Mismatch, very low
visibility in LM; higher for SMS** (water, translucent effects drawn after any scene copy).

### 3.7 Fog

LM always uses `GX_FOG_NONE`. Not exercised; the port's fog code (range LUT from RENDERING.md)
was not re-audited. Dolphin's fog model for reference [D-PS]: perspective eye distance from
24-bit Z via A/(B - (Z >> shift)), range adjustment `sqrt(x^2 + k^2)/k` from the 10-entry table,
exp/exp2/backward functions as powers of 2, integer blend `(prev*(256-f) + col*f) >> 8`, fog after
the Z texture. Relevant for SMS (fog is used there).

### 3.8 Rasterizer details

- Pixel centre [D-PC]: Dolphin PR #306 (2014) placed the non-AA pixel centre at 7/12 instead of
  1/2, citing a hardware test; Dolphin PR #12972 (2024, open) argues no offset is needed and
  reported mixed results. Aurora used the PC convention (1/2). **Settled on `hw-details`
  (section 9.2):** neither a plain 1/2 nor a uniform 7/12. Vertex positions snap down to the
  1/12-pixel grid and pixels are sampled at 6/12; that gives the measured 7/12 coverage threshold
  while whole-pixel geometry (LM's scene redraw) is still sampled at texel centres, as a hardware
  FIFO capture shows. Aurora 0180 snaps the positions.
- Projective texcoords with q = 0: Dolphin special-cases them (`xy/2`, clamped, "a GameCube special
  case"). Aurora divides. Only matters on the projector's plane; not seen in LM.
- Line width 6 = 1 px (units of 1/6 px; libogc's "1/16" is a documentation error, the SDK and
  Dolphin divide by 6). **Match.**
- Z texture adders do not clamp [PAT-Z]; Aurora (0008) wraps at 24 bits. **Match.**

## 4. CLAUDE.md "Next steps" checked against the sources

**EFB depth copies (Z16 read as IA8).** Agrees with the documented copy encoding (section 3.4):
I = bits 23..16, A = 15..8. Do not change the byte order.

**Shadow compare precision** (texgen depth vs rasterized Z, bilinear filtering of the packed
textures). The hardware compares 16-bit keys built from 8-bit filtered bytes; the port does the
same in the integer TEV. The only differences found are the filter weights (7-bit truncating vs
GPU 8-bit + rounding: at most 1 in the low byte, i.e. 1/65536 of the light's depth range) and float
vs 24-bit fixed-point Z (sub-LSB). Neither can make whole regions such as the mouth or hose flip
from frame to frame. Not a plausible cause of the flicker.

**Luigi's mouth/hose flicker.** New evidence [D-12903]: the same symptom in Dolphin (LM intro,
mouth flickers and is partly see-through, all backends, native resolution) was attributed by a
Dolphin developer to **dual-core timing** (the GPU thread reading memory at a different time
relative to the CPU than on hardware) and closed as a duplicate of the "DualCore Timing Mega
Issue". The port has the same structure: Aurora's "FIFO processor" is a separate thread
(`lib/gx/fifo.cpp`, `ProcessingMode::Thread`), and vertex arrays are passed as pointers into guest
memory (`GXSetArray` with size 0, sized and read when the worker processes the draw; display
lists likewise). If the game rewrites an array (e.g. CPU-skinned or morphed vertices of Luigi's
face, which are drawn in the shadow caster pass and again in the main pass) between issuing a
draw and the worker reading it, that draw gets the newer data: rare, random, timing dependent.
This fits the symptom better than the shadow-precision suspects. Suggested check in section 5.

**Audio second pass** (CLAUDE.md: reverb routing, filters not applied, L/R order): already done
on main (`b3a20b7`, AUDIO.md) and confirmed by the comparison in section 6; the CLAUDE.md item is
stale.

## 5. Candidate fixes (unimplemented, prioritised)

| # | Candidate | Evidence | LM visibility | Cost / risk |
|---|-----------|----------|---------------|-------------|
| 1 | **Mouth/hose flicker: rule out the CPU/GPU memory race.** Test: run with the FIFO processed synchronously on the guest thread (`kProcessingMode = ProcessingMode::Inline` in `lib/gx/fifo.cpp`: processed at publish), or snapshot `GXSetArray` data / display lists at call time, and watch Luigi's face in the intro and the nursery for flicker (`LM_FRAME_DUMP` every frame, diff mouth region). If it fixes it: snapshot arrays on the guest thread (copy on GXSetArray/draw, like the interpolation tape already does) | Dolphin issue 12903 (dual-core timing), section 4 | the reported bug | test: low; fix: moderate (memory copies per draw) |
| 2 | **Decide ZCOMPLOC + alpha test.** Test: count pixels the alpha test rejects in draws that have early Z and Z update (a debug colour for those pixels, or an occlusion query). Zero in normal scenes = the models are equivalent for LM, close the item. Non-zero = render those draws as a depth-only pass without discard followed by the colour pass (Z EQUAL/LEQUAL, no Z write), only for early-Z + alpha-test + Z-update draws, and compare Luigi's face and the room cut-outs | section 3.5 | unknown (none to small) | test: low; fix: one extra depth pass for the affected draws |
| 3 | Aurora SDK fixes: `GXInitLightSpot` SHARP and RING1 `a0`, `GXInitLightDistAttn` `ref_br <= 0` | section 3.2 (SDK source in this repo) | none in LM | trivial; would change pipeline-independent uniforms only (no precache impact) |
| 4 | 6-bit truncation (and the `f + f>>7` / truncating blend arithmetic) for blended draws | section 3.6 | HUD edges only (up to 3 levels) | high for an exact fix (WebGPU has no framebuffer fetch: needs an EFB copy per blended draw or a programmable-blend pass); a cheap partial step is SMS's 0113 (replace-blends + unread alpha). 0113 changes `ShaderConfig` -> re-run the pipeline sweep |
| 5 | ~~Pixel centre 7/12~~ **done** (`hw-details`, Aurora 0180: 1/12-pixel position snap, section 9.2) | section 3.8 | edges and textures of 3D geometry move by up to 1/12 px; whole-pixel 2D unchanged | done |
| 6 | Integer lighting accumulation (`lacc` rounding, `(mat*(lacc+lacc>>7))>>8`) | section 3.2 | not visible (<= 1 level) | small; pipeline configs unchanged, shaders change -> re-run the sweep |
| 7 | 7-bit truncating bilinear weights | section 3.3 | not visible | needs manual filtering in the shader: costly, not worth it |
| 8 | ~~Dither matrix 2x2 vs 4x4~~ **done** (`hw-details`, Aurora 0180: 4x4, identical output for RGBA6, section 9.1) | section 3.6 | none in LM | done |
| 9 | Intensity copies with the integer Y coefficients | section 3.4 | none in LM | trivial |
| 10 | AFC: drop the last partial frame of non-looping samples (AUDIO.md known gap) | microcode | inaudible | trivial |

Items 3-9 are generic Aurora changes shared with SMS; item 1's array snapshot would live in the
port's HLE GX (also shared). None of them were made on this branch. Pipeline-config impact (for
the `pipeline-sweep-2` sweep): item 4 (0113's `efbAlphaQ`) and item 2 (a new depth-only variant)
add pipeline-config state and need a re-sweep; items 5 and 6 keep the config keys but change the
generated WGSL of existing configs (re-sweep advisable so the shipped list is compiled with the new
shaders); items 1, 3 and 9 do not touch pipeline configs.

## 6. Audio: research findings

LM's DSP microcode is the "Zelda" family in Dolphin's HLE [D-Z] (LM = CRC 0x42F64AC4, flags
LIGHT_PROTOCOL, NO_CMD_0D, WEIRD_CMD_0C). The port's stand-in (`audio_dsp.cpp`) was written from a
disassembly of LM's own microcode (AUDIO.md). Comparison:

| Topic | Dolphin's Zelda HLE | Port | Status |
|---|---|---|---|
| Effect lines ("reverb") | 4 parameter blocks; per frame a 0x50-sample block from a ring in main RAM prefixed with the last 8 samples; 8-tap FIR before routing if enable bit 0, after if bit 1; two destinations with 1.15 volumes; ring size in 0x50-sample units; written back after the voices | the same (AUDIO.md "Effect lines") | **match** |
| Output fold of the surround bus | LIGHT_PROTOCOL: Dolphin skips its per-voice "Dolby" quadrant panning | the microcode's output fold (S on L, NOT S on R, centre 0.707) is a different, later step; the port follows the disassembly | no conflict (different features) |
| Voice one-pole filter | low-pass `y += (x - y') * g` with a 9.7 coefficient, applied when non-zero | JAudio calls it "distortion" (`DSP_SetDistFilter`, +0x150): `y = sat(y' + (x - x') * g/128)`, same form | **match** (naming only) |
| Voice FIR | parsed, never applied ("TODO") | applied per the microcode (uc 0x01E1) | port more complete |
| Voice IIR | biquad on previous inputs/outputs, no current-input term, skipped for pass-through coefficients | same | **match** |
| Resampler | 4-tap, 64-phase table from the setup command, nearest sample from ratio 4.0 | same | **match** |
| AFC | 9-byte (4-bit) and 5-byte (2-bit) blocks; end of a one-shot: done + zeros; loop: restore loop history | same; LM's microcode only has the 9-byte path wired (AUDIO.md) | **match** |
| Volume ramp | step = delta / 80 over the whole 0x50-sample block | delta/32 applied every 2nd sample, done after 64 samples (from the disassembly, uc 0x05F6/0x011E) | **differs**; the port follows LM's code, Dolphin's HLE is a simplification (16-sample difference in ramp length, inaudible) |
| Stop fade | targets halved each frame, done when all reach 0 | same | **match** |

Conclusions: the three open audio items in CLAUDE.md are resolved on main; nothing found that is
both confirmed and audible. Remaining known gaps are those in AUDIO.md (oscillator sources unused
by LM, the partial last AFC frame, the untested options-menu -> F1 sync). The user's "sounds a
bit off at times" is not explained by any documented microcode behaviour the port lacks; next
step would be an A/B against a console recording of the same scene (title screen thunder is
deterministic and a good candidate).

## 7. Notes for Super Mario Sunshine (shared Aurora)

- Section 3.2 SDK bugs (`GXInitLightSpot` SHARP/RING1, `GXInitLightDistAttn` brightness 0) are in
  the shared Aurora; check whether SMS calls them with those functions.
- DIVIDE_2 rounding in the integer TEV (3.1): settled on `hw-details` (truncation, Aurora 0180,
  section 9.3). SMS uses it (Bianco Hills' alpha-tested cut-outs, the windmill blades: alpha `(APREV + 128) / 2` against an
  alpha test of 128).
- 6-bit truncation after blending (3.6) matters more for SMS than for LM.
- Fog (3.7): SMS uses fog; Dolphin's integer fog blend and power-of-two exp functions are the
  reference to audit Aurora's fog against.
- The CPU/GPU memory race (section 4) applies to any game on Aurora's threaded FIFO.

Proposed SMS `docs/BACKPORT.md` row (research note, nothing to port yet):

| # | What | Where | LM uses it? | Status |
|---|------|-------|-------------|--------|
| next | From LM (research, accuracy-research): hardware-accuracy audit vs Dolphin/SDK/patents; Aurora SDK bugs found: `GXInitLightSpot` SHARP `a0` lacks `/(1-cr)^2`, RING1 `a0` has the wrong sign, `GXInitLightDistAttn` should treat `ref_br <= 0` as OFF; integer TEV DIVIDE_2 rounding unverified (Aurora rounds, Dolphin truncates); blended draws not truncated to RGBA6; threaded FIFO reads guest arrays late (Dolphin dual-core class) | LM `docs/ACCURACY.md` | n/a (research) | pending: check which apply to SMS |

## 8. Defect / verification table

| ID | Area | Finding | Evidence | Status |
|----|------|---------|----------|--------|
| A1 | TEV | ADD/SUB rounding, compares, konst, wrap: match Dolphin's hardware-tested model | algebra vs [D-PS]; audit (LM: scale x1/x2, ops ADD/SUB/GR16) | verified match |
| A2 | TEV | DIVIDE_2: truncating lerp, floor halving (was: rounded) | [HW-TEV] hardware test, section 9.3 | fixed (0180), not LM |
| A3 | Lighting | SPOT/SPEC/NONE formulas, normal normalization, emboss, SRTG | [D-LS, D-VS, SDK] | verified match |
| A4 | Lighting | float vs integer accumulation | [D-LS] | minor, invisible |
| A5 | SDK | `GXInitLightSpot` SHARP/RING1 a0 wrong | [SDK] GXLight.c vs Aurora GXLighting.cpp | defect, not LM |
| A6 | SDK | `GXInitLightDistAttn` ref_br == 0 not OFF | [SDK] | defect, not LM |
| A7 | Textures | CMPR, I4, IA4, RGB5A3 decode | [D-TD] PR #4656 (Wii test) | verified match |
| A8 | Textures | bilinear weight precision / truncation | [D-SW] | minor, invisible |
| A9 | Copies | filter, overflow, gamma, Z16 byte order, RGB565/RGB5A3 | [D-TC, D-SW] | verified match |
| A10 | Copies | intensity Y coefficients float vs integer | [D-SW] | minor, not LM-visible |
| A11 | Z | early Z + alpha test: Z of rejected pixels | [OGC, PAT-Z, D-PS] vs port experiment; LM trace (rooms, Luigi) | open, test proposed |
| A12 | EFB | blended draws not truncated to 6 bits | frame dumps: 4-8% of pixels, all HUD | mismatch, low |
| A13 | EFB | dither 4x4 Bayer (= 2x2 `0 2 / 3 1` for RGBA6) | [OGC] + section 9.1 algebra | fixed (0180), no LM change |
| A14 | Raster | positions snapped to 1/12 px, samples at 6/12 | [HW-RAS], [D-12844], [SDK] section 9.2 | fixed (0180), medium confidence |
| A15 | Raster | line width units, Z texture wrap | [SDK, D-PS, PAT-Z] | verified match |
| A16 | Flicker | mouth/hose: threaded FIFO reads guest arrays late | [D-12903], `lib/gx/fifo.cpp` | confirmed and fixed (draw-time vertex snapshots, RENDERING.md "Vertex snapshots") |
| A17 | Flicker | shadow compare precision | section 4 | ruled out as cause |
| B1 | Audio | effect lines, filters, resampler, AFC, stop fade | [D-Z] | verified match |
| B2 | Audio | mixer volume ramp length | [D-Z] vs disassembly | differs from Dolphin HLE, port follows LM code |
| B3 | Audio | CLAUDE.md audio next step | AUDIO.md, b3a20b7 | stale (done) |

Interpolation, widescreen and enhanced lighting were not retested: no code changed on this branch.

## 9. Three hardware details settled (branch `hw-details`, Aurora 0180)

Research pass 2026-10-11 on the three items section 5 left open (5, 8 and A2). Same rules as above:
Dolphin, hwtests and libogc are GPL and were read only to understand the hardware; no code was
copied. One generic patch, `patches/aurora/0180-flipper-hw-details.patch`, identical in LM and
SMS (applies on both chains). Each part has an environment switch for A/B, read once at startup:
`AURORA_PIXEL_SNAP=0`, `AURORA_DITHER_4X4=0`, `AURORA_TEV_DIV2_TRUNC=0` restore the previous
behaviour (verified: with all three at 0 the new build's frames equal the old build's where the
scene is deterministic). No pipeline-config change (no re-sweep needed); generated WGSL changes.

### 9.1 Dither matrix: 4x4 Bayer, which is the 2x2 `0 2 / 3 1` for RGBA6 (confidence: high)

- [OGC] `GX_SetDither`: "A 4x4 Bayer matrix is used"; [OGC-SO] `GX_SetScissorBoxOffset`: "Since
  the dither matrix is 4x4, a yoffset of -2 shifts the image down by 2 lines with respect to the
  matrix" (these docs follow Nintendo's SDK manual; the offset remark only makes sense with a
  period of 4). So the matrix is 4x4, indexed by EFB pixel (after the scissor-box offset).
- [D-PS]/[D-SW]: Dolphin dithers with the 2x2 matrix `0 2 / 3 1` (in 8-bit units added to
  `c - (c >> 6)`, then truncated to 6 bits). No hardware test of dithering exists in hwtests (its
  RGBA6 TEV path is disabled, "doesn't quite work, yet").
- The two agree. The standard 4x4 Bayer index `B4(x, y) = 4 * B2(x, y) + B2(x >> 1, y >> 1)`
  (B2 = `0 2 / 3 1`) in sixteenths of an output step, added to an 8-bit value that keeps 6 bits
  (2 bits dropped, fractions 0, 4, 8, 12 sixteenths), rounds up exactly where `floor(B4 / 4) =
  B2(x, y)` does, i.e. Dolphin's 2x2 result, pixel for pixel. The 4x4 pattern only shows where 3
  bits are dropped: RGB565's red and blue (8 levels).
- Port (0180): the 4x4 matrix in the shader's EFB quantization; RGBA6 and RGB565 green are
  bit-identical to before; RGB565 red/blue now dither with 8 levels instead of 4. Alignment: EFB
  pixel (render pixel at a higher render scale). SMS's 0176 resolve pass (6-bit blends) keeps its
  2x2 formula, which is the same thing for 6-bit channels. Neither game renders into RGB565 with
  dither in the scenes checked, so **no visible change in LM or SMS**.

### 9.2 Pixel centres: positions snap down to 1/12 pixel, samples at 6/12 (confidence: medium)

Evidence:
- [PAT-AA]: each pixel is a 12x12 subpixel grid; the three sample points per pixel are given in
  1/12-pixel units. [SDK] `GXSetCopyFilter` with `aa = GX_FALSE` writes all sample locations as
  6 (`0x666666`), and every non-AA render mode uses `{6, 6}` for all 12 points: the non-AA sample
  sits at 6/12 = the pixel centre on the 12-grid. The SDK adds 340 to the viewport origin (so
  screen positions are positive for fixed-point conversion).
- [HW-RAS] (run on a console): a left edge covers the pixel's sample while its x is at most
  0.583328 (the float just below 7/12 at x = 342.58) and not from the next float up; a quad only
  2e-5 wide covers the sample only when it straddles 7/12. Hence Dolphin PR #306's "pixel centre at
  7/12" (a uniform +1/12 shift of every vertex, still in Dolphin today).
- [D-12844] (hardware FIFO player): a game that copies 256x256 of the EFB and draws it back 1:1
  with LINEAR filtering gets no blur and no wrap-around on the console, while Dolphin's 7/12 shift
  blurs and wraps it; bisected to PR #306. So whole-pixel geometry is sampled exactly at texel
  centres on hardware: attributes are not evaluated 1/12 off. Dolphin PR #12972 (open) removes the
  offset for that reason, but then loses the measured coverage threshold.
- One rule fits all three: **the setup unit truncates screen positions to the 1/12 grid (floor) and
  samples at 6/12, edges inclusive top-left**. A left edge at x snaps to floor(12x)/12 and covers
  the 6/12 sample iff x < 7/12 (the hwtest threshold; the tiny quad snaps to zero width unless it
  straddles a grid line); whole-pixel (and half-pixel) positions do not move, so 1:1 blits stay
  sharp. Dolphin's software rasterizer, "adjusted to match hardware output", also biases positions
  by -9/16 (about the same threshold on its 1/16 grid). Medium confidence: no test measured a
  sloped edge or an attribute value at a fractional position; truncation vs. a coverage-only offset
  is inferred, not observed.
- Multisample (3-point AA, RGB565_Z16): samples at the programmed `(x, y)/12` positions on the
  same grid, coverage per sample, Z per sample from the quad's centre plane ([PAT-AA]); colour is
  computed once per pixel (position not documented; presumably the pixel centre). Aurora does not
  emulate the 3-point mode (its MSAA enhancement is the PC's), and neither game uses it.

Port (0180): the vertex shader snaps `out.pos` (triangles and quads; lines/points and the enhanced
shadow-map casters are not snapped) to floor(12 x)/12 in the logical (EFB) pixel grid, with a
1/256-step tolerance so float noise on exact grid positions does not drop a step; coverage and
attribute interpolation then follow the snapped positions, at any render scale (positions move by
at most 1/12 native pixel). The uniform block gained the logical viewport origin (and a viewport
`top` change now dirties the uniforms).

### 9.3 TEV DIVIDE_2: no rounding constant, floor halving (confidence: medium-high)

- [HW-TEV] (run on a console; the randomized test draws A, B, C from -1024..1023, D = 0, scale
  from all four values including DIVIDE_2, bias ZERO/ADDHALF/SUBHALF, ADD/SUB, clamp on/off): the
  expectation that passes is `lerp = (A*(256-C') + B*C' [<< s] + r) >> 8` with `r = 128` (ADD) /
  `127` (SUB) for x1/x2/x4 but **`r = 0` for DIVIDE_2**, then `D [<< s] +- lerp + bias [<< s]`,
  then `>> 1` for DIVIDE_2 (arithmetic, i.e. floor), then clamp to 0..255 or -1024..1023 (the
  10/11-bit signed range). Dolphin's shader generator implements the same and still carries a
  "TODO: is the rounding bias still added when the scale is divide by 2?", which the hwtest
  answers for D = 0. The patents describe the scale stage (0.5/1/2/4 after the bias) but no
  rounding.
- So: the lerp is truncated, the sum (D + bias +- lerp) is halved with truncation toward minus
  infinity (not round-half-up), and the clamped 8-bit output is the clamp of that. Untested by
  anyone: D != 0 with DIVIDE_2 (the formula is linear in D, so a different carry would be a
  separate adder stage; assumed not).
- Compare modes (bias field = 3): the scale field selects the compare width (R8/GR16/BGR24/RGB8)
  and the op GT/EQ; `D + (A cmp B ? C : 0)`, no lerp, no rounding, no scale, clamp applied
  (hwtest compare block, [PAT-TEV] register table). Port: already so (match).
- Port (0180): DIVIDE_2 in the integer TEV computes `(D + bias +- floor(lerp / 256)) >> 1`
  (previously `(256 D + 256 bias +- lerp + 256) >> 9`, i.e. rounded). The float TEV (integer TEV
  off) is unchanged. LM never uses DIVIDE_2. SMS does: Bianco Hills' alpha-tested cut-outs, the windmill blades (stage 1 alpha
  `(APREV + 128) / 2` unclamped, alpha test `>= 128`): the cut-out threshold moves from texture
  alpha 127 to 128, i.e. a few hundred edge pixels per frame.

### 9.4 Verification (frame dumps, native, Original 30, 4:3, enhancements off)

Old build vs new build and the new build with switches (`lmrecomp`/`smsrecomp` from this branch;
scratch APPDATA, card and cache). LM's 3D scenes are not frame-identical run to run (dust, Luigi's
idle animation: two runs of the old build differ in 8-48k pixels per nursery frame), so the
changes were isolated with the switches and with the frames that are deterministic:

- LM nursery (`LM_START_MAP=2 LM_START_APPEAR=1`, frames 2600-2900): GBH frames 2740/2760
  (deterministic: old = old rerun = new with all switches 0) differ in 10.7k / 16.4k pixels with
  the snap, 44-48 % of them on edges, the rest low-amplitude texture shifts on the 3D Game Boy
  Horror model (mean 4-5 levels). Title / file select: unchanged (frames 200, 300) or <= 3 levels
  on the slid-in file panels (frame 400); opening crows (frame 1400): branch and crow edges and the
  bark texture, sky untouched. RGBA6 share of pixels unchanged (92-100 %).
- Dither and DIVIDE_2: no LM pixel changes (RGBA6 only, no DIVIDE_2).
- SMS: see SMS docs/ACCURACY.md "Hardware details" (plaza 1,0 and Bianco 2,0).
- Enhanced lighting (nursery, master on): renders as before (flashlight pool, volumetric beam,
  shadows), no shader/validation errors with the patch; differences vs the old build only at the
  scale of the run-to-run noise plus the snap's edge shifts.

Evidence images (agent scratch, not in the repo): `scratchpad/hw/report/`.

## 10. Implemented fixes (branch `accuracy-fixes`, 2026-10-11)

Aurora patches (same file names as SMS; `0176` and `0179` are LM adaptations):

| Patch | What | LM effect / verification |
|---|---|---|
| 0123 | per-draw debug log (`AURORA_DRAW_LOG`, `_FRAMES`, `AURORA_DRAW_SKIP(_MATCH)`); frames are Aurora's recorded frames (about the present count, not the `LM_FRAME_DUMP` number: SMS's 0201 mapping did not apply cleanly and was not taken) | debug only |
| 0170 + 0179 | early Z + failing alpha test writes Z (candidate 2): such draws are split into a depth-only and a colour pass. **0179**: F1 > Visuals > Rendering "Early depth for cut-out textures" (`early_z_alpha_depth`, `GX_AURORA_ACCURACY_EARLY_Z`, on; `AURORA_EARLY_Z_SPLIT=0/1/ab` overrides), and a fix: `draw_prim` merges the next draw into the last draw command, so geometry following a split draw got only the colour pass without a depth write (Game Boy Horror hand model drawn through, parts missing). No draw merges into a split one now | with the fix, nursery dumps (GBH open, flashlight, rooms) are identical to the split off (max 3-4 levels = 6-bit blending dither): the game's early-Z cut-out depth makes no visible difference in the scenes tested |
| 0172 | direct texture lookups honour the texcoord's SU scale | no change in LM frames (boss map byte-identical with only 0172/0173/0178 active) |
| 0173 | GX lines widen along the minor axis | LM draws 1-pixel lines only |
| 0174 | `AURORA_FIFO_INLINE=1` (debug) | for the mouth flicker test |
| 0175 | Dawn implicit device synchronization (`AURORA_NO_DEVICE_SYNC=1` for A/B) | crash fix, no image change |
| 0176 | 6-bit blending (candidate 4): F1 > Visuals > Rendering "6-bit blending" (`efb_blend_6bit`, on). LM chain has no 0113, so replace blends (ONE/ZERO, logic COPY) keep their in-shader quantization and only real blends are resolved after the pass; the precache rebuild maps listed blended RGBA6 configs to the 0176 variant (no re-sweep needed) | GBH / HUD edges change by at most 3-4 levels |
| 0177 | frame submitted before the swap-chain acquire (`AURORA_LATE_SUBMIT=1` for A/B) | readbacks land earlier |
| 0178 | SDK light init: `GXInitLightSpot` SHARP `a0` over `(1-cr)^2`, RING1 `a0` negative, `GXInitLightDistAttn` `ref_br <= 0` = off (A5, A6), as LM's own SDK (`decomp/libs/dolphin/src/gx/GXLight.c`) | LM uses none of these cases: no change |

Verification: 30 fps, 4:3, render scale 1 in the nursery, foyer, lit ballroom (`LM_ROOMS_LIT`) and
the Boolossus map (11); enhanced lighting fully on (per-pixel, light shadow maps, AO, ghost and
world lights) in the nursery and ballroom; 120 fps + 16:9 + render scale 2 with `LM_INTERP_TEST=3`
(0 stale pixels, 0 snapped/unpaired values); shipped exe fresh setup (`--setup-auto`) to the foyer.
"Before" = the same build with `LM_ACCURACY=0x07 AURORA_LATE_SUBMIT=1 AURORA_NO_DEVICE_SYNC=1`.
These scenes are not frame-deterministic between runs (ghost/boss positions, lightning timing);
only the Game Boy Horror frames are, and there the runs differ by at most 4 levels.

Not done: the mouth/hose flicker fix (candidate 1). `AURORA_FIFO_INLINE=1` is available for the
test; at 30 fps draws still read guest arrays from Aurora's FIFO thread, while above 30 fps the
interpolation tape already copies the arrays at draw time. The 0170 depth-only pipeline variants
are not in `data/pipelines.bin` (built on first use).
