# PC enhancements in the recompiled build

The recompiled build (`lmrecomp`, see [RECOMPILER.md](RECOMPILER.md)) runs the retail game
code unchanged. Enhancements therefore live in the host layer: the GX HLE
(`src/recomp/hle_gx.cpp`), the frame handoff (`src/recomp/frame.cpp`), the VI emulation
(`src/recomp/hle_vi.cpp`), and hooks on individual recompiled functions
(`tools/recomp/hooks.txt` plus `HLE(name)`, with `callOriginal(c, name)` to wrap the
retail body instead of replacing it).

| Feature | Status |
|---------|--------|
| Arbitrary resolution | Works. Aurora renders at window size; EFB copies scale with it. |
| Widescreen / ultrawide | Works: Hor+ 3D, 4:3 or edge-anchored HUD, culling widened. Limitations below. |
| Field of view | Works (F1 > Visuals > Field of view); culling follows. |
| High frame rate (interpolation) | Works: 60 fps or display rate, game logic stays 30 Hz (section 3). |

## 1. Arbitrary resolution

The game always renders a 640x480 logical frame. Aurora maps it onto its content
framebuffer, which matches the window size:

- `GXSetViewport` and `GXSetScissor` take logical coordinates. Aurora multiplies them by
  `render size / 640x480` (`map_logical_viewport` and `map_logical_scissor` in
  `extern/aurora/lib/gx/gx.cpp`).
- `GXSetTexCopySrc` goes through the same scissor mapping. The copy destination is
  scaled the same way (`scale_copy_dst` in `lib/dolphin/gx/GXFrameBuffer.cpp`). An EFB
  copy therefore has native resolution, and anything that samples it with normalized
  texture coordinates stays correct. This covers the full-screen copies that the
  foyer, flashlight and ghost effects make each frame (`copy src (0,0 640x480)` in the
  trace), and the 128x128 render-to-texture in `fn_8006033C`.
- Under the widescreen STRETCH policy the scale factors differ in x and y. A 128x128
  logical copy at 1280x720 becomes a 256x192 texture. Its content is still correct,
  because the viewport it was drawn with was stretched by the same factors. It only has
  more horizontal than vertical resolution.

Nothing in the port assumes 640x480 pixels. Captures at 960x720, 1280x720, 1680x720,
1707x960 and 1920x822 all render correctly.

## 2. Widescreen and ultrawide

### Options

| Option | Meaning |
|--------|---------|
| `--aspect auto` (default) | Follow the window shape. A 4:3 window behaves exactly like the original. |
| `--aspect 4:3` | Original presentation, pillarboxed by Aurora in a wider window (FIT policy). |
| `--aspect 16:9`, `21:9`, `<w>:<h>`, `<ratio>` | Fixed aspect. The window opens at that shape (960 px tall). |
| `--window <w>x<h>`, `--fullscreen` | Initial window size, or fullscreen. |
| `--ws-backgrounds pillarbox\|stretch\|zoom` | Full-screen 4:3 pictures (title backdrop, file-select clouds): keep them 4:3 with bars (default), stretch them, or enlarge them to the full width and crop the top and bottom. |
| `LM_WS_TRACE=1` (env) | Logs each frame's widescreen decisions whenever they change: draw path, projection type, viewport, result, plus EFB copies. |
| `LM_CACHE_DIR=<dir>` (env) | Uses a separate shader/pipeline cache. Useful when several builds share one machine. |

### How it works (`src/recomp/widescreen.*`, `src/recomp/hle_gx.cpp`)

`s = (4/3) / display aspect` (`ws::factor()`, below 1 when the display is wider). It is
read on the host thread before each guest frame, so window resizes take effect on the
next frame.

1. **Stretch everything first.** `AuroraSetViewportPolicy(AURORA_VIEWPORT_STRETCH)` maps
   the logical frame onto the whole window.
2. **Track the guest's projection, viewport and scissor; send the projection lazily.**
   `GXSetProjection` only records the matrix. Before each draw, `prepareDraw()` sends one
   of three variants:
   - *plain*: unchanged, so the draw is stretched to the window.
   - *scaled*: row 0 of the projection multiplied by `s`, so `x_ndc' = s * x_ndc`. For a
     perspective projection this is **Hor+**: the vertical FOV is kept and the
     horizontal FOV grows. For an orthographic projection it keeps 640x480 2D at **4:3,
     centered**. A partial-width scissor is moved by the same transform.
   - *zoom*: row 1 divided by `s`, for the `zoom` background mode.

   Draws reach Aurora through `GXBegin`, `GXCallDisplayList` and `GXDraw*`, and also
   through raw FIFO writes from inlined game code. J3D inlines its display-list calls
   and matrix loads. Before the first raw FIFO write after a projection or viewport
   change, `hw::fifoWrite` brings the projection up to date (`sRawCheck`).
