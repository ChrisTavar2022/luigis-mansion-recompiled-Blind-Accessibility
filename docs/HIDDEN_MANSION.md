# PAL Hidden Mansion on the US build

What the PAL release (GLMP01, rev 0) changes in the Hidden Mansion compared to the US release
(GLME01, rev 0), where each change lives, and how the port brings it to the US-based build.
Everything below was found with `tools/pal_diff.py` against the user's own dumps; the commands
to reproduce each finding are given inline. Addresses are virtual (0x80......); "US" is GLME01,
"PAL" is GLMP01_00.

## Tools

| Tool | Purpose |
|------|---------|
| `tools/pal_diff.py code [--tsv f]` | Matches all functions of the two DOLs and lists what changed |
| `tools/pal_diff.py show <US fn> [--pal ADDR] [--side pal]` | Side-by-side disassembly; PAL calls/data are labelled with US names |
| `tools/pal_diff.py var <us\|pal> <ADDR>` | Maps a variable to the other version and lists every use in both |
| `tools/pal_diff.py xref <us\|pal> <ADDR> [--exact]` | Code that references an address (or the object containing it) |
| `tools/pal_diff.py data` | Same-shaped data objects whose bytes differ (pointers masked) |
| `tools/pal_diff.py files [--deep]` | Disc files that differ, and the members inside differing archives |
| `tools/pal_diff.py arcdiff A.szp B.szp [--path RE]` | Two archives member by member; JMP tables row by row with field names |
| `tools/pal_diff.py strings <regex>` | Strings in both DOLs |
| `tools/hidden_mansion_data.py` | Builds the data overlay the port serves in PAL Hidden Mansion mode |

How the matcher works: every function in `decomp/config/*/symbols.txt` is reduced to a
fingerprint of opcodes and registers (branch targets and 16-bit immediates masked). The two
address-ordered lists are aligned with difflib, then moved functions are paired by identical
fingerprint and the rest by similarity between the same matched neighbours. The PAL DOL was
built with a different compiler patch level that schedules some instructions differently, so
fingerprints compare the *multiset* of instructions. Pairs are then compared strictly:
immediates directly, address operands by the value they load from `.rodata`/`.sdata2` (or
ignored for writable data), and `bl` targets through the function map. Functions whose only
difference is struct field offsets are reported as `layout` (several classes gained members in
PAL, e.g. Luigi gained fields at +0x134C/+0x1350, shifting later fields by 4).

Current result: 10,984 US / 11,055 PAL functions; 10,432 identical, 51 layout-only, 164 with
constant/call differences, 282 modified, 55 US-only, 126 PAL-only. Almost all of the non-HM
differences are PAL infrastructure: language selection, 50/60 Hz display setup
(`LMDisplayUtil`), the PAL boot/file-select screens, and an updated SDK (CARD/DVD/GX).

## The hidden-mansion flag

Both versions keep the mode in the same `.sbss` word:

| | US | PAL |
|---|---|---|
| "ura" (hidden mansion) flag | `0x804D80C0` (r13+0x160) | `0x804C5784` (r13+0x164) |
| deferred "enter ura" request (PAL only) | – | `0x804C5780` (r13+0x160) |
| current map number `sCurMapNo` | `0x804D80C8` | `0x804C578C` |
| language (PAL only; 0=J 1=E 2=G 3=F 4=S 5=I) | – | `0x804BD658` (.sdata, initial 1) |

`python tools/pal_diff.py var pal 804C5784` prints the mapping and both use lists: **9 uses in
US, 44 in PAL** (36 PAL functions). The flag is set by the `<URALUIGI>` event command and
cleared by `<OMOTELUIGI>` and on new game (US `fn_8000B978`). The event scripts also keep save
flag 8 ("ura") in step with it.

## Code differences (every use of the flag)

"Both" means US already does the same thing; everything else is new in PAL. Function names
are the US symbols; most game functions are still unnamed in the decomp.

### Mode switching

| PAL | US | Change |
|-----|----|--------|
| `8006656C` | `fn_8006385C` (event command interpreter) | `URALUIGI` now sets the deferred request `0x804C5780 = 1` instead of the flag; `OMOTELUIGI` clears the flag (both) |
| `8000CDE8` | `fn_8000BF38` (scene start) | New: if the request is set, clear it and set the flag, before the scene's subsystems are created |
| `8000C050` | `fn_8000B978` (new game) | Both: flag = 0 |
| `800BDF64` | `init__Q24Koga11MissionModeFv` | New: when flag && map 2, load `/Map/map2u.szp` (`"%s%du.szp"`) instead of `/Map/map2.szp` |

### Mirror (the PAL hidden mansion is left/right mirrored)

