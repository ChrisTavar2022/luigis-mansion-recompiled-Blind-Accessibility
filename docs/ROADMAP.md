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

## Phase 0: Foundation *(in progress)*
- [x] Repo, vendored decomp (CC0), Aurora submodule, `.gitignore` that excludes game data
- [x] CMake skeleton + Aurora window bring-up (`src/port/main.cpp`)
- [ ] Toolchain on this machine: Visual Studio 2026 (C++ workload, clang-cl), CMake, Ninja
- [ ] First successful build and window
- [ ] Asset loading from the user's extracted `files/` dir or ISO via Aurora DVD
- [ ] `tools/`: script that lists which TUs are done / partial / missing from `splits.txt`

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
