# Gameplay / difficulty options

F1 > Enhancements > **Gameplay / Difficulty**. Saved in `settings.ini` under `[gameplay]`
(`port::GameplaySettings`, `src/port/settings.h`). Every option is neutral by default: at the
default value its hook calls the original function unchanged, so the original game is
untouched until a player moves a slider. All options apply in the normal mansion and in both
Hidden Mansion variants.

Implementation: `src/recomp/gameplay_hooks.cpp` (hooks listed in `tools/recomp/hooks.txt`,
all wrapping the original via `callOriginal`), plus the room-ambient hook in
`src/recomp/hidden_mansion_hooks.cpp`. Hooks are keyed by symbol name. A few use US data
addresses or struct offsets, marked "US" in the code; a PAL-base install needs the PAL
equivalents (`tools/pal_diff.py var/field` maps them; most Luigi offsets ≥ +0x134C shift by 4).

Testing override (not saved): `LM_GAMEPLAY="dark=1,ambient=0.4,damage=2,health=2,ghosthp=3,vacuum=0.5,hearts=0"`.

| Option | Setting (ini key) | Range | How |
|--------|-------------------|-------|-----|
| Hidden Mansion darkness everywhere | `pal_darkness_everywhere` | on/off | Room ambient x0.2 in every mansion (the PAL Hidden Mansion value) |
| Ambient light | `ambient_scale` | 0.05–1.5 | Multiplies room ambient light |
| Damage taken | `damage_scale` | 0–5 | Rescales every hit Luigi takes (0 = invincible) |
| Luigi's health | `health_scale` | 0.25–5 | Starting and maximum health (100 → 25..500, capped at 999) |
| Ghost health | `ghost_health_scale` | 0.25–5 | Ghost HP when a ghost's HP is set |
| Poltergust strength | `vacuum_strength` | 0.25–4 | HP the vacuum drains per drain tick |
| Disable recovery hearts | `no_heart_drops` | on/off | Heart items are removed from all item tables; any remaining heart heals 0 |
| Always ride the Poltergust | `always_ride` | on/off | Luigi's riding/gliding movement mode (the one PAL enables for Boolossus) everywhere |

## Details

**Darkness.** `fn_800168EC` computes the room's ambient colour from the current room
record's RGB (+0x2C..0x2E; lit rooms additionally x0.6) and fades towards it. The hook scales
the record's RGB for the duration of the call by `gameplay::ambientFactor()` =
(0.2 if the PAL Hidden Mansion rules are active **or** "darkness everywhere" is on, else 1)
× the ambient slider, clamped to 255. *Choice:* the slider **composes** with PAL darkness
rather than overriding it, so 1.0 always means "as the selected mode intends" and the slider
is a relative brightness/gamma-style control (e.g. 0.5 in the PAL Hidden Mansion = 0.1 of the
original). Light sources (candles, lamps, flashlight) are not ambient and are unaffected.

**Damage.** `fn_800B9488(luigi, newHp)` is the single setter of Luigi's health (+0xFC):
it clamps to [0, max (+0xFFC)] and plays the hurt/heal reactions. All damage (ghost
attacks, bombs, floors, the Hidden Mansion's x2 in `fn_800B9680`) reaches it as a lower value,
so the hook rescales the difference; healing passes through. Non-zero multipliers never round
a hit down to 0. Calls from Luigi's message handler (`fn_800AEEA8`, script/event commands that
*set* health, e.g. on arriving in the mansion) are not damage and are passed through, except
that a scripted "full health" of 100 becomes the scaled full health.

**Luigi's health.** Luigi's constructor (`fn_800AD5AC`) sets health = max = 100; the hook
scales both. `fn_800AE1A4` restores health/max from the state carried between maps; the hook
scales a maximum that is still the original 100. Health is shown and capped at 999.

**Ghost health.** Every ghost's AI (`EnThought`, message handler `fn_800C0F08`) sets its
actor's HP (+0xFC, mirrored in +0x89C) on message 6 (HP from spawn data) and 7 (per-type
HP from `EnTypesManager::getEnemyLife` or a table); the hook scales both afterwards. Bosses
with their own HP code are not covered yet.

**Poltergust strength.** The capture update (`fn_80081BCC`) drains the captured ghost by one
HP per drain tick (capture +0x98 = ghost HP − 1). The hook replaces each tick's 1 with the
strength, carrying fractions, so 0.5 drains half as fast and 2 twice as fast. The suction
*pull* (`fn_80081B5C`, scale `0x804D8C00`) is unchanged.

**Recovery hearts.** Hearts are spawned by item name (`sheart`, `mheart`, `lheart`,
`move_*heart`) from the map's JMP tables (furniture item tables, `itemappeartable`,
`iteminfotable`, ghost drops), all read through `ToolData::getValue(row, field, const char**)`.
With the option on, the hook returns `nothing` for any heart name. As a backstop the heart
item's setup (`fn_800D1458`, `EnHeartItem`) sets its heal amount (+0x238) to 0, so a heart from
any other source heals nothing. Tables are read when a map loads, so the option takes full
effect at the next map load.

**Always ride the Poltergust.** The US Luigi contains a complete alternate movement mode
behind byte +0x134C (PAL +0x1350): faster run (38 vs 17/18), momentum/gliding physics, its own
animations and a sound loop (docs/HIDDEN_MANSION.md, *Bosses*). PAL turns it on only in the
Boolossus fight; this option sets it every frame in Luigi's update hook (`fn_800B783C`).
It is **suspended** (forced off) outside the mansion and boss arenas — E. Gadd's lab, the
training room, the gallery and the ending maps, which position Luigi by script — and while
Luigi has no health (death sequence). Turning the option off clears the byte again.
Checked: the opening cutscene and arrival in the foyer are unaffected (cutscenes drive Luigi
directly), Luigi glides with momentum and stops against walls. Not yet checked in play:
doors, stairs, the elevator-like transitions and vacuuming/flashlight while gliding; if one
misbehaves, add its condition to `gameplay::rideAllowed()`.

## Verification

Run with `LM_GAMEPLAY=...` (e.g. `ride=1`); each hook logs `[gameplay] ...` the first time it
changes something (`LM_START_MAP=9/10/11` jumps to a boss map; `LM_HM_TRACE_LUIGI=1` logs
Luigi's position and riding mode). Tested with a scratch `APPDATA` and `LM_CARD_DIR`:

| Option | Test | Result |
|--------|------|--------|
| Luigi's health x2 | new game → Chauncey (map 10) | HUD shows 200; `Luigi starts with 200`, `restored Luigi max 200` |
| Luigi's health x1.5 + damage x2 | new game → foyer | HUD shows 150 (the arrival script's health set is not treated as damage) |
| Damage x0 | Chauncey's ball hits Luigi | `hit 5 -> 0`, health stays 200 |
| Ghost health x2 | map 10 | `ghost HP x2.00: 100 -> 200` |
| Darkness everywhere / ambient 1.5 | normal-mansion foyer | foyer visibly darker / brighter than original |
| Disable hearts | foyer load | `table item move_sheart -> nothing` |
| Always ride | opening + foyer | cutscene and arrival normal; gliding movement (trace) |
| Poltergust strength | — | implemented; not yet exercised in play (needs a scripted ghost capture) |
