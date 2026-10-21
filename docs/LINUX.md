# Linux port

The port builds and runs natively on Linux x86_64: the same runtime, recompiler and setup as
on Windows, Aurora on Dawn's Vulkan backend, SDL3 for the window, input, audio and file
dialogs. The game module that setup compiles from the user's disc is a `.so` built with a
bundled clang + ld.lld. Everything is in `#ifdef`s, the `linux-clang` CMake preset and the
files under `packaging/linux/`; Windows builds are unchanged.

## Building

Tested on Ubuntu 24.04 (WSL2). Build dependencies (Debian/Ubuntu names):

```
clang lld llvm cmake ninja-build pkg-config git python3 zip patchelf
libvulkan-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxinerama-dev
libxss-dev libxfixes-dev libxtst-dev libxkbcommon-dev libwayland-dev wayland-protocols
libdecor-0-dev libegl-dev libgl-dev libgles-dev libasound2-dev libpulse-dev
libpipewire-0.3-dev libdbus-1-dev libudev-dev libibus-1.0-dev libdrm-dev libgbm-dev libusb-1.0-0-dev
```

CMake 3.25+ (Ubuntu 24.04: 3.28). Aurora fetches the rest at configure time: Dawn and nod as
prebuilt static libraries (`dawn-linux-x86_64`, `libnod-linux-x86_64`), SDL3 built from
source (static; it `dlopen`s X11/Wayland/PulseAudio/PipeWire/ALSA at run time, so the
headers above only decide which of those it supports).

```
cmake --preset linux-clang [-DLM_DOL=<path to your main.dol>]   # build/linux-clang
cmake --build build/linux-clang --target lmrecomp LuigisMansionRecomp
```

Build in the Linux filesystem (e.g. `~/lm` in WSL), not on `/mnt/c`: it is much faster.
`lmrecomp` (dev build with the game linked in) needs `LM_DOL` like on Windows; the disc is
found from `--disc <iso>` or `orig/GLME01.iso` above the working directory.

## Runtime differences from Windows

| Area | Windows | Linux |
|------|---------|-------|
| Game module | `module.dll`, `lld-link /dll /noentry` | `module.so`, `ld.lld -shared -nostdlib` (below) |
| Module loader | `LoadLibraryW` | `dlopen(RTLD_NOW \| RTLD_LOCAL)` |
| Tool processes | `CreateProcessW`, below-normal priority | `posix_spawn`, nice 10, output to the per-file log |
| Settings / install.ini | `%APPDATA%\Luigi's Mansion Recompiled` | `$XDG_CONFIG_HOME/Luigi's Mansion Recompiled` (`~/.config/...`) |
| Modules, overlays, memory cards, screenshots | `%APPDATA%\Luigi's Mansion Recompiled` | `$XDG_DATA_HOME/Luigi's Mansion Recompiled` (`~/.local/share/...`) |
| GPU caches (`pipeline_cache.db`, `dawn_cache.db`) | `%APPDATA%\Luigi's Mansion Recompiled` | `$XDG_CACHE_HOME/Luigi's Mansion Recompiled` (`~/.cache/...`) |
| Crash reports | SEH filter + vectored handler, dbghelp symbols | `SIGSEGV/SIGBUS/SIGILL/SIGFPE` handler on an alternate stack: signal, pc as `binary+offset` (`dladdr`), fault address, `backtrace_symbols_fd` |
| Graphics backend | D3D12 (auto) | Vulkan (auto); OpenGL ES fallback |
| File dialogs | SDL3 | SDL3 (xdg-desktop-portal, or zenity/kdialog) |
| Updater (docs/SETUP.md "Updates") | WinHTTP, BCrypt SHA-256, `System32\tar.exe`; running files renamed to `.old`, deleted at the next start; new process started | libcurl `dlopen`ed (`libcurl.so.4`, else `libcurl-gnutls.so.4`; none: the menu offers the download page), own SHA-256, `tar -xzf`; files replaced by rename, old ones deleted at once, executable bits set, `execv` of the installed program (same PID) |

`port::configDir()`, `port::dataDir()` and `port::cacheDir()` (`src/port/install.h`) are the
only places that decide where files go; on Windows all three are the one `%APPDATA%`
folder. Each moves a 1.0.x "Luigi's Mansion PC" folder to the new name on first use
(docs/SETUP.md "What lives where"). Test runs: point `XDG_CONFIG_HOME`, `XDG_DATA_HOME`, `XDG_CACHE_HOME` and
`LM_CARD_DIR` at empty folders (the Linux equivalent of a scratch `APPDATA`).

### The game module (.so)

