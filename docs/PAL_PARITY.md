# PAL-base parity (October 2026)

Every port feature checked on the European game (GLMP01) as the base, next to the USA game
(GLME01). Runs: the European dev build (`lmrecomp` with `-DLM_DOL_VERSION=GLMP01`), the USA
dev build, and the shipped `LuigisMansionRecomp.exe` (fresh `APPDATA`, PAL disc only and both
discs). Scratch `APPDATA`/`LM_CARD_DIR` for every run; screenshots by PrintWindow at 1280x720
(16:9) and 1680x720 (21:9). How the version mapping works: docs/SETUP.md, *Version-aware code*.

Legend: **works** = checked, no change needed; **fixed** = broken or USA-only before, works
now; **gated** = greyed out in F1 with the reason.

| Feature | USA | PAL | Notes |
|---|---|---|---|
| Shipped setup, PAL disc only (fresh AppData -> module build -> game) | works | works | ~25 s module build, then title, foyer |
| Shipped setup, both discs, PAL as base + USA Hidden Mansion | works | works | both modules + `hidden_mansion/usa/` built; USA map data served, no mirror |
| Frame presentation | works | **fixed** | European double-buffered frame end (`fn_80007844`) dropped ~1 frame in 6 (25 fps shown): the guest waits for the host in `GXCopyDisp`, so no retrace callback fell between two frames. Now every copied frame is shown (`hle_vi.cpp`, `palfn_80007844` hook). 0 drops in a 70 s run |
| 50 / 60 Hz (PAL60) | n/a | works | game speed the same (29.7 / 29.9 frames/s measured); stale "20% faster" comments corrected |
| Languages (English, German, French, Spanish, Italian) | n/a | works | pause screen checked in all five; per-language `game_<l>.szp` loaded |
| Widescreen HUD anchoring (health, GBH + money tally, element meter, room name) | works | works | 16:9 and 21:9, "Keep HUD at 4:3" off |
| Pause screen | works | works | centered 4:3 at 16:9 (was untested on USA) |
| GBH screens (map) | **fixed** | **fixed** | the map is drawn into a viewport over the GBH screen (112,84 416x312) that was stretched across the wide frame, so it showed beside the bezel and was distorted; its scene (`fn_8000C598`) now runs in a centered anchor scope like the picture-in-picture views |
| GBH live camera (D-pad Up) | works | works | |
| GBH E3 clock (D-pad Left) | works | **fixed** | instruction patch had only the USA address; European line `patch 3 8003E9C0` |
| GBH ghost tracker (D-pad Down) | works | **fixed** | European lines `patch 1/2 80045C20` (same `li r0, 0` in `fn_80045B30`) |
| Beta ghost shading | works | works | checked in the ballroom (ordinary room, ghosts) on both: ghosts lit brighter |
| Air / water nozzles | works | **fixed** | hook `fn_800B36B8` tagged `us pal`, tables via `VA()`; water spray re-types the nozzle to `wpwater` |
| Early Poltergust (vbody) | works | **fixed** | the European resident archive is `game_<l>.szp`; its `vbody.arc` is identical, so the key order read from `game_usa.szp` (also on the PAL disc) holds |
| Early Bogmire | works | **fixed** | loaded and swapped in map 13 |
| Early Luigi | works | **fixed** | model built and swapped |
| Green Toad | works | works | was already `VA()`-based |
| Beta skybox, GBH-map Luigi | works | **fixed** | `VA()` for the sky descriptors; resource hooks tagged |
| Heart / star doors | works | **fixed** | door table reached through the map table (`lbl_802FF218[2] + 0x18`), which finds the European copy |
| Portrificationizer portrait models | works | **fixed** | table base via `VA()` (identical layout) |
| Leftover rooms (unused rooms) | works (WIP) | **gated** | hooks on Luigi's appear point are USA-only; F1 says "not available on the European version yet" |
| Debug: particle report, depth blur, info overlay (F3), model viewer (F4) | works | **fixed** | the whole Debug tab and F3/F4 were blocked on a PAL-only install |
| Debug: AR debug modes (debug flags, info display, scan boxes, axes, HUD test) | works | **fixed** | hooks tagged, HUD-test return address via `VA()`. Before, the HUD/font test flags were written at USA addresses on the European game every frame (memory corruption) |
| Debug: font test | works | works | European font test already prints Latin lines; the "Latin last line" fix is USA-only (greyed with a tooltip on PAL) |
| Map warps + skip entry cutscenes | works | works | maps 1, 4, 13, 10 (entry events held back), unused 5, 7, 8 |
| Flashlight shadows, shadows at any distance | works | works | `[shadow]` refits logged on PAL |
| Lightning shadows (`LM_LIGHTNING`) | works | **fixed** | threshold literal via `VA()`, `fn_8002D7C8` tagged; strikes forced in the ballroom |
| `LM_ROOMS_LIT`, `LM_TEST_STATE`, `LM_TELEPORT` test aids | works | **fixed** | `VA()` |
| Interpolation (dust, spiderwebs) | works | **fixed** | with the drops fixed, tapes pair like USA (5-7 unpaired values per tape instead of ~700). European tapes are stamped with the game's own N/60 s frame period instead of 50 Hz retraces (the blend interval jittered between 16.7 and 33.4 ms) |
| Shader precache | works | **fixed** | full `tools/pipelines/sweep.py` run on the European build merged into `data/pipelines.bin` (1999 -> 2148 configs) |
| Hidden Mansion, USA rules on the European game | n/a | works | foyer (not mirrored, USA lighting), Chauncey / Boolossus / Bowser maps run; Boolossus fight played to a game over |
| Hidden Mansion, European rules | works (overlay) | works (native) | mirrored, darker; Boolossus fight with the European mechanics |

## Notes for later work

- Patches in `tools/recomp/hooks.txt` are not mapped by `gen_version_map.py`: a patch for both
  releases is two lines, the USA one and one tagged `pal` with the European address.
- Performance comparisons between runs are only meaningful with the same machine load; side by
  side the two releases interpolate at the same rate.