3. **Choose per draw.**
   - Draws into a viewport that does not cover the logical width (render-to-texture,
     picture-in-picture) are *plain*. They then stay consistent with the EFB copy that
     reads them back.
   - Perspective draws into the full-width viewport are *scaled* (Hor+).
   - Orthographic immediate-mode draws into the full-width viewport are **held**:
     `GXBegin` and the FIFO writes that follow are buffered until the draw ends. Then
     `coversScreen()` decodes the vertex positions from the mirrored vertex descriptor,
     VAT, position array and position matrices, and projects them to NDC x.
     - A draw that spans the whole width and samples no texture, or only an EFB copy, is
       a fade or a screen-space effect. It stays *plain* so it lines up with the
       stretched EFB.
     - A draw that spans the whole width and samples an ordinary texture is a 4:3
       background picture. It follows `--ws-backgrounds`.
     - Everything else is HUD or text: *scaled*, so 4:3 and centered.
   - Orthographic display lists are *scaled*.
4. **Widen culling.** `fn_80008D58(camera, top, bottom, left, right, near, far)` builds
   the camera's four side culling planes. Every 3D projection setter ends with it
   (`fn_80008654`, `fn_800087D0`, `fn_80008928`). The hook in `widescreen.cpp`
   multiplies `left` and `right` by `1/s` when the camera viewport is full-width, then
   calls the retail body. Without this, objects at the screen edges pop in.

Overlays stay in place. Game code that projects a 3D position to the screen (for
markers over ghosts and similar) uses the 4:3 projection and gets a 640-space x.
Drawing that in the centered 4:3 2D space gives `s * x_ndc`, which is exactly where the
Hor+ projection put the object.

### Field of view

`settings().video.fovScale` multiplies the vertical FOV of full-screen perspective
projections. `ws::fovRatio(m11)` turns it into a scale for rows 0 and 1 of the projection
(`cot(f/2) / cot(k*f/2)`), applied together with Hor+ in the *scaled* variant, so a 4:3
window with FOV 1.0 is still untouched. The `fn_80008D58` hook scales all four culling
sides by the inverse, using the camera's `fovY` (`+0x90`).

### HUD anchoring

With "Keep HUD at 4:3" off, the in-game HUD moves to the display edges instead of the 4:3
safe area. Menus (title, file select, options) are J2DScreens and keep their layout. The
gameplay HUD is either drawn by two game functions as J2DScreens (`fn_80056F64`,
`fn_8003E3D0`), or as panes and pictures that game code draws without a screen. The health
meter and the room name are in the second group. `J2DScreen::draw` is hooked to know which
case applies (`ws::inHud()`).

For those draws, the held-draw decoder already knows the NDC x extent in the 4:3 frame.
A draw entirely in the outer 30% on one side gets its centered projection shifted by
`±(1 - s)`, which moves it to that edge of the display. The scissor moves with it.
HUD pieces are small and well inside one band, so an icon and its digits stay together.

An earlier per-element approach (anchoring J2D pane subtrees) split menu layouts apart
and missed the meter, which is not inside a screen. It was dropped.

### Screenshots

Captured with PrintWindow, so other windows on screen cannot leak into them. Files are
under `build/` and are not committed:

- Title: `title43_14.png` (960x720) vs `title169_14.png` (1280x720, backdrop pillarboxed,
  logo centered) vs `title219zoom_14.png` (1680x720, `--ws-backgrounds zoom`).
- File select: `final43_16.png`, `final169_16.png`, `final219_16.png`.
- Gameplay (`--input "600:START*10,1000:A*6"`):
  - `pw43_sheet.png` vs `pw169_sheet.png`: 4:3 and 16:9 at the same times. Covers the
    intro, the mansion exterior, and the foyer with the HUD.
  - `post_sheet.png` and `game219_*.png`: 21:9.

### Known limitations and next steps

- **HUD anchoring.** This is per draw, so a single HUD text that straddles the 30% band
  boundary would split. Nothing in the HUD does that so far. The Game Boy Horror and
  dialogue screens have not been checked yet.
