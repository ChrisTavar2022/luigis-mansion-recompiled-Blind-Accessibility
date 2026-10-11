# Build-time recompiler (plan)

Our own tool translates the user's own `main.dol` (GLME01) into C++ at build time.
Generated code lives in `build/` and is never committed.

## Pieces
1. **Recompiler** (`src/recompiler/`, C++; `tools/recomp/lmrecompile` at build time,
   in-process at first-run setup, docs/SETUP.md): decode PPC from the DOL; function
   boundaries and jump tables from `decomp/config/GLME01/symbols.txt` (CC0 metadata).
   One C++ function per guest function: `void f_XXXXXXXX(Ctx&)`; in-function branches
   as labels/goto; `bl` = direct call; `bctrl`/`blrl` = lookup in a generated
   address->function table; `bctr` with a known jump table = `switch`.
2. **Runtime** (`src/recomp/`): guest MEM1 (addr & 0x01FFFFFF), big-endian load/store
   helpers, `Ctx` (r0-31, f0-31 + ps1, CR fields, LR, CTR, XER, FPSCR, GQR0-7),
   paired-single quantized loads/stores, DOL loading, .ctors, boot into `main`.
3. **HLE SDK**: every function in the Dolphin SDK TUs is not recompiled; a host
   implementation marshals guest pointers/structs to Aurora (GX, VI, PAD), our
   existing OS/thread/message/VI/frame-driver layers, DVD reads into guest memory,
   AR/ARQ, CARD. Audio (DSP/AI) is silent at first.
4. **Enhancements** as hooks on recompiled functions: frame interpolation,
   widescreen/resolution, PAL Hidden Mansion behaviour.

## Instruction coverage
212 distinct mnemonics are used by the game (integer, branch, load/store incl. update
and indexed forms, lmw/stmw, single/double FP, frsqrte, fctiwz, paired singles:
psq_l/psq_st, ps_add/sub/mul/madd/msub/nmadd/nmsub/muls0/1/madds0/1/sum0/merge*/
neg/res/rsqrte/cmpo0, plus supervisor ops only inside SDK code that is HLE'd).

## Status (2026-10-07)

The recompiled build (`lmrecomp`) runs the retail game: boot → title → main menu →
opening cutscene → gameplay in the mansion, at full speed (30 Hz logic, ~0.26 CPU
cores in the foyer). Memory card saves work (GCI folders, Dolphin-compatible).
Audio is silent pending a JAudio DSP replacement.

Playing: build with `tools/build.ps1 -Target lmrecomp` and run
`build/windows-clang/lmrecomp.exe` with `orig/GLME01.iso` present (or `--disc`).
A GameCube controller (adapter) or SDL gamepad works through Aurora's PAD.

Runtime lessons:
- Console timing assumptions matter: disc reads must take time (BootScene's heap
  switch races its async open callback), the AI DMA interrupt must tick for
  JAudio's frame work, and interrupts must never be stranded while the CPU idles.
- A guest thread must not hold the guest CPU through a long host wait: interrupts are
  only taken at its safe points, so everything else stalls with it. Waits that block on
  the console too give the CPU up (`sched::releaseForHost` / `reacquireAfterHost`,
  `withCpuReleased`): the lockstep frame handoff in `VISetNextFrameBuffer` (up to a
  display refresh), `GXDrawDone` (draining Aurora's FIFO), and the host side of disc
  reads. GX calls from other threads meanwhile wait for the guest's turn
  (`frame::gxWaitForGuestTurn`, in every GX HLE and the write-gather pipe). Before this,
  AI interrupts arrived in pairs after each frame handoff and JAudio's DSP overload check
  stopped a voice several times a second (cut sound effects, the Pikmin movie's stream;
  docs/AUDIO.md). `LM_IRQ_STALL=1` logs the running thread whenever an interrupt waited
  over 10 ms (with `LM_TRACE=ring`, its recent functions).
- Indirect calls reach code the symbol map merged into neighbours; the recompiler
  adds entry points for every address-taken code location and every branch target
  inside another function.
