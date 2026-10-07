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
   - With both discs: build the European Hidden Mansion overlay for the USA game
     (`setup/hm_overlay.cpp`, the C++ version of `tools/hidden_mansion_data.py`; files are
     read straight from the discs).
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
| `hidden_mansion/` | European Hidden Mansion overlay + `overlay.key` |
| `USA/Card A`, ... | memory cards (unchanged) |

A module's key is the SHA-1 of: the module ABI version, the game ID and DOL SHA-1, the
compile and link flags, and the embedded inputs (symbol map, splits, PAL version map,
hooks.txt, ctx.h, module_abi.h, module_rt.cpp, recompiler.cpp). Any change to those in a
new release rebuilds the module on first launch. The overlay key covers both DOL hashes
and the overlay format version.

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
- **Hidden Mansion.** The European game has its own; the USA-game overlay and hooks are
  not used (`hidden_mansion.cpp` only reports the flag at the European address).

### Version-aware hooks

`tools/recomp/hooks.txt` lines are USA symbols, optionally followed by versions:
`<symbol>` (USA only, the default), `<symbol> us pal` (both), `<symbol> pal`. A hook
tagged `pal` must have been checked against the European code (struct layouts differ in
places, e.g. the camera); `gen_version_map.py` then maps it to its European address and
fails if it cannot. Currently the JAudio DSP hooks apply to both versions.

## Feature gating (F1 menu)

`port::Avail` (`src/port/install.h`):

| Feature | USA base | European base |
|---------|----------|---------------|
| Skippable cutscenes, HUD at screen edges, GBH camera, early ghost shading | yes | *USA version only* (greyed on a European-only install) |
| Widescreen culling hook | yes | not applied (may pop in at the edges in widescreen) |
| Hidden Mansion: European rules | with the European disc installed | native |
| Hidden Mansion: USA rules | native | not yet (see below) |
| PAL refresh rate, language | *European version only* (greyed on a USA-only install) | yes |

Rules: a single-disc install greys out what needs the other disc, with the reason
("needs the European disc: F1 > Game > Add or change discs"). With both discs nothing is
greyed out for lack of a disc: options of the other version stay selectable and say
"applies when playing the USA/European version"; switch with F1 > Game > *Version to
play* (restarts the game). The only exception is the USA Hidden Mansion on the European
base, which is shown with an explicit "not on the European game yet" note.

Runtime safety: code written against USA addresses (`skip.cpp`, the HUD anchoring in
`widescreen.cpp`, `hidden_mansion*.cpp`) checks `port::usCodeFeature()` and stays inert on
the European game; hooks without a `pal` tag are not compiled into the European module.

## Not done yet / design notes

- **USA Hidden Mansion on the European base** (the reverse of the current overlay). Data:
  serve the USA `/Map/map2.szp` for `map2u.szp` loads and the USA `eventNN.txt` scripts
  in the European event archives (the European archives also carry other languages, so
  only the script member is swapped, like the forward overlay). Code: wrap each European
  function in docs/HIDDEN_MANSION.md's tables with a hook that, while the variant is USA,
  forces the USA branch (e.g. hide the flag from the mirror/stick/HUD functions, restore
  the 400 heart threshold, the room ambient, Chauncey/Boolossus/Bowser behaviour). The
  hook list needs per-function checking against European code, so it is not in this pass.
- European ports of the USA-only enhancements: tag their hooks `pal` after checking the
  European code, and give `skip.cpp` / `unused_content.cpp` / `widescreen.cpp` their
  European addresses (`tools/pal_diff.py var us <addr>`).
- Linux/macOS: the module build is Windows-only (`CreateProcess`, `.dll`); the module
  boundary itself is portable (`dlopen` is wired up).
