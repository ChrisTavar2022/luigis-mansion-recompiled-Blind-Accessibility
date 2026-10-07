# Porting conventions

How retail code gets into `src/game/`. Read this before porting a function.

## Sources of truth
- **Disassembly** of the retail US DOL (GLME01 rev 0) is the reference. Generate it with
  lm-decomp's tooling (`python configure.py && ninja`) against your own disc; it lands in
  `build/GLME01/asm/`. Never commit it.
- **lm-decomp** (`decomp/`, CC0) has matched C++ for ~18% of the game. Port from it when a TU
  exists, but check it against the disassembly: the decomp sometimes misreads
  64-bit compares, enum values, or splits that the matching compiler hid.
- `decomp/config/GLME01/symbols.txt` names every function and object. Keep the retail
  name or address in a comment next to each ported function (`// fn_8000B610`).
- `tools/dolread.py` reads typed constants (floats, colors, strings) from the DOL by address.

## Rules
1. **No Nintendo data in the repo.** Constant tables that cannot be expressed as code (sine
   tables, embedded archives, fonts) are read at runtime from the user's DOL via
   `port::dol` (see `gx_util.cpp: loadTrigTables`, `boot_scene.cpp: staticdata`).
2. **Allocate through JKR heaps.** On console every `new` came from the current JKR heap and
   scenes free memory by heap group. Use `JKR_NEW` / `JKR_DELETE` (or `JKRAllocFromHeap`) for
   all game objects, never plain `new`/`delete`. Mixing them corrupts the CRT heap.
3. **Byte order.** File data and DOL data are big-endian. Read multi-byte fields with the
   `be16`/`be32` helpers (`helpers/endian.h`) or dedicated accessors (e.g.
   `DvdFileBlock::magicHost`). Do not compare raw magics like `'Yay0'` in memory.
4. **Pointers are 64-bit.** Never store pointers in `u32`. Struct offsets in comments are the
   retail ones; host layouts differ.
5. **Unported callees** go in `src/game/unported.{h,cpp}`, named `fn_<addr>_<what>`. Each
   logs once and returns a "nothing to do / finished" value. Porting a function means
   implementing it properly and deleting its ledger entry.
6. **Threads.** LM's own threads (DVD reader, loader) are OS threads. Only the main game
   thread hands frames to the host (`VISetNextFrameBuffer`). The JKR current heap is a
   single global shared across threads, as on console.
7. **PORT comments** mark every intentional behavior difference from retail (heap sizes,
   skipped cache flushes, crash-screen setup, etc.).

## Status
`python tools/tu_status.py` summarizes how much retail code is left. The live picture
of what the ported boot path still calls is the `[unported]` lines printed at runtime.
