# macOS port

The port builds and runs natively on macOS (Apple silicon): the same runtime, recompiler and
setup as on Windows and Linux, Aurora on Dawn's Metal backend, SDL3 for the window, input,
audio and file dialogs. The game module that setup compiles from the user's disc is a Mach-O
`.dylib` built with clang + ld64.lld. macOS shares the POSIX code paths of the Linux port
(docs/LINUX.md); the differences are `#ifdef __APPLE__` and the `macos-clang` preset.

Status (2026-10-24, M3 Pro, macOS 27): `LuigisMansionRecomp` builds, setup recompiles the USA
disc into `module.dylib`, the game boots to the title screen. Not yet checked: gameplay beyond
the title screen, the PAL disc, the `lmrecomp` dev build, packaging.

## Building

Xcode (Apple clang builds the program) and Homebrew:

```
brew install cmake ninja llvm lld sdl3
cmake --preset macos-clang          # build/macos-clang
cmake --build build/macos-clang --target LuigisMansionRecomp
```

Aurora fetches Dawn (`dawn-darwin-arm64`) and nod (`macos-arm64`) as prebuilt static
libraries; SDL3 is the system (Homebrew) one. Apple's clang has no lld, so the game module
is built with Homebrew's `clang` (llvm) and `ld64.lld` (lld): CMake finds them
(`LM_DEV_CLANG`, `LM_DEV_LD64_LLD`) and passes both folders to the program
(`LM_DEV_LLVM_DIR`, `LM_DEV_LLD_DIR`); `findToolchain` takes clang and ld64.lld from
different folders on macOS. `LM_LLVM_DIR` overrides at run time. The `clang` first on
`PATH` may be another toolchain (llvm-mingw): the preset names `/usr/bin/clang` explicitly.

## Runtime differences from Linux

| Area | Linux | macOS |
|------|-------|-------|
| Game module | `module.so`, `ld.lld -shared` | `module.dylib`, `ld64.lld -dylib -arch arm64 -platform_version macos 13.0 13.0 -undefined error -dead_strip` (ad-hoc signed by lld, required on Apple silicon) |
| Settings / install.ini, modules, cards | `~/.config`, `~/.local/share` | `~/Library/Application Support/Luigi's Mansion Recompiled` (both) |
| GPU caches | `~/.cache` | `~/Library/Caches/Luigi's Mansion Recompiled` |
| Own path | `/proc/self/exe` | `_NSGetExecutablePath` (`port::selfExePath`) |
| Crash reports | pc from `gregs[REG_RIP]` | pc/sp from `uc_mcontext->__ss` |
| Thread dumps (`[hang]`, `LM_HANG_DUMP`) | `/proc/self/task` + `tgkill` | `task_threads` + `pthread_kill(SIGUSR2)` |
| Thread policy (`[cpu]`) | none | guest threads QoS user-interactive (performance cores); Aurora's threads keep SDL's pthread priority (macOS allows no QoS class after one: logged as such) |
| Updater | libcurl.so, `-linux-x86_64.tar.gz` | `/usr/lib/libcurl.4.dylib`, `-macos-arm64.tar.gz` (no macOS release exists yet) |
| `LM_OFFSCREEN=1` | window moved off every display | window made transparent and click-through (macOS clamps positions onto a display) |

The XDG variables still override the macOS folders, so test runs use scratch folders the same
way as on Linux: `XDG_CONFIG_HOME`, `XDG_DATA_HOME`, `XDG_CACHE_HOME`, `LM_CARD_DIR`.

### Floating point on arm64

arm64 compilers fuse `a*b+c` into one `fmadd` by default (`-ffp-contract=on`); x86-64 builds
without `-mfma` never do. The PowerPC rounds a separate `fmul`/`fadd` twice, and the recompiler
emits the real `fmadd` instructions as `fma_` explicitly, so the game module and the runtime
(HLE PSMTX/PSVEC math, lmrecomp's generated code) are built with `-ffp-contract=off` on macOS.
Float-to-int conversions (`fctiw`, quantized stores) clamp explicitly, so arm64's saturating
converts change nothing.

### Thread stacks

Every guest call is a host frame. `std::thread` gets 512 KiB on macOS (Windows: 1 MiB, Linux:
8 MiB), so the threads that run guest code (guest threads, boot thread, interrupt timer) are
started by `sched::startHostThread` with 8 MiB stacks on POSIX.

## Open

- Setup's module build took 352 s on 12 cores (Windows: about 20 s). Not profiled yet.
- Shader preparation on Metal: about 180 s at a start where most pipelines came from the Dawn
  cache (`[precache]` lines: roughly 60 per second loaded), against 0.4-0.6 s warm on Windows.
- Packaging: no `.app` bundle, no bundled clang/ld64.lld (Homebrew's link `libLLVM`/
  `libclang-cpp` dynamically; an official LLVM release or copying those libraries would be
  needed), no release script, not notarized.
- Intel Macs: the code has the x86_64 branches but was not built.
