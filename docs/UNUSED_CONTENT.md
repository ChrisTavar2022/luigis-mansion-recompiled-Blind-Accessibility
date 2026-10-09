# Unused content in the retail code (GLME01)

Everything here also works on the European game (GLMP01): addresses go through `VA()` and
the hooks are tagged `us pal` (docs/SETUP.md, *Version-aware code*). European differences:
the GBH sub-view clear is `fn_8000B7CC` there (no hook needed); the end-of-frame hook is the European
`LMChangeFrameBuffer` (`fn_80007940`).

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
| 1 | never | The cut E3 clock + radar border (`fn_80044C4C`: 16-segment minutes/seconds, border segments animated by the ghost tracker). The mode switch at 0x8003D53C sends modes 0/1 to the same viewport code as the camera, so the 3D view covers it unless instruction patch 3 (0x8003D544) skips that; exposed as "Game Boy Horror E3 clock" (D-pad Left) |
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

Depth reset: the view-1 clear `fn_8000B228` resets depth with
`GXSetZTexture(GX_ZT_REPLACE)` from an all-0xFFFFFF Z24X8 texture. Aurora now implements Z
textures (patch 0008, docs/RENDERING.md), so the game's own clear works; the earlier
workaround hook (moving the clear quad onto the far plane) is gone.

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

## Unused assets (`src/recomp/unused_assets.cpp`)

All of these are under F1 > Enhancements > Unused content and are off by default. The hooks
are US-only (`tools/recomp/hooks.txt`) and gated with `port::usCodeFeature()` in the menu.
Data patches only overwrite a value that matches the expected retail original (or the patched
value when you switch an option off). Anything else is left alone and logged once.

### Beta skybox ("Beta skybox")

- Retail: `fn_8002F924` picks the sky descriptor from `lbl_80300C7C[sCurMapNo]`. For map 2
  that is `lbl_80300B60`: archive `/Iwamoto/vrball_M.szp`, model `/vrball01.bmd`, one texture
  animation `/vrball01.btk`. `fn_800067D0` mounts the archive, and the callback
  `fn_8002F4DC` loads the resources through the archive's `getResource`.
- Unused: the always-mounted game archive (`/Game/game_usa.szp`, returned by
  `fn_80066FB4`) contains an older `iwamoto/vrbox/vrball01.bmd` (38752 bytes; the retail one
  is 41888) with its own `vrball01.btk`. No code references that path.
- Port: `HLE(fn_8002F4DC)` marks the load when the descriptor is `lbl_80300B60`. During that
  load, `HLE(getResource__10JKRArchiveFPCc)` sends `/vrball01.bmd` and `/vrball01.btk` to the
  game archive at `/iwamoto/vrbox/...`. The sky then loads, animates and draws through the
  game's own code. Applies to map 2: the opening, the grounds and every window/balcony. The
  boss skies (`vrball_B`, `vrKoopa`) are untouched. Takes effect on the next map load. Test
  aid: `LM_BETA_SKY=1`.
- Verified: the redirect fires (`[unused] beta skybox: ...` in the log). On screen the older
  sky is very close to retail (same cloudy green palette), so the visible difference in the
  opening shots is small.

Verified in game (`LM_TELEPORT`, empty card): the Nursery shows the heart door and the Twins'
room the star door; retail shows the plain brown door in both. The GBH map (Y, then A to
zoom) shows a small full-body Luigi instead of the big head.

### Heart door / Star door

- Map 2's doors are a static table in the DOL: `lbl_802FF218[2]` → +0x18 = `0x80302F80`,
  0x1C bytes per door. Byte +6 is the door type. It selects the model, via
  `lbl_802FF95C[type]` → `lbl_802FF868` = `/iwamoto/Door/door_NN.bin` in the game archive.
  It also selects the lock/unlock cutscenes: `fn_8011496C` plays scene
  `3 + (type-1)*4 + 2*a + b` in `lbl_80373AA4`, which maps to
  `oodemoNN/codemoNN/hodemoNN/nodemoNN.szp`.
- Types 07 and 08 have a model (`door_07.bin`, `door_08.bin`) and all four cutscenes
  (`/Ajioka/ADemo/{oo,co,ho,no}demo07/08.szp`), but no door in any map uses them.
