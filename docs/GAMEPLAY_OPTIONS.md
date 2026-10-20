# Gameplay / difficulty options

F1 > Enhancements > **Gameplay / Difficulty**. Saved in `settings.ini` under `[gameplay]`
(`port::GameplaySettings`, `src/port/settings.h`). Every option is neutral by default: at the
default value its hook calls the original function unchanged, so the original game is
untouched until a player moves a slider. All options apply in the normal mansion and in both
Hidden Mansion variants.

Implementation: `src/recomp/gameplay_hooks.cpp` (hooks listed in `tools/recomp/hooks.txt`,
all wrapping the original via `callOriginal`), plus the room-ambient hook in
`src/recomp/hidden_mansion_hooks.cpp`. Hooks are keyed by symbol name and apply to both
releases (`us pal`); data addresses go through `VA()` (docs/SETUP.md). European specifics: Luigi's
riding byte is +0x1350 and the European Luigi update rewrites it every frame, so "Always ride"
sets it in his state update `fn_800B84E4`; the PAL Hidden Mansion's 0.2 room ambient is the
game's own there, so "darkness everywhere" does not apply it twice.

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
a hit down to 0. Calls made directly by Luigi's message handler (`fn_800AEEA8`: heals, and the
event-script commands that *set* health) are not damage and pass through unchanged.

**Where the game keeps health.** During a map, health and maximum live in Luigi's actor
(+0xFC / +0xFFC). Between maps and in the save file they live in his persistent player record
(Luigi +0x79C = `lbl_803D5CD0` + 0x16C × player; +0 health, +2 maximum, +4 + 2 × i the
remaining HP of portrait ghost i, index from the portrait table `fn_800553FC`, 23 entries).
Luigi's message 59 (leaving a map) copies the actor into the record and `fn_800AE1A4` copies
the record into the new actor, which also sends Luigi's own AI (`EnThought`, like every
character's) message 6 with that health. The record is copied into the save as a whole.

**Rule for every option:** nothing scaled is ever stored in the record. It always holds the
original units, so options never compound across map loads, are never written into the save,
and with a slider back at 1 everything is the retail value again.

**Luigi's health.** Luigi's message handler is wrapped (`gameplay::callLuigiMessage`, from the
`fn_800AEEA8` HLE in `luigi_states.cpp`). Entering a map (`fn_800AE1A4`), health and maximum are
the record's values × the scale (the HUD health global `fn_80188D04` is updated too); leaving
(message 59) the record gets the actor's values ÷ the factor this Luigi was set up with
(rounded; after a round trip the value is a fixed point, at most k/2 HP of drift once). The
constructor (`fn_800AD5AC`) scales its 100/100 for a Luigi no record restores. Event-script
commands 14 (set health, data +0x48) and 15 (set maximum, +0x4C), e.g. health = 100 on
arriving in the mansion, are given scaled values for the call. Message 21 (a capture) ranks
the boss portraits (types 64, 213, 220, 221) by Luigi's health (≥ 90 / ≥ 60): the handler sees
his health in original units for that call. Heals (hearts: 10/20/50) are not scaled. A slider
change applies at the next map load. Health is shown and capped at 999.
*v1.0 bug:* the scaled maximum was stored in the record, so a save made at x2 kept a maximum
of 200 with the slider back at 1 (and the "scripted 100 → scaled" special case also turned a
heal that happened to reach 100 into a full scaled heal). Saves written by v1.0 with the
option on keep their stored maximum; nothing converts them.