- **Partial EFB copies computed from 3D positions.** No such copy has been seen so far.
  If one turns up (for example a copy of a rectangle around an object), its source rect
  is in stretched logical space, while the object was drawn Hor+. Fix it by moving the
  copy rect through the `s` transform in `GXSetTexCopySrc` whenever the last full-width
  perspective draw was *scaled*.
- **Embedded 3D in UI.** 3D drawn into a partial viewport (picture-in-picture) is
  stretched horizontally. The fix is to pillar-transform such viewports when the
  surrounding 2D is centered, but render-to-texture viewports must stay put, because
  their copy rect has to match. The two cases could be told apart by whether a
  `GXCopyTex` of that rect follows before the next `GXCopyDisp`/present.
- **Classification relies on the mirrored vertex state.** If a display list changes the
  vertex descriptor or VAT before an immediate 2D draw, `coversScreen()` gives up when
  the computed stride does not match, and the draw is treated as HUD. That fallback is
  safe.
- **Other culling.** Room and portal visibility and per-actor distance culling have not
  been audited. Nothing popped in the foyer or the exterior at 21:9.

## 3. High frame rate: frame interpolation

### As built (`src/recomp/interp.*`, `src/recomp/frame.cpp`, Aurora patch 0005)

The setting is F1 > Visuals > Frame rate:

- **Original (30 fps)** is the lockstep path, unchanged.
- **Interpolated 60 fps** and **Interpolated, unlimited** use the design below, simplified
  where the measurements allowed it.

**Capture instead of snapshotting.**
- Aurora patch `0005-capture-and-replay` makes the FIFO's display-list redirection
  per-thread, and adds `AuroraCaptureBegin/End/Pos` and `AuroraReplay`.
- While interpolating, the guest thread's GX output goes into a capture buffer, which
  becomes that frame's tape. A tape is self-contained: Aurora already serializes texture
  objects by value, and `GXCallDisplayList` inlines display lists. GXDrawDone and
  AuroraGXSync don't wait on the capturing thread.
- The host submits a tape with `AuroraReplay` inside its own frame and never touches the
  guest's `__gx` state.
- A game-recorded display list (`GXBeginDisplayList`) suspends the capture around itself.

**Decoupled guest.**
- `frame::hostFrame()` replaces `runUntilPresent()` in the host loop.
- Once interpolation is on, after the first 60 presents, the guest stops waiting for the
  host in `present()`. Each present closes a tape (`interp::endTape`, stamped with the VI
  retrace count) and opens the next one.
- The guest then runs exactly as on the console, paced only by the VI retrace timer, so
  game logic, audio and VI timing do not depend on the presentation rate.
- Switching back to Original hands control back at the next guest present: the host
  replays that last tape as is, then resumes lockstep.
- While the window is paused, `frame::setHostPaused()` stops the VI retraces, so a
  decoupled guest holds as it would in lockstep.

**What gets blended.**
- Measured before building: every matrix load in a gameplay frame comes through
  `GXLoadPosMtxImm`, about 150 to 450 loads per frame. There were no raw XF writes and
  no indexed loads.
- The GX HLE notes the tape offset of each 3D matrix load (pos, normal, texture) and of
  each perspective projection. The key is `(source address, slot, occurrence)`, which
  matched about 100% across frames in the foyer and the intro.
- The host replays the newest tape. Each noted block is blended toward the previous
  tape's value at the same key: `alpha = (now - tape done) / (game frame interval)`,
  which puts the display one game frame behind real time.
- 2D (anything under an orthographic projection) is not blended. Its matrices come from
  per-glyph scratch matrices that don't line up between frames, and blending them made
  file-select text and HUD pieces fly.
- **Cuts and jumps.** A matrix is shown unblended when it moves more than 400 units or
  0.35 in rotation in one game frame. Texture matrices use a relative bound, because
  scrolling coordinates wrap. Camera cuts therefore snap rather than smear.

**Measured** (1280x720, 120 Hz display, vsync on, `LM_INTERP_TRACE=1` logs alpha and
frame time per present):

| Mode | Presented | Game | Pacing |
|------|-----------|------|--------|
| Interpolated 60 | 59.9 to 60.0 fps | 29.6 to 30.0 fps | present interval 16.7 ± 1 ms, alpha alternating about 0.4/0.9, so displayed poses are evenly spaced |
| Unlimited | 120 fps (the display rate under vsync) | 30 fps | |

