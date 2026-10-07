# Luigi's Mansion PC — Roadmap

A source port in the style of [Dusklight](https://github.com/TwilitRealm/dusklight), built on
[Aurora](https://github.com/encounter/aurora). Base executable: **GLME01 (USA rev 0)**, with
the PAL Hidden Mansion ported in from **GLMP01 (PAL rev 0)**.

## Where we start (lm-decomp @ c347326, 2026-10-06)

| Area | Code matched |
|------|--------------|
| Whole binary | ~18% (2,702 / 10,985 functions) |
| Game code (Ajioka/Koga/Sato/…) | ~13%, 5 / 61 units complete |
| SDK (Dolphin OS/GX/…) | 100%, replaced by Aurora anyway |

Dusklight could start building straight away because the TP decomp was essentially finished.
We can't: most game logic, plus large parts of JSystem (J3D, JAudio, JParticle), still has to be
decompiled. The decomp's `symbols.txt` and `splits.txt` (in `decomp/config/`) give us the name,
address and translation unit of every function, which is our work map.

Our code does **not** need to byte-match. It only needs to behave identically. That makes each
function much faster to write than a matching decomp, but every unit still needs behavioral
verification against Dolphin (see Phase 1).

## Phase 0: Foundation *(done)*
- [x] Repo, vendored decomp (CC0), Aurora submodule, `.gitignore` that excludes game data
- [x] Toolchain: VS 2026 Build Tools (clang-cl), CMake, Ninja; `tools/build.ps1`
- [x] Aurora window/renderer bring-up; JSystem core (JKernel/JSupport/JMath/JUtility/
      JGadget/J2DGraph) from Dusklight compiles and links
- [x] Game runs on its own thread; emulated 59.94 Hz VI retrace drives timing (29.97 ticks/s)
- [x] Disc: `tools/make_iso.py` rebuilds an ISO from an extracted dump; Aurora DVD serves it;
      `port::dol` verifies the DOL (SHA-1) and serves embedded data
- [x] `tools/tu_status.py` (decomp coverage), `tools/dolread.py` (DOL constants)

### Phase 1 progress
Ported and running: `main` -> system init (heaps, pad, DVD, ARAM, display) -> scene machine
(per-scene frame loop, wipes, reset handling) -> **Boot scene** (static data from the DOL,
Nintendo logo, async load of `/Game/game_usa.szp`) -> requests Title -> halts (Title not ported).

Next, in boot order: Title scene (and its J2D screens), the screen-wipe class
(`fn_80039xxx`), system font (`fn_8003EB44`), audio (JAudio; see `src/game/unported.h`),
memory-card manager, then the main game scene.

## Phase 1: Finish the decompilation (bulk of the work)
Order follows the boot path, so something is visible on screen as early as possible:
1. Runtime/MSL bits not covered by Aurora; replace them with the host libc where possible.
2. **JSystem**: JKernel (heaps, archives, RARC/Yaz0), JUtility, J2D, J3D (models, materials,
   TEV), JParticle, JAudio (or a host replacement, as Dusklight does). This is shared with
   other decomps (TP, SMS) and can borrow their *open-licensed* implementations where the
   library versions line up.
3. Boot → title screen → file select.
4. Mansion: map/room loading, collision, Luigi, Poltergust, camera.
5. Enemies/ghosts (`Ajioka`, `Koga`, `Sato`), portrait ghosts, bosses, events/cutscenes.
6. Save data (CARD via Aurora).

Port hygiene, applied as each unit comes in: 64-bit-safe pointers (no `u32` pointer storage),
big-endian asset reads through byte-swapping accessors, no reliance on fixed MEM1 addresses.

**Verification:** run Dolphin with the original DOL and the port side by side, use input
recordings, and diff per-frame game-state dumps (actor positions, RNG, flags).

## Phase 2: PC features
Status for the recompiled build: [ENHANCEMENTS.md](ENHANCEMENTS.md). Resolution and
widescreen (Hor+, 4:3 or edge-anchored HUD, widened camera culling), field of view and
frame interpolation (60 fps / display rate, game logic at 30 Hz) work.

- **High frame rate.** Keep the simulation at a fixed 30 Hz tick. Record matrices, camera,
  particles and UI each tick, and draw intermediate frames by interpolating between the last
  two ticks. Uses the same design as Dusklight's `src/dusk/interp` (CC0, usable as reference).
- **Arbitrary resolution.** Aurora renders at native output resolution. EFB copies and
  render-to-texture passes (the flashlight cone, the vacuum, ghost transparency, the Boolossus
  freeze, etc.) need to scale with it rather than assume 640×480.
- **Widescreen / ultrawide.** Hor+ projection fix in the camera. Anchor 2D/HUD elements to
  screen edges. Widen frustum culling and room visibility, since LM culls aggressively per
  room.
- **Faithful effects.** Indirect-texture distortion, TEV fog, per-room lighting, the flashlight
  and darkness model, ghost alpha, and the mirror/reflection rooms. Validate each one against
  Dolphin at native res.

## Phase 3: PAL Hidden Mansion on the US base
Diff GLMP01 against GLME01 using the two `symbols.txt` maps plus the disassembly. Known PAL
Hidden Mansion differences to confirm and catalogue: mirrored mansion, higher ghost HP and
damage, changed money/gem behavior and rankings, and Boo/boss changes. Implement them as a
selectable mode alongside the US Hidden Mansion. Each change is a behavioral port, not a
binary patch. Assets come from the user's own PAL dump only if a difference turns out to be
data-driven; otherwise from the US dump.

## Out of scope / rules
- No Nintendo assets or retail code in the repo, ever. Users supply their own dump.
- Nothing written here is submitted to lm-decomp (they prohibit AI-assisted contributions).
