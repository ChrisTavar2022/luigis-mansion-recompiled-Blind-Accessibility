# First-run setup and the game module

End users run one program, `LuigisMansionRecomp.exe`. It contains the runtime (memory,
scheduler, HLE SDK, Aurora, menu, audio), the recompiler and a setup screen. The game
itself is a DLL that setup builds on the user's machine from the user's own disc, with a
bundled clang + lld. Nothing derived from the game is shipped or committed.

## Release layout

`cmake --install build/<preset> --prefix <dir>` produces:

```
LuigisMansionRecomp.exe   runtime + recompiler + setup UI
*.dll                     SDL3, Dawn (+ dxcompiler/dxil), nod, libpng, zlib
llvm/bin/clang.exe        bundled compiler (from LM_LLVM_DIR, default: the LLVM that built the exe)
llvm/bin/lld-link.exe     bundled linker
llvm/LLVM-LICENSE.TXT     Apache-2.0 WITH LLVM-exception (packaging/LLVM-LICENSE.TXT)
NOTICE.md
```

The LLVM binaries are copied at install time, never committed. Toolchain lookup order:
`<exe dir>/llvm/bin`, `LM_LLVM_DIR`, the LLVM that built the exe (dev builds,
`LM_DEV_LLVM_DIR`), then `PATH`.

## Setup flow

1. **Pick discs.** The setup screen (Dear ImGui on the Aurora window; SDL3
   `SDL_ShowOpenFileDialog` for browsing) has a slot for the USA disc (GLME01) and one for
   the European disc (GLMP01). Any image nod reads works (ISO/GCM, RVZ, WIA, GCZ, CISO,
   WBFS, NFS, TGC). Each pick is validated at once (`setup/disc.cpp`): the image is
   opened with nod and the SHA-1 of its `main.dol` must match a supported retail build
   (`port::dol::identify`, hashes from `decomp/config/*/build.sha1`). A disc in the wrong
   slot, another game or another revision gets a message.
   One disc is enough; with both, the user picks the version to play (default USA).
2. **Install** (worker thread, progress bar, cancellable):
   - For each disc whose game module is missing or stale: recompile its `main.dol`
     (`src/recompiler`, in-process), write the 32 generated sources plus the shipped
     freestanding sources (`ctx.h`, `module_abi.h`, `module_rt.cpp`) to a scratch folder,
     compile them in parallel (one clang per core, largest first), link with lld-link,
     then delete the generated sources (set `LM_KEEP_BUILD=1` to keep them).
     A full build takes about 15-25 s on an 8+ core machine.
   - With both discs: build the Hidden Mansion overlays (`setup/hm_overlay.cpp`, the C++
     version of `tools/hidden_mansion_data.py`; files are read straight from the discs):
     the European rules' data for the USA game and the USA rules' data for the European
     game (`usa/`).
3. **Launch** the chosen version: open its disc in Aurora's DVD layer, check the DOL
   again, load the module, boot.

Later launches skip the screen when the configured discs are readable and every module
and overlay is current; they are rebuilt automatically when stale. `--setup` (or F1 >
Game > *Add or change discs...*) opens the screen again.

### What lives where (`%APPDATA%\Luigi's Mansion PC`)

| Path | Contents |
|------|----------|
| `install.ini` | disc paths and the version to play |
| `settings.ini` | F1 menu settings |
| `game/GLME01/module.dll`, `game/GLMP01/module.dll` | game modules |
| `game/<id>/module.key` | cache key (below) |
| `game/<id>/recompiler.log` | recompiler summary |
| `hidden_mansion/` | European Hidden Mansion overlay for the USA game, `usa/`: USA Hidden Mansion overlay for the European game; `overlay.key` |
| `USA/Card A`, ... | memory cards (unchanged) |

A module's key is the SHA-1 of: the module ABI version, the game ID and DOL SHA-1, the
compile and link flags, and the embedded inputs (symbol map, splits, PAL version map,
hooks.txt, ctx.h, module_abi.h, module_rt.cpp, recompiler.cpp). Any change to those in a
new release rebuilds the module on first launch. The overlay key covers both DOL hashes
and the overlay format version (3: lab menu choice in event08).

Command-line helpers (testing): `--us-disc <path>`, `--pal-disc <path>` prefill the slots,
`--setup-auto` starts installing without a click, `--setup` forces the screen.
`LM_CACHE_DIR` separates the GPU caches of test runs; pointing `APPDATA` at an empty folder
gives a fresh install.

## Module boundary