Typical figures per present:
- Tape size: 0.7 to 1.1 MiB per game frame.
- Matrices: about 1,000 to 2,000 blended per present; about 1 pos/nrm snapped.
- Texture matrices: about 350 are always snapped. These are projection-style matrices
  that the game alternates with zero matrices between frames, so blending them would be
  wrong.

Runtime toggling between Original and Interpolated every 3 s for 60 s ran cleanly.
Render resolution 2x (`resolution_scale=2`) together with widescreen, HUD anchoring and
unlimited mode also worked: the EFB copies are replayed with the rest of the tape, so
copy-based effects (the foyer window, the flashlight, fades) follow the interpolated
frame.

**Limitations.**
- **Replay starting state.** Replaying a tape twice starts from the GX state left at
  the end of that tape, not from its own start. That only matters if a frame relies on
  state from the previous frame without setting it again. Nothing has shown up.
- **Not blended.** Lights loaded with `GXLoadLightObjImm` (view-space positions),
  immediate-mode vertex positions (particles, the flashlight cone mesh if it is built on
  the CPU) and texture-projection matrices step at 30 Hz.
- **Guest memory referenced at replay time.** Vertex arrays and textures are read from
  guest memory when the tape is replayed, up to one game frame after capture. This is
  the same lag the console's GPU has, and games double-buffer for it. Data that the CPU
  rewrites within a frame could still show up in the next replay.
- **Pacing.** Pacing is driven by the host clock. The guest's completion jitters by a
  few ms, so alpha is computed from the retrace stamps and the wall time of completion.

### Original design notes

### Constraints in this port

- The game simulates and renders at 30 Hz. It paces itself on the emulated 59.94 Hz VI
  retrace (`hle_vi.cpp`), and `VISetNextFrameBuffer` marks a finished frame
  (`frame::present`).
- **Lockstep today.** The guest only runs inside `frame::runUntilPresent()`, between
  `aurora_begin_frame()` and `aurora_end_frame()`, and calls Aurora's GX directly from
  the HLE. While the guest sleeps on retraces inside a frame, the host is blocked, so
  presentation is capped at the game's rate.
- **No source changes.** Unlike Dusklight (`src/dusk/interp`), we cannot split game
  logic and drawing or add record calls in game code. Everything has to come from
  observing the GX stream and from hooks on recompiled functions.

### Architecture: record the frame as a GX "tape", replay it with interpolated matrices

**Stage 1: decouple guest and host (no visual change).**

1. The HLE stops calling Aurora directly and appends to a per-frame tape instead. Every
   SDK GX call already goes through one choke point, the `GXHLE` macro in `hle_gx.cpp`;
   each records an op with its decoded arguments. Raw FIFO writes (`hw::fifoWrite`) are
   recorded as byte runs.
2. `frame::present` closes tape N and hands it to the host through a two-slot queue.
   The guest continues immediately and keeps being paced by the VI timer, not by
   `aurora_begin_frame`.
3. The host loop (`src/recomp/main.cpp`) runs at display rate. Each frame it executes
   the newest tape into Aurora: the existing widescreen logic, then Aurora's GX.
4. **Guest memory lifetime.** A tape refers to guest memory by pointer: display lists,
   vertex and matrix arrays, textures, CI palettes. Once the guest runs ahead, it may
   overwrite that memory while the host still replays the tape. Snapshot only what can
   change:
   - **Immediate vertex data**: already inline in the tape.
   - **Vertex arrays (`GXSetArray`)**: copy the range the frame's draws actually use.
     Aurora patch 0002 already computes that range per draw from the indices. Expose it
     and snapshot at record time. Typical frames reference well under 5 MiB, Aurora's
     own budget.
   - **Matrix arrays**: `GX_POS_MTX_ARRAY`, `GX_NRM_MTX_ARRAY`, `GX_TEX_MTX_ARRAY`.
     J3D loads matrices indexed from these (`J3DShapeMtx::loadMtxIndx_*`, 0x801B79FC to
     0x801B7B5B, CP commands 0x20/0x28/0x30 in the raw stream). Snapshot each referenced
     element.
   - **Display lists**: static model DLs never change, so keep them by pointer. DLs
     built at run time (`GXBeginDisplayList`/`GXEndDisplayList`) are copied.
   - **Textures**: textures loaded from disc are static. CPU-written textures are rare.
     Hash them at `GXInitTexObj` to detect them. EFB copies already live on the host
     inside Aurora.