- Door 46 (`0x80303488`, position -3005/700/-710) is the door from the 2F hallway into the
  Nursery (Chauncey's room). Door 45 (`0x8030346C`, -2190/700/-710) is the Twins' room door.
  This was checked against furniture positions: `bed24`/teddy/rocking horse at x ≈ -3700,
  `bed25`/toy car at x ≈ -1300, both south of the hallway wall at z = -710. Both doors ship
  as type 01.
- Port: `HLE(fn_800122BC)` (map setup, before the doors are built) sets the type byte to 07
  (heart, `0x8030348E`) and/or 08 (star, `0x80303472`). Because the type also picks the
  cutscenes, the game plays the unused locked/unlock scenes for that door by itself. Takes
  effect on the next map load. Test aids: `LM_HEART_DOOR=1`, `LM_STAR_DOOR=1`, and
  `LM_TELEPORT=x,y,z,frames`, which moves Luigi once, for example
  `-3005,560,-400,60` = inside the Nursery facing the door.

### Game Boy Horror map: full Luigi model

- Retail: the GBH map (`fn_8002967C` for map 2, `fn_8002B310` for the training maps) loads
  nine files from the guide-map archive (`guidemap.szp` in the DOL's static data, in ARAM)
  through `fn_8001F8D8`. File 1, `/gide_lui.bin` (43968 bytes), is the big Luigi head drawn at
  Luigi's position (model `lbl_804D830C + 0x318`, placed by `fn_8002B140`).
- Unused: the disc still has an older guide-map archive, `/Iwamoto/map2/gidemap.szp`
  (gidemap/gide_d1/gide_d2/gide_lui/g_d1x/g_d2x/g_key1). Its `gide_lui.bin` (24288 bytes) is
  a full-body Luigi figure. Nothing loads that archive.
- Port: on first use, the host reads `/Iwamoto/map2/gidemap.szp` from the disc (Yay0/Yaz0 +
  RARC, original code), copies `gide_lui.bin` into guest memory (`hleAlloc`) and returns it
  from `getResource("/gide_lui.bin")`. `HLE(getResSize__10JKRArchiveCFPCv)` reports its size.
  The game parses and draws it like the retail marker. Test aid: `LM_BETA_MAP_LUIGI=1`.

### Portrificationizer: each ghost's own model

- Table `lbl_8036E52C`, 0x18 bytes per portrait ghost, read by `fn_80121E14`/`fn_80124890`.
  Field +4 of the 21 entries at `0x8036E548 + 0x18*i` is `0x60` (the generic blue blob)
  everywhere. The values the known Gecko code writes (`0x64`, then `0x121`, `0x125`, …,
  `0x16D`) are each ghost's own unused portrait-scene model id.
- Port: both readers are hooked and write the table first (only over `0x60` or the patched
  value), so it is correct whenever the scene reads it and goes back to `0x60` when the option
  is off. Test aid: `LM_UNUSED_PORTRAITS=1`.

### Air and water nozzles ("Air and water nozzles")

`/model/wplight.szp`, `/model/wpwater.szp` and `/model/wpair.szp` are character types 7/8/9
(`lbl_8033B590`, right after `vbody` 5 / `vhead` 6). Each has a model-registry entry and its
own attach table for Luigi in `fn_80078008` (`lbl_80354290`, `lbl_803543B4`,
`lbl_803544D8`; Luigi's other character types 1/0xB3 have a parallel 0xB5..0xB9 set).

How the retail code uses them:
- Luigi's equipment is created in `fn_80072554` (types 6, 5, 0x0A `gameboy`) and attached by
  `fn_800AE1A4`: nozzle at Luigi +0x7A4, tank +0x7A8, GBH +0x7AC.
- The nozzle is one actor whose character type is changed on the fly: `fn_800B9064(luigi,
  type)` → `fn_800B90D0` → `fn_80067B04` (MoveObj re-init with the new type's model).
- Luigi's Poltergust mode selector `fn_800B36B8` sets +0x1180: 1 vacuum (R, input object
  +0x7D4 +0x20 > 0), L (+0x2C > 0) → the element +0x1184 (2 fire, 3 water, 4 ice; 1 = no
  element → 6 air), 5 flashlight. On a change it re-types the nozzle: **5 → type 7
  `wplight`** (so wplight *is* used in retail, as the idle/flashlight nozzle), every other mode
  → type 6 `vhead`. Types 8/9 are never selected.
- Vacuum, air and element spray all work in ordinary rooms with `LM_TEST_STATE` (checked in the
  Nursery and in the foyer after the opening). The earlier "L/R do nothing" runs pressed the
  buttons before gameplay started (still in the opening/dialogue).

Port (`src/recomp/nozzles.cpp`, hook on `fn_800B36B8`, US code only): the selector runs
unchanged; afterwards, with the option on, the nozzle is re-typed through the game's own
`fn_800B9064`: mode 3 (water) → 8 `wpwater`, mode 6 (air) → 9 `wpair`; when the mode leaves
them (or the option is turned off) it goes back to 6/7 as in retail. D-pad Right toggles an
"air nozzle" selection: while Luigi carries an element, the selector sees +0x1184 = 1 for the
duration of its call, so L blows air with `wpair` and the element is kept. With the option
off the hook only calls the original (types stay 6/7, D-pad Right does nothing).

Verified (`build/nozzles_cmp.png`, guest frames from `LM_FRAME_DUMP`, Nursery,
`LM_START_MAP=2 LM_START_APPEAR=1`): retail air (big `vhead`), restored air (`wpair`),
flashlight (`wplight`), retail water (`vhead`), restored water (`wpwater`); the water stream
and the air effect come out of the new nozzles. `build/nozzles_full.png` adds the D-pad Right
case (air while carrying water). Test aids: `LM_NOZZLES=1` (force on), `LM_NOZZLE_DUMP=1`
(logs mode/element/meter/nozzle type on change, plus the Poltergust controller
`[Luigi+0x7E4]` +0x310/+0x314/+0x334 and its 0x50-byte input block).

What the game itself never finished:
- No code path selects types 8/9; the mapping water → `wpwater`, air → `wpair` is inferred from
  the type names and their attach tables. There is no `wpfire`/`wpice`, so fire and ice keep
  `vhead`.
- The attach tables for 8/9 are only read by `fn_80078008`; the re-type keeps the slot the
  `vhead` was attached with (like `wplight` in retail), so the nozzle sits where `vhead` does.
- Air and water behave exactly as with `vhead` (same effects, same element meter); nothing
  in the code gives the dedicated nozzles different gameplay.
- Luigi's other character types (1/0xB3) have 0xB8/0xB9 entries too; the port leaves them alone.

## Other leftovers noticed

- GBH screen mode 1 is the E3 clock (exposed as a toggle); there is no separate gauge screen.
- `fn_80011150` / `fn_800204B4`'s second branch: a complete two-player split-screen
  (`sAppearPointSlotIndex__Q24Koga9EnManager == 1`; view 0 top half, view 1 bottom half, each
  following its own player record in `lbl_803A3CA0`).
- Ghost archives (`model/obakeNN.szp`) carry `bti/obamN.bti` (64x64 CMPR ghost portrait) and
  `obamN_kg.bti` (I4 silhouette) plus a `geb/` file. No code references them by name.

## Debug features

Retail LM was built from an older JSystem that has no JUTProcBar / JUTConsole, and no game
code calls `OSReport`. What is left are a few switches in `.sbss` that code reads but nothing
ever writes. They were found by listing every small-data/bss variable that game code
(0x80003000-0x80190000) loads but never stores to or takes the address of (`pal_diff.py`'s
`walk()` over every function), then reading what each one gates. Most hits are guarded static
locals; these two are real debug switches. Both are in **F1 > Enhancements**, off by default
(`src/recomp/dev_tools.cpp`, applied each frame in the `LMChangeFrameBuffer__Fv` hook):

| Switch | Gates | Port |
|--------|-------|------|
| `lbl_804D8E04` (byte) | `fn_80160138` (particle manager per-frame update) prints `"Emits %d Ptcls %d Chldn %d"` and five `"%02d %02d ..."` history rows with `fn_801D71B0` (JUTReport: vsnprintf, then `fn_801D7114` inserts into the JUTDbPrint queue at `lbl_804D9550`) | Debug tab "Particle manager report". Retail never creates the print object (`lbl_804D9550` is never written), so `fn_801D7114` is hooked: each line is collected and drawn by an ImGui overlay at the game's 640x480 position. |
| `lbl_804D86B0` (word) | `fn_8005E9EC` (called from the game-scene draw `fn_8000BCE8`): copies the EFB at full size and at 160x120, then blends the small (blurred) copy back by depth with a TEV setup; its buffers `lbl_804D86B8`/`BC` are allocated by `fn_8005F098` on every scene setup | Unused content tab "Unused depth blur pass". Works as is: the far parts of the picture go soft. |

Also checked and not usable: `print__14JUTDirectPrint` is only the crash screen
(JUTException); `test.blog` and `/scrn/testtitl.blo` are unreferenced layout names;
`ATestData.cpp` (Ajioka) is a water-effect test loader whose strings are OSPanic messages.

### Debug modes known from Action Replay codes

Community AR codes (TCRF / GC-Forever) reach more leftover debug code. The recompiled game has
no instruction memory to patch, so each code is reproduced by a hook with the same effect
(`src/recomp/dev_tools.cpp`, Debug tab "Debug modes", all off by default; the original codes
activated them with button combinations on the pad word `0x804CAFD0`, here they are toggles).

| Feature | AR code (US) | What it changes in the game | Port |
|---------|--------------|-----------------------------|------|
| Debug mode | `403A2B84 001325CB` (writes 0xCB to byte +0x1325 of the actor at `[0x803A2B84]`, Luigi) | Luigi's debug-flag halfword +0x1324 (initialised to 0x14 by `fn_800AD5AC`). Bits: 0x08 debug text, 0x40 scan boxes (both read by Luigi's draw callback `fn_800B172C`), 0x10 (`fn_800AE770`), 0x20 (`fn_800B1BD4`, toggled by `fn_800B2F98`), 0x04 (`fn_800B80C4`); 0xCB clears 0x10/0x04, which changes how Luigi is drawn | Low byte forced to 0xCB in Luigi's update hook (`fn_800B783C`); the game's value is put back when turned off |
| Debug info display | NOP the `beq` at `0x800B17E4` | `fn_800B172C` pass 6: Japanese text at the top left, ホコリ (dust) mg, れいき (aura) cc, みず (water) ml, ハート (hearts), via `fn_8003EF40` | Bit 0x08 presented to the original for the duration of the pass-6 call |
| Game Boy Horror scan boxes | NOP the `beq` at `0x800B17CC` | `fn_800B172C` pass 3: `fn_8007BDA4` draws orange boxes around every scannable object | Bit 0x40 presented during the pass-3 call |
| XYZ vectors | NOP the `ble` at `0x800B1794` | `fn_800B172C` pass 2 draws axes (`fn_80094170`, size 150) on joint `+0x1328` only when it is > 0 | After the original, the skipped draw is done: `PSMTXConcat(actor +8, joint 0)` then `fn_80094170` |
| HUD test | byte `0x804D854A` = 1, plus `0x80043910` = `addi r16, r16, 0x0A90 / 0xFD00 / 0x0FA0` | `fn_800433B4` draws four coloured message panels and a row of 19 sprites; the patched instruction picks the sprite set: HUD icons (shipped value), digits (-0x300), element meter (0xFA0) | Flag written each frame; the sprite pointer of the calls from that loop (`fn_8003BEEC`, return address `0x80043930`) is moved by the same offset. Combo Off / Icons / Numbers / Element meter |
| Font test | PAL only: `0A4B8590 0201 / 024C5B58 0001`, `02309B26 9B28` | Flag `lbl_804D8488` (PAL `0x804C5B58`, mapped with `pal_diff.py`): `fn_8003EBB0` prints 12 lines of kanji, kana and symbols. The second PAL write repoints table entry 11 (US `0x80304C18`, katakana) at "alphabet 123" | Flag written each frame; "Latin last line" repoints entry 11 (restored when off). US prints under the HUD's projection, so nothing shows; PAL added `LMDefaultOrthoView` at the start of the function, and the hook does the same while the test is on |

Verified in the foyer (`LM_START_MAP=13; LM_DEV_WARPS=2:6@3000`): debug text and scan boxes
(`build/ar_info_scan_axes.png`, axes are the short red/green lines at Luigi's feet), debug mode
(`ar_debugmode.png`), HUD test modes (`ar_hud1..3.png`), font test (`ar_font.png`). Test aids:
`LM_DEV_LUIGI_FLAGS`, `LM_DEV_INFO_DISPLAY`, `LM_DEV_SCAN_BOXES`, `LM_DEV_AXES`,
`LM_DEV_HUD_TEST=0..3`, `LM_DEV_FONT_TEST=0|1|2` (2 = with the Latin line).

Port-side debug aids in the Debug tab (not game code): an **info overlay** (F3: map, room,
Luigi's position, taken from Luigi's update `fn_800B783C`), the **Visit area** warp list and
the **model viewer** (F4). F3/F4 are keyboard-only and not mapped to the pad, so they cannot
interfere with play; the viewer neutralises the pad while it is open, like F1.

Test aids: `LM_DEV_PARTICLE_REPORT=0|1`, `LM_DEV_DEPTH_BLUR=0|1`, `LM_DEV_INFO=1`,
`LM_MENU_TAB=<Enhancements sub-tab>` (starts with the menu open on that tab).

## Unused areas

Map table: `lbl_802FF218` (14 pointers, indexed by `sCurMapNo` `0x804D80C8`) to per-map
descriptors (room count, room files `/Iwamoto/mapN/...`, room bounds, adjacency). Maps are
changed only by the event command `<WARP>(n)` (in `fn_8006385C`: next map `lbl_804D80A4` = n
and `sCurSceneId` = 0, so the scene manager `fn_8000B4A0` tears the game scene down,
`fn_8000BF38` sets `sCurMapNo` = next map and `fn_8000C188` sets the new map up), by new game
(map 2) and by game over (`fn_8000F140`: map 1 or 12). `<SETLUIGI>(p)` picks the Luigi appear
point (`sGlobalLuigiAppearPoint`, consumed by `fn_800E52BC`). WARP targets in the event
scripts: 1, 2, 3, 4, 6, 9, 10, 11, 12, 13. Never loaded: **0, 5, 7, 8**.

| Map | Rooms (`/Iwamoto`) | Content | State |
|-----|-------------------|---------|-------|
| 0 | `map0/test_00..03.bin` | Test map. JMP: Luigi, `dancer`, `situji` (butler), `candle`, two `dltelesa` Boos, `marioe`, `mphand`; observers `kinopio` (Toad) and `rion1`; events 74 and 95; rat, skull, "waiter" and `test1-4` paths | Loads; a mansion-like room with test actors |
| 5 | `map5/h_03_00..03.bin` | "Statue hall": winged-unicorn statues, pillars, two doors. Only Luigi in its tables; `telesa`, `dance`, `mg_test` paths | Loads, playable |
| 7 | `map7/h_05_00..03.bin` | Map 5's statue hall with both corridors (rooms 1 and 2) 2000 units longer and the end hall (room 3) moved back to match; own `col.mp`. Only Luigi | Loads with the rebuilt archive (below), walkable end to end |
| 8 | `map8/h_06_00..03.bin` | The same four-room plan in a wooden, lantern-lit style (two stone chests in the entrance hall, side alcoves, corridors 5500 units long, end hall). E. Gadd (`lohakase`) stands in the entrance hall with event 14 attached. JMP tables in an older layout | Loads with the rebuilt archive (below), walkable |

Used maps for reference: 1 E. Gadd's lab, 2 mansion, 3 training room, 4 Portrificationizer,
6 portrait gallery, 9 roof (King Boo / Bowser), 10 Chauncey's room, 11 balcony (Boolossus),
12 the Portrificationizer room with its own setup (after Bowser / game over; same `h_02.bin`
as map 4), 13 graveyard (Bogmire).

**Port:** the Unused content tab lists maps 0/5/7/8; the Debug tab "Visit area" lists all 14
plus the ten mansion appear points (map2 `jmp/characterinfo` `luige` entries: 0 foyer, 1/7 2F,
2 1F, 3 roof level, 4/5/8 3F, 6 foyer door, 9 Secret Altar). A warp is queued and applied in
the `fn_8000B4A0` hook exactly like `<WARP>`, and only while the game scene is idle
(`sCurSceneId` is the game scene and no scene change is pending, `lbl_804D8088 == 0`); during
transitions, events that hold the scene and the pause/map screens it waits. Maps with entry
events play them (map 4's E. Gadd explanation, map 9's Bowser intro, appear point 9's King
Boo scene). An event that is already running keeps running in the new map, so warping out of
the opening sequence continues the intro story there. Verified in Original30 and
Interpolated60: maps 0, 2 (appear points 6 and 9), 3, 4, 5, 9, 13.

Test aids: `LM_START_MAP=<n>` (new game starts in map n; moved here from the Hidden Mansion
hooks), `LM_START_APPEAR=<p>`, `LM_DEV_WARPS=<map>[:<appear>]@<frame>,...`.

### Maps 7 and 8: why they crashed and how they load now

Both crashed for the same reason, and it is in the data, not the port: the game would fail
the same way on a console.

- **Missing model.** Every map setup loads `k03-elem1_eyes.bmd` from the map archive (the
  lookup at `0x8017310C`, type `K03-`) and builds a J3D model from it without checking the
  result. Every retail map archive has `j3d_effect/k03-elem1_eyes/`; `map7.szp` and
  `map8.szp` have no `j3d_effect` directory at all. The model is built from a null resource.
  On the console that read of address 0 (not mapped by the OS's BATs) is a DSI exception.
  In the port low addresses alias main RAM, so the loader wrote through garbage pointers
  instead (J3D packets, the audio message queue, JAudio's track tree). The audio thread then
  recursed through the corrupted track tree (`SeqUpdate` -> `Jam_UpdateTrack` ->
  `SeqUpdate`, and `Jam_SeqmainNote`) until the host thread's stack ran out. That is the
  "unbounded recursion" seen before: a cycle in corrupted data, not deep-but-finite guest
  recursion, so a bigger host stack would not have helped. Found with `LM_TRACE=ring`, a
  resource-lookup log and `LM_WATCH` on a track's child pointers.
- **Map 8's tables.** Its JMP tables predate the retail field layout: characterinfo has 16
  fields instead of 23 (no `create_name`, `room_no`, `item_table`, `appear_point`, `msg_no`,
  `cond_type`, `move_type`, `appear_type`; `dir_x/y/z` are 16-bit ints), groupinfo 20
  instead of 9 (with link and transform fields since dropped), eventinfo 21 instead of 24,
  polygoninfo 8-byte entries with 7 fields instead of 4-byte entries with 3. furnitureinfo,
  observerinfo, soundgroupinfo, soundpolygoninfo and roominfo are missing.

**Fix** (`src/recomp/unused_maps.cpp`, `src/recomp/rarc.cpp`): when the game opens
`/Map/map7.szp` or `/Map/map8.szp`, the port serves a copy rebuilt from the user's disc
(`hle_dvd.cpp` overlay chain, virtual entries `0x4C000000+`). The archive is unpacked into a
directory tree and `j3d_effect/k03-elem1_eyes/k03-elem1_eyes.bmd` is added from
`/Map/map5.szp` (map 7's twin). For map 8, every table map 5 has is rebuilt in map 5's
layout. Fields are matched by name hash (that is how `ToolData` finds them), ints and floats
are converted where the layouts disagree, and fields with no source get the retail
"nothing" values (`create_name` `----`, other strings `(null)`, numbers 0). The sound
tables get one zeroed entry per collision polygon or group, and the other missing tables
stay empty. The result is repacked as a RARC and wrapped in a literal-only Yay0 stream.
Retail never loads these two files, so nothing else changes. With tracing on, the runtime
now reports host-stack exhaustion with the call ring instead of dying silently.

**What is there** (verified with `LM_START_MAP=7/8` and `LM_DEV_WARPS=7@3000,8@4500` from
the mansion, with screenshots, F3 overlay for positions):

- Map 7: Luigi starts in the statue hall. Both corridors lead about 4500 units north (map 5's
  stop at about 2500) and end at a door.
- Map 8: Luigi starts at (2547, 0, -138) facing a lantern-lit wooden hallway. E. Gadd
  (`demo_hakase`) stands at (2549, 0, -932) between the corridors. Luigi bumps into him, and
  the walls and the chests block him. The left corridor runs to a door at about z -6500.
  The wall between the entrance hall and the corridors (z -1050) is fan-triangulated from
  one corner in `col.mp`, so thin collision slivers cover the right part of each corridor
  entrance. Enter the left corridor at x of about 2200, and the right one near its left
  edge. Maps 5 and 7 have a milder version of the same thing.
- Event 14 on map 8 is a talk-style trigger (`EventIf` 1, `EventLock` 1, area 120) at
  E. Gadd's position. Its script is the retail `event14.txt`: Lydia's portrait line "Oh
  dear... Such a draft..." (map 2 uses it for her room), then `<FLAGOFF>(169)`. With the
  shipped area it cannot start, because E. Gadd's body keeps Luigi about 165 units away. In
  a test with the area raised to 300 (not shipped), pressing A next to him played the line
  over this hallway. So in this early build the event was a test hookup on E. Gadd, not a
  scene of his own.

## Unused rooms (work in progress, paused)

Leftover room models on the disc: `/Iwamoto/map1/h_01.bin` (E3 2001 lab, extra robotic-arm
stand), `/Iwamoto/map1/h_02.bin` (pre-E3 Portrificationizer room; retail is `map4/h_02.bin`),
`/Iwamoto/map2/room_67.arc` member `b1_c_67.bin` (older breaker room), `room_07.arc` /
`room_30.arc` member `room..bin` (duplicate 1F / foyer 2F hallways; room_07's differs from the
retail `room.bin` by 42 bytes), `/Iwamoto/map2/room01A.bin` (no retail counterpart).

`src/recomp/room_swap.cpp` (Unused content tab, "Unused rooms"): Go warps to the retail room's
map and, for that visit only, `hle_dvd.cpp` serves the retail file rebuilt with the leftover
model (RARC member replaced, or the whole `.bin`); collision, doors and objects stay retail.
The swap ends when Luigi leaves the room/map or another warp is made. Test aid
`LM_DEV_ROOM=<index>@<frame>`.

Status: the lab and Portrificationizer visits load the leftover models (logged and seen).
The mansion rooms (67, 7, 30) need Luigi to start inside them; the spawn override
(`findLuigiAppearIndex` + `ToolData::getValue` pos_x/y/z of Luigi's entry while a visit is
pending) is implemented but not verified, and Luigi still started in the foyer in the last
test. Not started: a dedicated room slot for room01A (would need a map-2 room table entry,
JMP roominfo row and collision), and the breaker-room door/collision mismatch check.

## Model viewer

Debug tab "Model viewer" (F4). Lists the archives under `/model`, `/Game`, `/Kawano` and
`/Ajioka` (Aurora's disc reader; Yay0/Yaz0 and RARC are parsed host-side and nested `.arc`
files expanded, so `game_usa.szp`'s `model/luige.arc` etc. appear), and every `.mdl` model and
`.key` animation inside. The selected model is drawn by the game's own model code:

- the bytes are copied into a private `JKRExpHeap` built with
  `__ct__10JKRExpHeapFPvUlP7JKRHeapb` at guest 0x81812000-0x81FF0000 (host-backed memory above
  the 24 MiB the game uses; the system/current heap globals are restored afterwards);
- `fn_80061B40` relocates the `.mdl`, `fn_80061A24` the `.key`, `__ct__8S3DModelFll` builds the
  S3DModel (its matrix arrays come from the viewer heap), `fn_8005AA3C` sets the animation;
- drawing follows the US file-select screen's Luigi (`fn_800494E4`): lighting mode 0 of
  `fn_80057F90`, `fn_8005AA34` frame, `fn_800589B0` joints, `fn_80059710` view, `fn_80059C54`
  opaque and `fn_80059F6C` translucent passes, after a full-screen fill at the far plane;
- it runs at the end of every game frame (`LMChangeFrameBuffer__Fv` hook), so it works on any
  screen (title, menus, in game) and in every frame-rate mode.

Orbit camera (drag / wheel or sliders), auto-spin, play/pause, speed and frame scrubbing. The
default animation is the archive's "wait" key. Not supported: J3D `.bmd` models (effects,
`kt_static`) and room `.bin` models. Some characters get their materials from game code at run
time (Luigi's textures are bound by the player code), so Luigi is drawn untextured; ghosts
such as Chauncey (`/model/baby.szp`) are complete. "(no animation)" uses a key with every
channel at its default, which collapses skinned models.

Test aid: `LM_DEV_VIEWER=<archive>[#<model name part>][|<key name part>]`, e.g.
`/model/baby.szp|akubi` or `/Game/game_usa.szp#luige|wait`.