Compile flags (`src/setup/module_builder.cpp`): `--target=x86_64-pc-linux-gnu -fPIC
-fvisibility=hidden`, plus the freestanding set used on Windows (`-ffreestanding -nostdinc
-fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-math-errno -fno-stack-protector
-funwind-tables`). Link: `ld.lld -shared -nostdlib --eh-frame-hdr -Bsymbolic -z defs -z
noexecstack -z now`.

- **Visibility.** Only `lm_module_init` is exported (`LM_MODULE_EXPORT` in
  `module_rt.cpp`); everything else is hidden and bound inside the module, so its `g_mem`,
  `memcpy`, `fma`, ... can never be interposed by the executable's or libc's symbols.
  The built module has one dynamic symbol and no undefined ones.
- **Unwinding.** Guest threads end by throwing `sched::ThreadExit` through module frames;
  on Linux the unwinder needs `.eh_frame` (from `-funwind-tables`) and the
  `.eh_frame_hdr` lookup table (`--eh-frame-hdr`, which the clang driver normally adds).
- **No stack probes / `__chkstk` / `_fltused`** (Windows-only in `module_rt.cpp`): Linux
  thread stacks are fully mapped.
- `-z defs` turns a helper the compiler calls but `module_rt.cpp` lacks into a link error
  instead of a `dlopen` failure.
- A full build takes about 60 s on 24 threads in WSL (while the setup screen compiles
  shaders on the CPU renderer); `[setup]` lines in the log name the toolchain and the time.

### Portable executable (glibc 2.34+)

The executable must run on distributions older than the build host. Aurora links SDL3,
Dawn and nod statically; on top of that the runtime targets (`lm_runtime_target` in
`CMakeLists.txt`):

- link libstdc++ statically (`-static-libstdc++`); libgcc_s stays shared (the static
  unwinder of a new GCC needs `_dl_find_object`, glibc 2.35);