```
LuigisMansionRecomp.exe                     module.dll (no imports, no CRT)
  runtime, HLE, scheduler, Aurora   <---->    f_XXXXXXXX (recompiled functions)
  RuntimeApi table (module_abi.h)  ---init-->  module_rt.cpp: globals, forwarders
  g_module (funcs/syms/orig tables) <--ret--   func_table.cpp: kGameModule
```

- The module exports one function, `lm_module_init(const RuntimeApi*)`, and returns its
  `GameModule` (function table, symbol table, hooked originals). The runtime calls game
  code only through those tables (`Ctx::call`, `guestSymbol`, `callOriginal`).
- `ctx.h` is **freestanding**: no system headers, only compiler builtins
  (`__builtin_memcpy`, `__builtin_bswap32`, `__builtin_fma`, `__builtin_bit_cast`, ...),
  so the bundled clang needs no SDK or C++ library. Paired-single quantization is inline.
- Hot paths stay inside the module: memory access inlines against a module-local copy of
  `g_mem` (set once at init; it never changes), the trace/watch flags are copied, the
  interrupt flag is read through a pointer to the runtime's flag (one load at loop
  back-edges), and indirect calls binary-search the module's own table. Everything else
  (MMIO, interrupts, HLE lookups, SPRs, tracing, fatal errors) goes through the
  `RuntimeApi` function pointers.
