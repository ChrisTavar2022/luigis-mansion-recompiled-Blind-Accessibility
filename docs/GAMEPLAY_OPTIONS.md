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
a hit down to 0.

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

## Verification

Run with `LM_GAMEPLAY=...`; each hook logs `[gameplay] ...` the first time it changes
something (`LM_START_MAP=9/10/11` jumps to a boss map). Results are listed in the commit
message of the change that added these options.