- add `src/port/glibc_compat.c`, which defines the few functions a new glibc would
  otherwise be required for: the C23 `__isoc23_strtol`/`sscanf` family (2.38; the headers
  redirect to them, also inside the prebuilt Dawn), `strlcpy`/`strlcat`/`wcslcpy`/`wcslcat`
  (2.38, used by SDL), `arc4random*` (2.36, libstdc++'s `random_device`), `hypotf` (bound to its
  original version instead of the 2.35 one) and
  `pidfd_spawnp`/`pidfd_getpid` (2.39, weak references in Rust's std inside nod).

Result: `NEEDED` is only `libc`, `libm`, `libz`, `libgcc_s`, `ld-linux-x86-64`, and the
highest symbol version is `GLIBC_2.34` (`objdump -T`). Re-check after dependency updates;
`tools/package_linux.sh` prints the versions of every binary it packages.

### Bundled compiler

The package carries `llvm/bin/clang` and `llvm/bin/ld.lld` from the official LLVM release
(`LLVM-<ver>-Linux-X64.tar.xz` from github.com/llvm/llvm-project releases; 21.1.8 for the
first package). Those binaries link LLVM statically and need only libc (2.34), libm, libz,
libgcc_s and libstdc++ (GCC 12+, `GLIBCXX_3.4.30`). A distribution's own clang is not
suitable (it needs `libLLVM`/`libclang-cpp` and their dependencies).

`lld` also needs `libxml2.so.2`, used only by its COFF flavour to merge Windows manifests.
Distributions disagree on libxml2's soname (2.14 is `.so.16`) and the build host's copy
needs ICU and glibc 2.38, so the package ships `llvm/lib/libxml2.so.2` built from
`packaging/linux/libxml2_stub.c`: the 16 symbols lld imports, with libxml2's symbol
versions (`libxml2_stub.map`), aborting if ever called. lld finds it through its
`RUNPATH $ORIGIN/../lib`. The license is `llvm/LLVM-LICENSE.TXT` (Apache-2.0 WITH
LLVM-exception), like on Windows.

The module needs no clang resource headers (`-nostdinc`, builtins only).
Toolchain lookup is the same as on Windows: `<exe dir>/llvm/bin`, `LM_LLVM_DIR`, the LLVM
that built the executable (`LM_DEV_LLVM_DIR`, dev trees), `PATH`.

## Packaging

```
tools/package_linux.sh <LLVM bin dir> [out dir]
```

configures with `LM_LLVM_DIR=<LLVM bin dir>`, builds, runs `cmake --install --strip` into
`<out>/LuigisMansionRecompiled-linux-x86_64/`, prints the glibc/libstdc++ requirements, and makes a
`.tar.gz` (keeps the executable bits) and a `.zip` of it. Layout:

```
LuigisMansionRecompiled.sh  launcher: log to ~/.local/share/Luigi's Mansion Recompiled/last_run.log
                         (previous_run.log), chmod +x in case unzip dropped the bits,
                         LD_LIBRARY_PATH=lib/ if a lib/ folder is ever added
LuigisMansionRecomp      the program (stripped; symbolize crash offsets with the
                         unstripped build/linux-clang/LuigisMansionRecomp of the same commit)
pipelines.bin            shader precache list
README.txt               requirements, how to run, where files go, reporting problems
                         (packaging/linux/README.txt)
NOTICE.md
llvm/bin/clang, ld.lld   bundled compiler; llvm/lib/libxml2.so.2 (stub); llvm/LLVM-LICENSE.TXT
```

No game data, nothing derived from it, and no build output other than these.

Releases: `tools\release_windows.bat` (Windows) builds the Windows zip, then runs
`tools/release_linux.sh <version> out` in WSL (clone in `~/lm-release` from the committed HEAD,
missing apt packages installed, an `LLVM-*-Linux-X64` under `~` or `/opt` found or 20.1.8
downloaded, `package_linux.sh`, versioned copies in `out/`), and publishes GitHub release
`v<version>` with every package and `packaging/release_notes/v<version>.md`.
Size: about 450 MB unpacked, 150 MB compressed (clang and lld are 250 MB and 190 MB).

## Testing

- **WSL2 + WSLg** (what was used): Ubuntu 24.04 with WSLg; display through XWayland or
  Wayland, sound through WSLg's PulseAudio server. WSLg has no hardware Vulkan driver for
  this GPU (Mesa's D3D12-backed `dzn` is not in Ubuntu's Mesa), so Vulkan runs on Mesa's
  CPU renderer: `LM_BACKEND=vulkan LM_ALLOW_CPU_GPU=1` (Aurora ignores CPU adapters
  otherwise). Without them Aurora picks OpenGL ES on llvmpipe; `GALLIUM_DRIVER=d3d12` puts
  GL on the GPU but was slower than llvmpipe here.
- **Older distribution:** a `debootstrap` Ubuntu 22.04 chroot (glibc 2.35, libstdc++ 12) with
  desktop runtime libraries (`libvulkan1 mesa-vulkan-drivers libx11-6 ... libpulse0`), WSLg's
  X11 and PulseAudio sockets bind-mounted, running the packaged tarball as a normal user.
- Checked (October 2026): fresh setup with `--us-disc <iso> --setup-auto` in scratch XDG
  dirs (module built with the bundled clang/ld.lld in about 60 s and loaded), title ->
  file select -> opening -> foyer with HUD through `--input "600:START*10,1000:A*6"`
  (frame dumps), audio device opened on PulseAudio, pipeline precache list found next to
  the executable, F1 menu rendering (`LM_MENU=Visuals`), setup screen with mouse input,
  the file-dialog fallback, clean exit on SIGINT/SIGTERM (SDL quit event), crash handler
  output (a shutdown use-after-free of the SDL audio stream found this way is fixed:
  `audio::outShutdown` before `aurora_shutdown`). The "no GPU" message box is untested.
- Not verifiable in WSLg: keyboard input. WSLg gives an X11 window keyboard focus only when
  its Windows-side window is focused, so `xdotool` key/text injection does not arrive;
  mouse injection does.
- Frame dumps (`LM_FRAME_DUMP`) work as on Windows; window screenshots in WSLg:
  `SDL_VIDEO_DRIVER=x11` and ImageMagick `import -window <id>`.

## Known issues / not done

- Real GPUs: only tested on Mesa's CPU Vulkan driver (WSLg). NVIDIA/AMD/Intel drivers on a
  real Linux install are untested; performance on llvmpipe is not representative.
- Wayland vs X11: SDL3 prefers Wayland when available; `SDL_VIDEO_DRIVER=x11` forces X11.
- Window activation / focus-loss pause, gamepad hot-plug, rumble and the PAL disc path are
  untested on Linux. File-picker dialogs need xdg-desktop-portal or zenity (SDL3); when
  neither exists, setup shows SDL's error and the Linux-only path box next to "Browse..."
  (type/paste a path, quotes and `file://` stripped) works instead; `--us-disc` /
  `--pal-disc` too.
- No usable GPU: Aurora falls back to its null device and stops at the first allocation;
  `main.cpp` remembers "Failed to create adapter" errors and shows a message box naming
  the missing Vulkan driver before aborting (skipped with `LM_BACKGROUND`).
- No AppImage/Flatpak; the tarball is self-contained apart from glibc, libstdc++ (bundled
  compiler only), libz, libgcc_s and the GPU/audio/display libraries of the desktop.
- `lmport` (the old prototype) is not built on Linux.