- The module is compiled with `-ffreestanding -nostdinc -fno-exceptions -fno-rtti
  -fno-threadsafe-statics -fno-math-errno -fno-stack-protector -mno-stack-arg-probe` and
  linked `/dll /noentry /nodefaultlib`; `module_rt.cpp` provides `memcpy`/`memset`/
  `memmove`/`memcmp`, `_fltused`, and `fma`/`trunc`/`nearbyint`/`sqrt` (forwarded to the
  runtime's CRT).
- Bump `kModuleAbiVersion` (ctx.h) when `Ctx`, `GameModule` or `RuntimeApi` change.

### Developer builds

- `lmrecomp` (unchanged workflow): `tools/build.ps1 -Target lmrecomp`. The build runs
  `lmrecompile` (the same recompiler) on `LM_DOL` and links the generated code statically
  (`LM_STATIC_MODULE`): full debug info, no setup. `-DLM_DOL_VERSION=GLMP01` with the
  European `main.dol` builds the European game.
- `LuigisMansionRecomp`: the shipped program; in a dev tree it finds the LLVM that built
  it, so it runs from `build/windows-clang` as is.
- The Python recompiler is gone; `src/recompiler/recompiler.cpp` is the only one
  (`tools/recomp/lmrecompile.cpp` is its command line).

## The European (PAL) game as the base

The European release is recompiled from its own `main.dol` with
`decomp/config/GLMP01_00/symbols.txt`. That config has no translation-unit splits and
names few functions, while the runtime finds SDK functions (HLE) and hooks by their USA
names. `tools/recomp/GLMP01_map.txt` (generated by `tools/recomp/gen_version_map.py`
from the user's dumps, committed like the decomp symbol maps, embedded in the exe)
supplies, per European function address, the canonical name and translation unit:

- SDK functions: matched to their USA counterparts with `tools/pal_diff.py`, then named by
  call-site agreement (matched callers whose `bl` sequences line up), plus a short list of
  hand-checked overrides for the SDK units the European release reorganised (PAD, VI
  callbacks, `OSGetLanguage`).
- Game functions the European map leaves unnamed but the USA map names (e.g. JAudio's
  `DspSync__Fv`, used by the audio HLE).
- Hooks tagged `pal` in `tools/recomp/hooks.txt`.

Differences the runtime handles for the European game:

- **Frame pacing.** The European `LMDisplayUtil` paces frames with an `OSAlarm` of N/60 s
  (the USA game counts N VI retraces) and re-submits the framebuffer from its post-retrace
  callback every retrace. The runtime HLEs `OSCreateAlarm`/`OSSetAlarm`/`OSCancelAlarm`
  (an alarm re-armed from its own expiry counts from the expiry, so host latency does not
  slow the game) and presents a frame at `GXCopyDisp` instead of `VISetNextFrameBuffer`.
- **50 / 60 Hz.** F1 > Visuals > *PAL refresh rate*: 50 Hz (default, the console's PAL
  video timing) or 60 Hz (PAL60). Because the European game times its frames with the
  alarm, the logic speed is the same either way (30 fps, like the USA game); the setting
  changes the VI retrace (`VIGetTvFormat`, retrace callbacks, `VIWaitForRetrace`).
- **Language.** The game asks `OSGetLanguage` when its save has no language yet; F1 >
  Game > *Language* answers it (English, German, French, Spanish, Italian). A save keeps
  its own language (changeable in the game's Options). The `.sdata` language variable
  (0x804BD658) is also initialised from the setting.
- **Boot values.** Disc ID, `.ctors` start, arena start (from `__init_registers`),
  `VI_PAL` TV format.
- **Hidden Mansion.** The European game has its own; with both discs the USA rules can be
  chosen instead (below, *USA Hidden Mansion on the European game*).

### Version-aware code

Host code (hooks, HLE, menu features) is written once, against the USA game, and runs on
both releases:

- **Hooks.** `tools/recomp/hooks.txt` lines are USA symbols, optionally followed by
  versions: `<symbol>` (USA only, the default), `<symbol> us pal` (both), `<symbol> pal`
  (European only). A function only the European build has is written `palfn_<PAL address>`.
  `gen_version_map.py` maps each `pal`-tagged hook to its European address (the matched
  function, given the USA name in `GLMP01_map.txt`) and fails if it cannot. Tag a hook
  `pal` only after checking the European function (`tools/pal_diff.py show <fn>`): a
  `layout` or `modified` pair can move struct fields the handler uses.
- **Functions the runtime calls by name** (`guestSymbol`, `GU_SYM`, `callOriginal`) get the
  same USA name in the European module: `gen_version_map.py` scans `src/recomp` and
  `src/port` for those names and writes a `<PAL address> <USA name> -` line for each.
  The recompiler renames an unrelated European function that happens to carry the same
  name (`pal_fn_...`), so names stay unique.
- **Addresses.** Wrap every USA data or code address in `VA(0x...)`
  (`src/recomp/version.h`). On the USA game it is the address itself; on the European game
  it is looked up in `src/recomp/version_addrs.inc`, which `gen_version_map.py` generates
  by scanning the sources for `VA(...)`: a data address is mapped through the matched
  functions that reference it (or another address of the same object, a literal with the
  same value in the matched function, or the pointer table that points at it); a code
  address (e.g. a return address checked in a hook, or a function's last instruction) is
  the same instruction in the matched European function. The script fails on an address it
  cannot map. Each row says how it was mapped.
- **Struct offsets** are not generated: `VOFF(usa, european)` after checking the field with
  `tools/pal_diff.py field` / `show` (e.g. the camera's viewport fields moved by +0xC,
  Luigi's fields from +0x134C on by +4).
- `ver::pal()` tells which game runs. Code that still uses raw USA addresses must check
  `port::usCodeFeature()` and stay inert on the European game; F1 greys such options with
  "applies when playing the USA version" (or "needs the USA disc").

Workflow after adding a hook or a `VA()`: `python tools/recomp/gen_version_map.py`
(needs both dumps; about 10 s), commit `GLMP01_map.txt` and `version_addrs.inc` (address
metadata only, like the decomp symbol maps). The module cache key covers the map and
hooks.txt, so users' European modules rebuild on the next launch.

**For new features (cheat-code ports, debug modes, unused content):** use `VA()` for every
address from the cheat/AR code and `VOFF()` for offsets, tag the hooks `us pal`, re-run the
script, and test on the European build (`-DLM_DOL_VERSION=GLMP01`, below). A feature that
cannot be mapped yet stays USA-only: gate its menu entry with `port::usCodeFeature()`.

### Enhancements on the European game

All enhancements now work on both releases. Checked in the European static build
(`lmrecomp` with `LM_DOL_VERSION=GLMP01`, scratch `APPDATA`/`LM_CARD_DIR`):

| Feature | European specifics | Verified |
|---------|-------------------|----------|
| Skippable cutscenes | scene table has a 9th (language select) scene; ids and event interpreter layout unchanged | opening + mansion entrance skipped; snapshot matches a watched run |
| HUD at the screen edges | the two HUD functions are larger (end address mapped by `VA()` of their last instruction) | 21:9 foyer: meter at the left edge |
| Widescreen culling (`fn_80008D58`) | camera viewport x/width at +0x1F8/+0x200 (USA +0x1EC/+0x1F4) | hook runs for every 3D projection |
| Game Boy Horror camera | the European view-1 clear is `fn_8000B7CC` (the matcher pairs the USA `fn_8000B228` with an unrelated full-screen clear); it resets depth with a Z texture, which Aurora now implements, so no hook is needed (the old far-plane workaround is gone) | live view on the GBH screen in the foyer |
| Ghost shading Beta | `fn_800581E8` identical; call site mapped | lighting visibly changes (Boolossus balcony); check in an ordinary room pending |
| Gameplay options | Luigi's riding byte +0x1350; the European Luigi update writes it every frame, so it is set in `fn_800B84E4` (state update) instead; darkness: the game's own hidden-mansion 0.2 is not applied twice | health x2 HUD 200, darkness, hearts, riding |
| Debug: info overlay, particle report, depth blur, warps, model viewer | `LMChangeFrameBuffer` is the European `fn_80007940` (alarm-paced variant) | overlays, warps to maps 5 and 13, viewer (Chauncey) |

### PAL parity status (paused)

Done and checked on the European static build: version mapping (`VA`/`VOFF`, generated
`version_addrs.inc`, USA names for European functions, `palfn_` hooks), skippable cutscenes
(snapshot identical to a watched run), HUD edge anchoring + culling hook at 21:9, GBH camera,
gameplay options (health x2, darkness, no hearts, always ride), debug overlays / warps / model
viewer, and the USA Hidden Mansion on the European game in the foyer (USA map data served, no
mirror, USA lighting; the European variant still mirrored and dark).

Pending: ghost shading Beta re-check in an ordinary room (the Boolossus check shows the
change); the reverse Hidden Mansion's boss rules (Chauncey, Bowser, Boolossus), hearts and
enemy timers are implemented but not yet exercised in play; `LuigisMansionRecomp` with both
discs (setup building `hidden_mansion/usa/` and both modules) not yet run end to end; a
warp queued very early (`LM_DEV_WARPS` during the opening) broke the European game once;
European runs sometimes print `crash: abort()` when the test harness stops them (seen before
this work too).

### USA Hidden Mansion on the European game

With both discs installed, E. Gadd's lab offers both Hidden Mansions on either base
(docs/HIDDEN_MANSION.md, *E. Gadd's lab menu*); F1 > Enhancements > *Hidden Mansion
version* is the fallback when the lab menu is not installed. On the European base, the USA
rules work like the forward direction in reverse:

- **Data** (built by setup next to the forward overlay, `hidden_mansion/usa/`; dev builds:
  `tools/hidden_mansion_data.py`): the USA `/Map/map2.szp`, served when the European
  `MissionMode::init` loads `/Map/map2u.szp`; the European `event08/16/73.szp` with only
  the script (`text/eventNN.txt`) swapped for the USA one (the message tables of every
  language stay).
- **Code.** The European game reads the hidden-mansion flag in 36 functions. Those whose
  only flag use is a PAL change are hooked (`pal` tag) and run with the flag reading 0
  for the duration (`hm::callWithoutFlag`): the screen mirror, stick and HUD mirroring, the
  iris wipe, the GBH map mirror, the enemy strategy timers, Chauncey's balls / rocking
  horses / approach, Bowser's volley flip, the damaging-floor pattern and the JAudio
  stereo swap. Functions where the USA game reads the flag too (damage x2 `fn_800B9680`,
  vacuum `fn_80081BCC`, the GBH screen `fn_8003DF88`, `fn_800DBE8C`) and the heart hooks
  run with the real value even when reached from inside a hidden call
  (`hm::callWithFlag`). The heart thresholds go back to the USA 400 (European 1000), the
  room-ambient factor (European `.sdata` 0x804BD728, 0.2) is set to 1.0, and Luigi's
  Boolossus movement mode is kept off in his state update.
- The scene-start / event-interpreter / new-game code (which sets or clears the flag) and
  the map loader are left alone.

Limitation: the hiding is a write to the flag word, so a guest thread switch inside a
hidden call (an interrupt waking a higher-priority thread) lets that thread see the USA
value too; only flag readers with USA hidden-mansion behaviour could be affected, and the
ones found are all on the main game thread.

## Feature gating (F1 menu)

`port::Avail` (`src/port/install.h`):

| Feature | USA base | European base |
|---------|----------|---------------|
| Skippable cutscenes, HUD at screen edges, widescreen culling, GBH camera, ghost shading, gameplay options, debug tools | yes | yes |
| Hidden Mansion: European rules | with both discs (overlay) | native |
| Hidden Mansion: USA rules | native | with both discs (reverse overlay) |
| PAL refresh rate, language | *European version only* (greyed on a USA-only install) | yes |

Rules: a single-disc install greys out what needs the other disc, with the reason
("needs the European disc: F1 > Game > Add or change discs"). With both discs nothing is
greyed out: options of the other version stay selectable and say "applies when playing
the USA/European version"; switch with F1 > Game > *Version to play* (restarts the game).

Runtime safety: hooks without a `pal` tag are not compiled into the European module, and
code with raw USA addresses checks `port::usCodeFeature()`.

## Not done yet / design notes

- Linux/macOS: the module build is Windows-only (`CreateProcess`, `.dll`); the module
  boundary itself is portable (`dlopen` is wired up).