The 3D scene is mirrored, the HUD is not, and everything that ties the two together is patched:

| PAL | US | Change |
|-----|----|--------|
| `80062388` | `fn_8005F8B8` (game scene draw, called from `fn_8000BCE8`) | The full-screen quad that redraws the EFB copy of the 3D scene uses flipped U texcoords when flag == 1 — this is the mirror |
| `80082838`, `80083724`, `80083934` | `fn_8007F9B8`, `fn_800807EC`, `fn_800809CC` (Luigi input, called from Luigi update `fn_800B2084`) | Main/C stick X (pad +0x44 / +0x54) negated |
| `80042720` | `fn_80041214` (HUD, lifemeter/`dmman` unit) | Screen X of positions the game projects itself: `x = 640 - x` (two sites) |
| `80057B94` | `fn_80055C54` (iris wipe, `res_crcl.szp`) | Wipe rectangle centred at x = 328 instead of 312 |
| `8002ADA0` | `fn_80029A74` (Game Boy Horror map init) | New variable `0x804C5A10` = -1 (hidden) / +1 |
| `8002B124`, `8002B3E4` | `fn_80029DD4`, `fn_8002A01C` (GBH map draw) | Concatenates `PSMTXScale(-1, 1, 1)` onto the map's view matrix |
| `8002B9C8`, `8002BB98`, `8002C2DC` | `fn_8002A5A0`, `fn_8002A74C`, `fn_8002AE74` (GBH map markers) | X multiplied by the sign variable |
| `8018BAFC`, `8018C500` → `8019F8E0` | `fn_8018865C`, `fn_80189054` | Set the JAudio flag `0x804C6A21` = ura |
| `8019F900` | `Jac_VframeWork` (`8019C360`) | Swaps the left/right mix buffers in `Jac_imixcopy` when that flag is set (stereo swap) |

### Difficulty and tuning