5. **Fallback.** If a frame's snapshot would be too large, or the tape is incomplete,
   run it the lockstep way.

**Stage 2: interpolate.** Keep tapes N-1 and N. Presenting at time `t` between their
timestamps (`alpha` from the retrace counter, `period = 2 retraces`), replay tape N with
these substitutions:

- **Matrices.**
  - Pos and normal matrices from `GXLoadPosMtxImm`, `GXLoadNrmMtxImm` and
    `GXLoadTexMtxImm`: key = `(guest source address, slot, occurrence #)`.
  - Elements of the matrix arrays: key = `(array role, element address)`.
  - J3D double-buffers its draw matrices (`J3DMtxBuffer`), so element addresses
    alternate between frames. Normalize to `(base of the buffer pair, offset)`. Or key
    J3D draws by `(DL pointer, matrix-index slot)`, because a shape's DL and its
    `loadMtxIndx` slot are stable.
  - Interpolate 3x4 model-view matrices as translation lerp, rotation quaternion slerp
    and scale lerp. A component-wise lerp is acceptable for small deltas.
  - A key that is missing in N-1 means a new object: use N unchanged.
- **Projection.** Lerp the elements, then apply the widescreen transform.
- **Camera cuts.** Hook `fn_80008654` (the game camera apply, camera struct at
  `lbl_803A3820`: eye `+0x0`, target `+0xC`, fovY `+0x90`, view matrix `+0x18C`) with
  `callOriginal`. Record eye, target and fov each sim frame. If they jump past a
  threshold, or the scene changes (the scene machine's change request), present N
  without interpolation for that frame.
- **Immediate-mode geometry** (particles, the flashlight cone, ghost trails): match
  draws by ordinal within runs of identical state (same texture, TEV and vertex
  format). If the vertex counts match, lerp positions; otherwise draw N's vertices.
  Dusklight's `interp/vertex.cpp` and `particle.cpp` do the same at source level.
- **2D (orthographic) draws.** Not interpolated: N is replayed as is. HUD motion is
  rare and steps at 30 Hz.
- **EFB copies.** Replay the copies too (`GXCopyTex` ops). The copy then captures the
  interpolated frame, and the blur, ghost and flashlight effects stay consistent.
  Effects that accumulate over frames (feedback blurs) will run at the display rate.
  Detect copies whose destination is read before it is written again in the same tape,
  and skip those on replayed frames.
- **Latency.** One sim frame (33 ms) of added latency, the same as Dusklight. Option:
  extrapolate the camera only.

**Stage 3: refinements.**

- Tie `begin_frame` pacing to the display (VRR friendly).
- Expose `--fps <n|unlimited>` and `--interp off|camera|full`.
- Optional source-level hook on `J3DModel::viewCalc` (0x801C79F0): rerun view
  calculation with an interpolated camera view matrix. This is cheaper than per-matrix
  matching when only the camera moves.

### Hook points summary

| Purpose | Where |
|---------|-------|
| Frame boundary / tape close | `VISetNextFrameBuffer` HLE -> `frame::present` (`hle_vi.cpp`, `frame.cpp`) |
| Record SDK GX calls | `GXHLE` macro (`hle_gx.cpp`) |
| Record raw FIFO (J3D inlines) | `hw::fifoWrite` (`hle_gx.cpp`) |
| Matrix identities | `GXLoadPosMtxImm`/`GXLoadNrmMtxImm`/`GXLoadTexMtxImm` HLE (source address), `GXSetArray` for matrix arrays, `J3DShapeMtx::loadMtxIndx_*` (0x801B79FC...) |
| Projection | `GXSetProjection` HLE (already deferred for widescreen) |
| Camera state / cut detection | hook `fn_80008654` (`lbl_803A3820`); optionally `J3DModel::viewCalc` |
| Sim pacing | VI timer thread and `VIWaitForRetrace` (`hle_vi.cpp`) |
| Present loop | host loop in `src/recomp/main.cpp` |

### Why not run the draw code twice?

Running the game's draw phase again for in-between frames would need the calc and draw
functions split at a stable point per scene. That needs per-scene knowledge of LM's
scene machine. Draw code also has side effects: particle emitters advance during draw,
J2D animations step, and `GXSetDrawDone` / `VIWaitForRetrace` pacing is mixed in.

The tape design only depends on the GX interface, which the HLE layer already owns. It
also keeps the guest's simulation bit-identical to the console.
