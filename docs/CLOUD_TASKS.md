# Tasks for cloud sessions

Cloud sessions run on Linux without a GPU, a display, or the game's disc images (game data
must never be uploaded or committed). They can't build-and-play the game, so they take
code-only work. Read CLAUDE.md first (rules, architecture, commit attribution). Work on a
branch named `cloud/<task>` and open a pull request against `main`; the local session merges
and tests it on Windows with the real discs.

## 1. Linux (and macOS) game-module build in first-run setup

`docs/SETUP.md` describes the setup flow: `LuigisMansionRecomp.exe` recompiles the user's DOL
with the C++ recompiler (`src/recompiler/`) and compiles the output into a game module with a
bundled clang + lld. The module boundary is `src/recomp/module_abi.h` (no imports; everything
via `RuntimeApi`; `lm_module_init` export) and `src/recomp/module_rt.cpp` (freestanding
support code). Today the module build is Windows-only (PE DLL via lld-link /entry, LoadLibrary).

Goal: make the module build and load work on Linux (ELF shared object via ld.lld, dlopen) and
design/implement macOS (Mach-O dylib via ld64.lld, dlopen), keeping the module freestanding:
- abstract the compile/link command lines and the loader (`module_loader.cpp`, setup code)
  per platform; correct relocation model (-fPIC), visibility of `lm_module_init`, no libc;
- packaging: which LLVM binaries to bundle per platform (`cmake --install` rules, licence);
- CMake: the runtime itself must also configure/build on Linux (Aurora supports Linux;
  check SDL3/Dawn/nod availability, `dbghelp`-style Windows-only bits in crash_handler, etc.).
Verification without discs: unit-test the compile+link+load path with a tiny synthetic
"recompiled" source set (a few fake `f_XXXXXXXX` functions and tables) that exercises the
real ABI; make sure the Windows path is unchanged (CI-style check that it still compiles).

## 2. Unit tests for the recompiler

Add a small test harness (CMake `ctest`) for `src/recompiler/`: PPC instruction decoding and
emitted C++ for a corpus of hand-written instruction words (integer, rotate/mask, compare/CR,
branches incl. bc/bclr/bcctr, loads/stores incl. update forms, FP single/double, paired
singles and psq_l/psq_st quantization, mtspr/mfspr), plus the "patch" runtime-switch lines
and hooks.txt parsing. Tests must not need a DOL: feed synthetic words directly.
