# Luigi's Mansion PC Port — working notes

## Goals
- Native PC port of Luigi's Mansion (GameCube), in the style of Dusklight (TP port), on the Aurora engine (`extern/aurora`, WebGPU/Dawn + SDL3).
- Base: USA disc **GLME01 rev 0**. Bring in the **PAL (GLMP01) Hidden Mansion** features, including its boss mechanics (Chauncey patterns, Boolossus vacuum-riding, harder Bowser), not just stat/data changes (docs/HIDDEN_MANSION.md). It is an *additional* runtime-selectable variant: E. Gadd's lab menu will offer both "Hidden Mansion (USA)" and "Hidden Mansion (PAL)" once unlocked (menu UI later).
- Enhancements: high frame rate via interpolation (logic stays 30 Hz), arbitrary resolution, widescreen + ultrawide, faithful advanced effects (EFB copies, fog, lighting, indirect).
- Every user-facing option lives in the F1 overlay menu (Esc closes): tabs Visuals / Sound / Enhancements (gameplay toggles, cheats, debug viewers). Add new options to `src/port/settings.h` (persisted to %APPDATA%\Luigi's Mansion PC\settings.ini) and `src/recomp/menu.cpp`; CLI flags only override for testing.

## Hard rules
- Never commit game data (ISO/DOL/extracted files/saves) or `build/`. Dumps live in `Luigis Mansion US/`, `Lugis Mansion PAL/`, `orig/` (gitignored).
- Architecture is a **build-time recompiler**: never hand-translate game functions into the repo; only original tooling/runtime/HLE/hook code. Generated C++ stays in `build/**/recomp_gen`.
- Never submit anything to lm-decomp (bans AI contributions). Don't copy GPL code (e.g. Dolphin).
- Commits: `git -c user.name=rmelv -c user.email=rmelville7@gmail.com commit`, message ends with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Architecture (see docs/RECOMPILER.md)
- `tools/recomp/recomp.py`: DOL + `decomp/config/GLME01/{symbols,splits}.txt` → one `void f_XXXXXXXX(Ctx&)` per function (gotos, jump-table switches, interior/address-taken entry points). Runs at CMake configure time (target `lmrecomp`, cache var `LM_DOL`).
- SDK translation units (os/gx/vi/pad/dvd/ar/card/ai/dsp/...) are **HLE**'d by name: `HLE(Name) { ... }` in `src/recomp/hle_*.cpp`. Args r3–r10/f1–f8, extra ints at `stackArg(c,n)`, structs by pointer, results in r3 (r3:r4 for 64-bit).
- Any game function can be replaced: add its symbol to `tools/recomp/hooks.txt` + `HLE(symbol)`.
- `src/recomp/ctx.h`: guest memory (`hostp`, `rd32/wr32`, BE), MMIO (0xCC008000 WGP → Aurora FIFO), Ctx/paired singles.
- `src/recomp/sched.*`: one host thread per OSThread, only one runs guest code (priority scheduler); interrupts via `postIrq`/`postIrqAfter`, delivered at loop back-edges/HLE calls or by the idle poster.
- `frame.*`: guest runs only inside an Aurora frame; handoff at `VISetNextFrameBuffer`. VI retrace timer 59.94 Hz; AI DMA interrupt paced by the SDL3 audio device drives JAudio (docs/AUDIO.md); DVD reads have modelled latency (game depends on it).
- Aurora local patches: `patches/aurora/*.patch` (auto-applied at configure). 0002 = vertex arrays with size 0 sized from draw indices.
- `src/game/`, `lmport` target: older hand-port prototype + dev viewers (reference only).

## Build / run
- `powershell -ExecutionPolicy Bypass -File tools/build.ps1 [-Configure] -Target lmrecomp`
- Run: `build/windows-clang/lmrecomp.exe [--disc orig/GLME01.iso] [--input "600:START*10,900:A"]`
- Screenshot: `powershell -File tools/capture_window.ps1 -Exe lmrecomp -Seconds 15 -Out build/x.png -ExeArgs '...'`

## Debugging
- `LM_TRACE=ring` (last 64 calls dumped on fatal) | `LM_TRACE=8000E46C,...` (log entries) | `LM_TRACE=all`
- `LM_WATCH=<addr>[,<len>]` report guest stores (use with LM_TRACE for function names)
- `LM_WATCHDOG=1` dump guest threads every 3 s; `LM_TRACE_MQ=1` message-queue log; `LM_DVD_SPEED=1` console-speed disc.
- `python tools/recomp/addr2sym.py <addr>...`; `tools/recomp/hle_refs.py` lists SDK functions game code calls.

## Gotchas
- Don't write C/C++ with `\n` escapes through bash heredocs/python-in-bash: escapes get mangled. Use the Write/Edit tools.
- Write Python files with `encoding="utf-8"`.
- Timing matters: instantaneous HW completion can break game assumptions (BootScene heap race).

## Status (keep updated)
- Boots → title → main menu → opening cutscene → in-game foyer with HUD (`--input "600:START*10,1000:A*6"`, foyer ~70 s).
- Memory card emulated (GCI folders in %APPDATA%\Luigi's Mansion PC\USA\Card A).
- Audio plays (DSP microcode stand-in in audio_dsp.cpp, SDL3 out in audio_out.cpp; docs/AUDIO.md; LM_AUDIO_WAV=, LM_AUDIO_LOG=1, LM_AUDIO=0, LM_AUDIO_FX=0). Reverb routing is a guess.
- Parallel work streams: widescreen/interp, PAL Hidden Mansion (agent worktree branches).