**Ghost health.** Every character's AI (`EnThought`, message handler `fn_800C0F08`) sets its
actor's HP (+0xFC, mirrored in +0x89C) on message 6 (HP from spawn data) and 7 (per-type
HP from `EnTypesManager::getEnemyLife` or, for portrait ghosts, the record); the hook scales
both afterwards, **for ghosts only**: actors of type 0 (Luigi, player 2, Mario) are skipped.
*v1.0 bug (user report):* Luigi's message 6 on every map load was scaled too, so "Ghost health"
0.5 halved Luigi's health at each map load (100 → 50 → 25 → 13 → 7 → 4 in a warp test) and
the halved value was saved. Portrait ghosts keep their remaining HP in the record: message 7
reads it and the thought update `fn_800C08DC` (hooked, `us pal`) writes the current HP back
every frame. The hook rewrites that entry ÷ the factor the ghost was set up with (kept per
thought), so a partly drained portrait ghost comes back with the same share of its HP and the
record never compounds (v1.0 halved a portrait ghost's stored HP at every encounter and saved
it). Bosses with their own HP code (Chauncey's HP comes from `getEnemyLife`, not the record)
are covered by message 7 like any ghost.

**Poltergust strength.** The capture update (`fn_80081BCC`) drains the captured ghost by one
HP per drain tick (capture +0x98 = ghost HP − 1). The hook replaces each tick's 1 with the
strength, carrying fractions per ghost (v1.0 shared one carry across all ghosts), so 0.5
drains half as fast and 2 twice as fast. The suction *pull* (`fn_80081B5C`, scale
`0x804D8C00`) is unchanged. Nothing is stored; portrait ghosts' remaining HP reaches the
record through the ghost-health rule above.

**Recovery hearts.** Hearts are spawned by item name (`sheart`, `mheart`, `lheart`,
`move_*heart`) from the map's JMP tables (furniture item tables, `itemappeartable`,
`iteminfotable`, ghost drops), all read through `ToolData::getValue(row, field, const char**)`.
With the option on, the hook returns `nothing` for any heart name. As a backstop the heart
item's setup (`fn_800D1458`, `EnHeartItem`) sets its heal amount (+0x238) to a marker; its
pickup (`fn_800D1A24`) sends the amount to Luigi as message 13, and the wrapped handler
swallows a message 13 carrying the marker, so a heart from any other source heals nothing.
*v1.0 bug:* the backstop wrote 0, which message 13 treats as "heal to full health". Tables are read when a map loads, so the option takes full
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

### Side-effect audit (v1.0 user report: "lowering Ghost HP also lowers Luigi's HP")

`LM_GAMEPLAY_LOG=1` logs every scaling with its target (ghost HP with type and whether it came
from the record, portrait record write-backs, Luigi's record on leaving / entering a map,
vacuum ticks); `LM_HEALTH_LOG=1` logs Luigi's health and maximum with the map. Routes:
new game (`--input "480:START*10,880:A,1000:A,1100:A,1200:A,1300:A,1400:A"`,
`LM_SKIP_CUTSCENES=1 LM_SKIP_AUTO=30 LM_TEST_STATE=poltergust`) with
`LM_DEV_WARPS=1@2600,2@3400,3@4200,2@5000,1@5800,2@6600` (lab / mansion / training room),
`LM_TEST_HIT=2500,0,30`, `LM_TEST_SAVE=2400` (save in a scratch `LM_CARD_DIR`, then loaded by a
second run on the same card), `LM_SPAWN=46@2600,...` (portrait ghosts), `LM_START_MAP=10`.

| Option | Problem found (v1.0) | Now (verified) |
|--------|----------------------|----------------|
| Ghost health | Luigi's own AI gets message 6 with his carried health on every map load and was scaled: x0.5 gave 100 → 50 → 25 → 13 → 7 → 4 over five loads, saved. Portrait ghosts' stored HP was scaled again at every encounter and saved | Luigi stays 100/100 through 6 loads at x0.5; ghosts x0.5 every load without compounding (Chauncey 100 → 50 each entry, map 10 ↔ 2); portrait ghosts 50 in play, record 100 |
| Luigi's health | Scaled maximum stored in the record/save: slider back at 1 kept 200 (or 50); a heal reaching exactly 100 became a full scaled heal; boss portrait rank compared scaled health with 90/60 | x2: 200/200, hit 30 → 170/200, record 85/100 on every map exit; the save loads as 85/100 at x1 and 43/50 at x0.5 |
| Damage taken | none (the scripted-100 special case moved to Luigi's health) | unchanged |
| Disable recovery hearts | backstop heal 0 = "heal to full" in Luigi's message 13 | marker swallowed (code path; a non-table heart was not staged) |
| Poltergust strength | fraction carry shared by all ghosts | per ghost; stored HP goes through the ghost-health rule |
| Darkness / ambient | none: room RGB scaled only during `fn_800168EC` and restored | — |
| Always ride | none: byte set per frame, US/PAL offsets via the PAL hook | — |
| Unfinished Luigi states | none: poison writes health directly (its own damage slider), burn damage goes through the setter (scaled by "Damage taken", as intended) | — |

European base (`LuigisMansionRecomp --pal-disc`, setup-built module): the same route gives the
same numbers (record at `0x803C3290`, portrait path, 85/100 stored at x2). All options at their
defaults: Original 30 frame dumps (`LM_FRAME_DUMP`, frames 2300-3100, with `LM_TEST_HIT` hits)
byte-identical to the build before these changes.
