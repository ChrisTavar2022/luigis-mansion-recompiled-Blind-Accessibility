# Unused content in the retail code (GLME01)

Two pre-release features are still in the US `main.dol`. Both are switched on from
**F1 > Enhancements** and are implemented as hooks that call the original function and
only change which of the game's own code paths it takes (`src/recomp/unused_content.cpp`,
`tools/recomp/hooks.txt`). No game code was rewritten. Addresses are GLME01 rev 0, and the
names are the dtk/symbols.txt ones.

## Game Boy Horror live camera ("Game Boy Horror camera")

Pre-release builds (Nintendo Power 2001 screenshots, the E3 2001 demo) showed a live
first-person view on the Game Boy Horror screen in the bottom-right HUD, so the player saw
the room from two angles at once. The retail game still has the whole pipeline behind a
screen-mode variable that it never sets to the camera value.

### How it works in the retail code

GBH HUD state lives in `lbl_803CE2D8` (sprites at +0x48.. frame `cgbh.bti`, +0x60 shadow,
+0x78 `back.bti`, +0x90/+0xA8/+0xC0 LED pieces; +0x48/+0x4C hold the frame's x/y).
The screen-content mode is `lbl_804D846C`:

| mode | set by | effect |
|---|---|---|
| 0 | never | renderer viewport hook only (`fn_80010EBC`), no extra 2D |
| 1 | never | `back.bti` + an orange 16-segment gauge with two numbers (`fn_80044C4C` on `lbl_804D8560`); probably the early money readout the GBH showed before the camera |
| **2** | **never** | **live camera**: see below |
| 3 | `fn_8003EA78` (E. Gadd's call starts) | call portrait at the screen rect (`fn_80115474`, 110x84) |
| 4 | init `fn_8003CEB4`, `fn_8003EA84` (call ends) | the retail screen (`fn_800571B8`) |

Mode 2, in `fn_8003CFC0` (HUD update, every frame):
- the GBH is raised to its normal on-screen height;
- `fn_80010EBC(x+0x18, y+0x16)` stores the screen rectangle (110x82 at 640x480) in
  `lbl_804D0008/804D000A/804D8162/804D8164` and sets `lbl_804CFFF0 = 2`. It returns 0
  (screen off-screen, or map 0/1), and then `lbl_804D8440` turns on the static-noise overlay
  (`gbnoise01..03.bti`, drawn by `fn_8003E3D0`/`fn_8003E7B0`).

Renderer side (`fn_80010F20`, called each frame from `fn_800112F8`):
- `lbl_804CFFF0 == 2` sets the view count to 2 (`fn_8000AE04`). View 1
  (`lbl_803A4C80[1]`) gets the GBH screen rectangle as its viewport, and the flag is reset
  to 1 every frame, so the HUD has to request it again each frame.
- The frame loop `fn_8000BCE8` draws each view (`fn_800115F8(i)` sets the viewport; for
  view 1 it also calls `fn_8000B228`, the sub-viewport scissor/clear). View 1 draws the map
  and actors (`fn_8000BB98(1)`: `fn_80011824` + `fn_80060E70(2)`), then `fn_8003E7B0(1)` puts
  the noise on top when needed.
- View 1's camera was set up for single player in `fn_800204B4`: update callback
  `fn_80021DDC` → `fn_80021B04`, a first-person camera. The target is Luigi's position
  + 180 up, the eye is 100 units along his facing vector (flipped when he is upside-down,
  `unk28 == 1`), the FOV is 50 and near is 10. The same camera code is reused by the shipped
  first-person GBH look mode (`fn_80021C68` when `lbl_804D8160`).

So every piece is present: HUD mode, viewport handshake, second view, camera, noise.
Only the write of 2 to `lbl_804D846C` is missing.

### What the port does

`HLE(fn_8003CFC0)`: when the option is on, D-pad Up (edge on port 1, read from the PADRead
HLE) toggles the camera. While it is on, mode 4 becomes 2 before the original runs, and 2
goes back to 4 when it is switched off. Mode 3 (E. Gadd's call) is never overridden, so a
call still shows the portrait and the camera returns when the call ends. All rendering is
the game's own code. Test aids: `LM_GBH_CAM=1` starts with the camera on, without the
setting. `LM_GBH_CAM=2` also forces the GBH HUD up (player +0x55) before Luigi has the
Game Boy Horror, so the opening foyer can be used.

Port fix it needed: the view-1 clear `fn_8000B228` resets depth with
`GXSetZTexture(GX_ZT_REPLACE)` from an all-0xFFFFFF Z24X8 texture. Aurora leaves
`GXSetZTexture` as a TODO, so the clear quad (z = -900, literal `lbl_804D99AC`) wrote its own
depth and the whole screen failed the depth test (black). `HLE(fn_8000B228)` moves that
literal onto the far plane (-999.99) for the call. The visual result is the same.

Verified: in the foyer (`LM_GBH_CAM=2`, foyer at ~2 min under load), the GBH screen shows
Luigi's view of the staircase and dresser, and D-pad Up from the scripted input toggles it
(`[unused] Game Boy Horror camera on` in the log).

Limitations: ghosts are drawn in view 1 only by the direct pass
(`fn_80060E70(2)`; the ghost glow layer is view-0 only), so on the GBH screen they appear
with the early direct shading described below. The camera image is not mirrored in the
PAL mirror mode.

## Early ghost shading ("Early (opaque) ghost shading")

Retail ghosts get the glowy see-through look from a separate **ghost layer**:

1. `fn_8000BB98` draws the map and normal actors (`fn_80060E70(0x10)`: pass 0x10 skips actor
   types with type flag 8, which is every ghost; flags are in the type table `lbl_8033B590`,
   0x1C bytes per type, read by `fn_8006A3BC`).
2. `fn_8000C2D0`: `fn_8005F518` copies the scene to `lbl_804D86BC` and clears the EFB.
   `fn_80060E70(0x30/0x130)` draws only the ghosts, writing each ghost's alpha
   (MoveObj +0xE3) as destination alpha. `fn_8005F498` copies that layer at half resolution
   (320x240 RGBA8) to `lbl_804D86B8`.
3. `fn_8005F5B0` puts the scene back, and `fn_8005F280` adds the half-res ghost layer
   **additively**, weighted by its alpha (blend SRCALPHA, ONE).

The result is a soft, blurred, light-emitting ghost that never fully covers what is behind
it.

The older path is still in `fn_80060E70`. With pass bits `r3 & 0x30 == 0`, every actor,
ghosts included, is drawn directly into the scene: ghost lighting (`fn_800581E8`: ambient
from the ghost's colour +0xE9, its own glow light), full-resolution S3D material
(`fn_80058C30`, alpha = material alpha × ghost alpha), normal alpha blending. Retail only
uses this pass for the never-shipped GBH camera view (`fn_80060E70(2)`) and the event
renderer (`fn_8000F1F8`). This is the solid, more opaque ghost look of the E3 2001
material.

### What the port does

`HLE(fn_80060E70)`: when the option is on, the 0x10 pass runs as pass 0 (normal actors plus
ghosts, drawn directly). The 0x30/0x130 ghost-layer passes run with pass bits 0x20, which no
case handles, so the layer stays empty and the additive composite adds nothing. Everything
else (per-ghost alpha fades, vacuum flashes, colours) is the game's own data and code. Test
aid: `LM_EARLY_GHOSTS=1`.

Verified with `LM_START_MAP=11` (Boolossus balcony). Retail: a glowing, see-through Boo with
Luigi visible through it. Early shading: a solid grey Boo that hides what is behind it.

Not reconstructed: the shipped GBH first-person "ghost scan" pulse (dst-alpha pulsing in the
0x30 pass, `fn_800E9E48`) has no effect while the option is on.

## Other leftovers noticed

- GBH screen mode 1 (gauge + two numbers, `fn_80044C4C`) is never selected. It could be
  exposed the same way.
- `fn_80011150` / `fn_800204B4`'s second branch: a complete two-player split-screen
  (`sAppearPointSlotIndex__Q24Koga9EnManager == 1`; view 0 top half, view 1 bottom half, each
  following its own player record in `lbl_803A3CA0`).
- Ghost archives (`model/obakeNN.szp`) carry `bti/obamN.bti` (64x64 CMPR ghost portrait) and
  `obamN_kg.bti` (I4 silhouette) plus a `geb/` file. No code references them by name.