| PAL | US | Change (normal → hidden) |
|-----|----|--------------------------|
| `800BCA98` | `fn_800B9680` (Luigi takes damage) | Both: damage x2 |
| `800B7534` | `fn_800B41E0` (Luigi, damaging floor) | Hidden: damage on 5 of every 8 frames (table `0x804C8D5C` = 1,1,0,1,1,0,1,0) instead of every 2nd frame; each hit still x2 |
| `800EA4F8`, `800EAAD0` | `fn_800E70BC`, `fn_800E7698` (heart items, `Koga/800E634C.cpp`) | Threshold on Luigi field +0x6C8: 200 normal, **400 in US hidden, 1000 in PAL hidden** |
| `800179F8` (new, split out of `80017BC8`) | `fn_800168EC` (room ambient colour) | Ambient RGB additionally x0.2 (`0x804BD728`) — darker rooms |
| `800A2204` | `fn_8009F1DC` (`KtEnBaby`, Chauncey) | A distance becomes 900-200 = 700 instead of 200 |
| `800A2CFC` | `fn_8009FCA8` (Chauncey) | Sets field +0xD8 = 2.0 |
| `800A2DBC` | `fn_8009FD54` (Chauncey) | Threshold 20 → 40 |
| `800A3A50` | `fn_800A09D0` (`KtEnMoku`, Chauncey's rocking horses) | Scale 1.8 → 3.6 |
| `800A48E4` | `fn_800A184C` (`KtEnMoku`) | Rewritten (0x1A8 → 0x308 bytes) with hidden-only branches |
| `800A4DC4` | `fn_800A1BCC` (`KtEnMoku2`) | Argument to `fn_8008907C` 50 → 100 |
| `800A7640` | `fn_800A441C` (Bowser / `dkopa` boss) | Toggles field +0x1EE when its counter reaches 3 |
| `800BABC8` | `fn_800B783C` (Luigi) | Map 11 + hidden: counts frames in new field +0x134C, sets +0x1350 after 15 |
| `8006FD0C`, `8006FE44`, `8006FF88`, `800702A0`, `80071240` | `fn_8006CFBC`, `fn_8006D0DC`, `fn_8006D1B8`, `fn_8006D4B8`, `fn_8006E414` (enemy strategy, state table `0x8034BB48`; not yet identified) | State timer 400 → 250, early exit below 130, colour words, extra parameters |
| `8003F444` | `fn_8003DF88` (GBH screen) | Both: picks `fn_8003B668` instead of `fn_8003B700` |
| `800DF2CC` | `fn_800DBE8C` | Both: value x0.5 |
| `80084B64` | `fn_80081BCC` | Both |

No other PAL code reads the flag, and no PAL code checks save flag 8 through
`InGameFlagsUtil` with a constant argument. The portrait-ghost/boss parameter files
(`Game/game*.szp` `param/ctp/*.prm`) are identical between US and PAL except `banaoba.prm`,
so there is no separate boss HP table: boss differences are the code changes above and the
map data below.

## Data differences

### `/Map/map2u.szp` (PAL only) vs `/Map/map2.szp`

`python tools/pal_diff.py arcdiff "<PAL>/files/Map/map2.szp" "<PAL>/files/Map/map2u.szp"`.
Collision (`col.mp`), polygon, room, object, group and event tables are identical. Changed:

| Table | Change |
|-------|--------|
| `jmp/telesa` (the 35 Boos) | Higher `str_hp` (e.g. 40 → 100, 50 → 200, 150 → 300), much higher `max_speed` (up to 24 → 132), longer `move_time`, `attack` 0 → 1 on several, an unnamed field (`#16abce3`) 3 → 5..15 |
| `jmp/enemyinfo` | 186 → 258 rows (92 added): more ghosts (`obake`, `mapoo`, `topoo`, `mopoo`, `tenjyo`, `skul`, ...), many existing ghosts changed to other types |
| `jmp/teidenenemyinfo` | 172 → 175 rows (blackout ghosts) |
| `jmp/generatorinfo` | 84 → 90 rows; spawn counts changed |
| `jmp/iyapootable` | Speedy spirits / gold mice drops: coins 20 → 30, bills → 20, gold 1; emerald/ruby/sapphire columns replaced by `diamond` |
| `jmp/characterinfo` | Six treasure chests (`rtakara1`, `btakara1`, `gtakara1`, `wtakara1`) moved to other spawn slots |
| `jmp/keyinfo` | `key01` spawn slot `38_4` → `38_5` |
| `jmp/furnitureinfo` | 47 furniture item tables changed |
| `jmp/itemappeartable` | 16 → 21 rows |
| `jmp/railinfo`, `jmp/observerinfo`, `path/*` | Paths for the added ghosts; 18 new `path/` entries |
| `jmp/itemtable` | Dropped (no code references it) |

Every ghost type named in `map2u` already appears in `map2`, so the US code can spawn all of
them. JMP tables are looked up by field-name hash, so the US code also reads PAL's extra
`roominfo` fields harmlessly.

### Event scripts

Event archives (`/Event/eventNN.szp`) hold a language-neutral script (`text/eventNN.txt`) and
message tables. PAL changed three scripts for the hidden mansion:

* `event08`: adds `<OMOTELUIGI>` + `<FLAGOFF>(8)` at the start and `<FLAGON>(8)` right before
  `<URALUIGI>` (a dated "bug fix" comment).
* `event16`, `event73`: add a `<CHECKFLAG>(8)"ura""omote"` branch; in the hidden mansion the
  Boo-count checks use `<CHECKTELESA>(40)(41)(42)(43)(44)` instead of `(19)(29)(34)(38)(39)`
  (event16) and `(35)..(39)` (event73).

The US interpreter already knows every tag used (`CHECKFLAG`, `CHECKTELESA`, `COUNTJMP`,
`OMOTELUIGI`, ...). The "omote" branches are byte-for-byte the US logic, so the PAL scripts
are safe in the normal mansion too. `event48`'s `entrdemo.cmn` (camera demo) also differs but
is not hidden-mansion specific and is left alone.

### Text and languages

PAL ships every language: `message/<language>/...` inside each event archive, `Game/game_X.szp`
+ `sdata_X.arc`, `Kawano/<e|f|g|i|s>/res_*.szp`, `Ending/<language>.szp`, and a language
select screen (`Kawano/res_lngg.szp`). The language index lives at `0x804BD658` and is read by
22 functions. None of this is needed: the PAL hidden-mansion changes add no new message lines,
and the PAL English message tables are identical to the US ones in 100 of 102 event archives
(events 39 and 80 differ by two bytes). The port keeps the US text and only swaps the scripts.

### Frame timing

PAL data uses the same per-frame units as US: the normal-mansion `map2` tables match US apart
from the few rows `arcdiff` lists, so nothing was rescaled for 50 Hz. PAL frame counts (e.g.
Boo `move_time`) are therefore used unchanged. On a 50 Hz PAL console they last 20% longer in
wall-clock time than they will in the 60 Hz US build; scaling them is a design choice, not a
porting requirement. (No PAL60 option was found in the PAL DOL.)

## Implementation in the port

The mode is selected with `LM_HIDDEN_MANSION=pal` (default: US rules). Prototype:
`src/recomp/hidden_mansion.{h,cpp}` (compiled into `lmrecomp`, not yet called).

### 1. Data overlay (done in the prototype)

`python tools/hidden_mansion_data.py` writes `orig/hidden_mansion/` (git-ignored) from the
user's two dumps: `Map/map2u.szp` copied from PAL, and `Event/event{08,16,73}.szp` = the US
archive with only the script replaced (stored as literal-only Yay0 so the container format is
unchanged). `hm::redirect()` / `hm::overlayEntry()` / `hm::overlayRead()` serve those files:
map2 only while the flag is set and the map is 2 (exactly PAL's condition), event archives
always in PAL mode. Integration: in `hle_dvd.cpp`, check `hm::overlayEntry(path)` in
`DVDConvertPathToEntrynum`/`DVDOpen`, and route `DVDFastOpen`/`DVDRead*` for
`hm::isOverlayEntry(entry)` to `overlayLength`/`overlayRead`.

### 2. Constant patches (done in the prototype)

Where a PAL change multiplies a literal that in the US build sits in `.sdata2` and is used by
exactly one function, `hm::syncConstants()` rewrites the literal while the flag is set and
restores it otherwise (it first checks that the US value is there):

| US literal | Value | PAL hidden | Used only by |
|------------|-------|------------|--------------|
| `0x804DB278` | 1.8 | 3.6 | `fn_800A09D0` (rocking horse scale) |
| `0x804DB2B0` | 50 | 100 | `fn_800A1BCC` |
| `0x804D9B60` | 0.6 | 0.12 | `fn_800168EC` (lit rooms only; the unlit branch needs a hook) |

Integration: call `hm::syncConstants()` once per retrace (`frame.cpp`).

### 3. Mirror at the HLE level (helpers in the prototype)

Rather than copying PAL's flipped-texcoord quad, flip the perspective projection in the GX
HLE (`hm::mirrorProjection` in `GXSetProjection`) and swap front/back culling
(`hm::mirrorCullMode` in `GXSetCullMode`). Orthographic 2D/HUD stays unmirrored, which is
what PAL produces, and the GBH map's 3D view is mirrored for free (PAL mirrors it
explicitly). The stereo swap belongs in the host audio mixer (`hm::mirrorStereo`) once audio
is emulated. Still needed as hooks: stick X for Luigi's input (3 functions), projected HUD
X (`fn_80041214`), iris wipe centre (`fn_80055C54`), GBH map markers (3 functions).
Validate against Dolphin running the PAL disc: anything PAL draws into the 3D pass with an
orthographic projection would not be mirrored by this method.

### 4. Hooks for the remaining behaviour

These need a hook that runs the original US function and then adjusts a value, or adds a
branch. `tools/recomp/hooks.txt` + `HLE(name)` today *replaces* a function and cannot call the
translated original, so the recommended prerequisite is a small `recomp.py` change: emit each
hooked body as `f_XXXXXXXX_orig(Ctx&)` and let the hook call it (`hleCallOriginal`). With that:

| Hook (US) | Behaviour to add when `hm::palActive()` |
|-----------|-----------------------------------------|
| `fn_8000BF38` / event `URALUIGI` | Optional: PAL's deferred flag set (only matters for scene-change edge cases) |
| `fn_800E70BC`, `fn_800E7698` | Threshold 1000 instead of 400 |
| `fn_800B41E0` | Damaging-floor pattern (5 of 8 frames) |
| `fn_800168EC` | Ambient x0.2 on the unlit branch (the lit branch is a constant patch) |
| `fn_8009F1DC`, `fn_8009FCA8`, `fn_8009FD54` | Chauncey tuning (700 / +0xD8 = 2.0 / 40) |
| `fn_800A184C` | `KtEnMoku` hidden-only branches (study PAL `800A48E4` first) |
| `fn_800A441C` | Bowser +0x1EE toggle |
| `fn_800B783C` | Map 11 frame counter (needs 5 bytes of state kept by the hook, since the US Luigi object has no +0x134C field) |
| `fn_8006CFBC`, `fn_8006D0DC`, `fn_8006D1B8`, `fn_8006D4B8`, `fn_8006E414` | Enemy strategy timers/colours (identify the enemy first) |
| `fn_8007F9B8`, `fn_800807EC`, `fn_800809CC` | Negate stick X |
| `fn_80041214`, `fn_80055C54`, `fn_80029A74`, `fn_8002A5A0`, `fn_8002A74C`, `fn_8002AE74` | Mirror fix-ups for HUD, wipe and GBH map |

Each hook is written from the behaviour described here, not translated from either binary.
Verify each one by playing the same input recording in Dolphin (PAL disc, hidden mansion) and
in the port, and comparing per-frame state.

### Order of work

1. Wire the overlay into `hle_dvd.cpp` (gives PAL ghosts, Boos, treasure and Boo gates).
2. Mirror via GX projection/culling, then the input and HUD fix-ups.
3. `recomp.py` call-original support, then the tuning hooks from the table above.
4. Constant patches stay as they are (no hooks needed).
