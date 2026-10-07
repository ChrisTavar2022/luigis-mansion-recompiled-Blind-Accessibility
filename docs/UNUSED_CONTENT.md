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

### Not done: dedicated air/water nozzles

`/model/wpwater.szp` and `/model/wpair.szp` (and `wplight`) are registered character types
7/8/9 (`lbl_8033B590`: `wplight`, `wpwater`, `wpair`, right after `vbody`/`vhead`), each
with a model-registry entry. No code ever creates those actors. Luigi's equipment is created
in `fn_80072554` (types 6 `vhead`, 5 `vbody`, 0x0A `gameboy`) and attached to joints by
`fn_800AE1A4` → `fn_800685DC` (child slots at parent +0x5C8, stride 0x40). Hiding an actor is
MoveObj flag 0x4000 at +0x40. A reconstruction would create type 8/9 actors, attach them
like `vhead`, and swap visibility while Luigi sprays.

What is known about the spray state so far:
- The Poltergust controller is `[Luigi+0x7E4]` (code in `auto_03_80073A18`, updated by
  `fn_80073AF8` from `fn_800B5324`). +0x310 is the mode (1 idle, 2 active) and +0x314 the
  sub-action. Each frame Luigi copies a 0x50-byte input block to its start: +0x04 element
  index, +0x40 request.
- `fn_8007528C` starts actions from the request: 2 = vacuum (sub 0xF); 5 = element spray, only
  when the element index is 1..3 (sub 0x11); 9 and 11 are other actions. Air blowing showed up
  as mode 2 / sub 0x0A.
- Luigi's request comes from `fn_800B5324`: 5 while Luigi +0x1114 > 0 (element spray), 6
  while +0x110C > 0. Luigi's element is +0x1184 (2 fire, 3 water, 4 ice) and the meter amount
  is +0x1188 (0..100).
- The scripted runs could not produce a sustained spray. The test Luigi was sitting in event
  dialogue, so L was ignored. The next step is to rerun with `LM_TEST_STATE=water`, periodic
  A presses or `LM_SKIP_AUTO`, then hold L and read `LM_NOZZLE_DUMP` to confirm the sub-actions
  for water and air.

No option is offered until that is confirmed.

## Other leftovers noticed

- GBH screen mode 1 (gauge + two numbers, `fn_80044C4C`) is never selected. It could be
  exposed the same way.
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
| 7 | `map7/h_05_00..03.bin` | Same room geometry as map 5 (the files differ only in their names), its own `col.mp` | Host stack overflow while loading (unbounded guest recursion); Go button disabled |
| 8 | `map8/h_06_00..03.bin` | Same room family; JMP tables in an older field layout (characterinfo 16 fields instead of 23, groupinfo 20), an E. Gadd (`lohakase`) and event 14 | Crashes while loading; disabled |

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
