# Rendering accuracy

Hardware-accurate GX features on top of Aurora, and the F1 > Visuals > Rendering toggles
that control them (CLAUDE.md, "Rendering principle"). Each section: what the console does,
what Aurora did, what the port does now, cost, and test aids. Other agents add sections here.

## EFB copies to RAM

Toggle: **EFB copies to RAM (accurate)** (`[rendering] efb_copy_to_ram`, default on;
`LM_EFB_RAM=0/1` overrides). Code: Aurora patch `patches/aurora/0007-efb-copy-to-ram.patch`
(`lib/gfx/efb_ram.{hpp,cpp}`, C API `include/aurora/efb_ram.h`) and `src/recomp/efb_copy.cpp`.

### Console behaviour

`GXCopyTex` writes the EFB rectangle into main memory as a texture in the format given to
`GXSetTexCopyDst` (tiled: 4 bpp in 8x8 blocks, 8 bpp in 8x4, 16 bpp in 4x4, RGBA8/Z24X8 as
4x4 blocks of 64 bytes with the AR pairs followed by the GB pairs; big-endian texels). Anything
that later reads that memory, a texture of any format/size/offset, the CPU, or DMA, sees those
bytes; anything that writes it replaces them.

### What Aurora did

Copies stay GPU-only: the copy is resolved into a texture (at the upscaled render resolution)
keyed by the destination pointer, and any texture whose data pointer equals that key samples it,
whatever format and size it was created with. Memory is never written, and the association lives
until the pointer is copied to again, even after the game frees the buffer and loads something
else there.

### What Luigi's Mansion does with its copies (US, measured with `LM_COPY_LOG=1`)

| Destination | Copy | Read as | Code |
|---|---|---|---|
| shadow buffer (heap, 0x96000) | Z16 640x480, every frame | **IA8 640x480** texture (TEV `GR16` compare against a 256x256 IA8 depth ramp: Luigi's self-shadow) | `fn_8005D8CC` copy, `fn_8005D41C` texture, `fn_8005DD50` TEV setup |
| `lbl_804D86BC` (0x8021E000) | RGB565 640x480, twice per frame | RGB565 same size | `fn_8005F518` / `fn_8005F8B8` |
| `lbl_804D86B8` (0x802B4000) | RGBA8 320x240 (box-filtered) | RGBA8 same size | `fn_8005F498` |
| 0x803A4D00 | RGB565 70x100 from (250,140 140x200) | same | `fn_80025C98` |
| transition captures | RGB565 640x480 | same | `fn_800081A8` (LMDisplayUtil) |
| title/file select | 4 x RGB565 148x116 | same | `fn_800494E4` |

- The shadow buffer is the one copy read with a different format. It and the two screen
  buffers are also reused as scratch memory for ARAM/DVD loads (Game Boy Horror archive,
  `fn_80121884`; `fn_80066DB8`, `fn_8005336C`), i.e. written by DMA/CPU between copies.
- After the screen-buffer copies the game calls `GXPixModeSync` + `DCInvalidateRange` on them
  (cache hygiene before reusing the memory, no CPU reads of the pixels were found). No GXPeek
  calls exist.
- The 148x116 title buffers are freed after the menu and the memory is reused for other textures
  (e.g. Luigi's IA8 textures at +0x540/+0xD60) at different pointers.

### What the port does now

1. **Every copy** of a supported format (all of Aurora's copy conversions plus RGBA8/RGB5A3) is
   recorded as a memory range `[dest, dest + GXGetTexBufferSize)` with a hash of the memory
   there (`efb_ram::on_copy`, FIFO thread).
2. **Textures** (`resolve_sampled_textures`): a texture overlapping a recorded copy
   - exactly matching it (pointer, format, size) keeps Aurora's fast copy texture (upscaled:
     the enhancement on top of the accurate base);
   - otherwise (different format, size or offset, or spanning several copies) is **decoded from
     the copy's GX bytes**: a compute pass encodes the copy texture into the exact console bytes
     (native resolution, tiling, byte order: `EncodeShader`), and a fragment pass decodes them
     as the requested format (`DecodeShader`, same conventions as Aurora's CPU decoders). Both
     run before the render pass that samples the texture, in the same frame, with no CPU sync.
     Partial overlaps start from the memory under the texture and lay each copy's bytes over it
     in copy order. Palette (C4/C8/C14X2) and CMPR reads keep Aurora's existing paths.
   - Luigi's shadow map is the case that takes this path in LM: the Z16 copy is read as IA8.
3. **CPU/DMA writes**: when a texture is resolved over a recorded copy, the memory is re-hashed
   (once per frame per copy); if it changed, the copy is dropped and Aurora's pointer association
   evicted, so the texture comes from memory. (Fixes stale copies shown for reused heap memory
   with the same pointer.)
4. **CPU reads**: copies into ranges the game `DCInvalidateRange`s (how GameCube code makes
   GPU-written memory visible to the CPU) are encoded and read back into guest memory. The
   readback is asynchronous: the bytes are written at the next guest frame boundary after the GPU
   finishes the frame (1-2 frames after the copy), and only if the memory is unchanged since the
   copy (or still holds our previous writeback), so a CPU write after the copy is never clobbered.
   A same-frame synchronous readback would need a mid-frame GPU submit/stall, which Aurora's
   single-encoder frame model doesn't support; LM never reads copy pixels with the CPU, so this
   is not needed here. `LM_EFB_READBACK_ALL=1` reads back every copy (test aid).

Encoded copy semantics follow Aurora's copy conversions (`tex_copy_conv.cpp`): e.g. Z16 = depth
bits 23..16 in the IA8 intensity byte and 15..8 in the alpha byte, which is how LM's shadow TEV
(swap tables + `GR16` compare) expects it.

### Results

- Bit-exactness check: in the intro (copies on vs off, 2x internal resolution, 550 frames dumped
  at native resolution) the frames are identical, i.e. the GPU encode/decode of the shadow map
  gives the same image as Aurora's direct Z16 sampling there. (Feeding the decode deliberately
  shifted bytes also left the intro door scene unchanged: the self-shadow receiver doesn't affect
  those shots.) The mouth flicker itself is rare and random and was not reproduced; this feature
  removes the copy-related differences from hardware (native-resolution, byte-exact reinterpretation
  of the shadow map, stale-copy eviction). Re-check if the flicker is still reported.
- Regression run (title, file select, intro, foyer with HUD, Game Boy Horror map screen) renders
  correctly with the toggle on.

### Cost (foyer gameplay, 2x internal resolution, Original 30)

- CPU in efb_ram (hashing, resolves, writebacks): ~0.6 ms per guest frame (~5 MB hashed per
  frame: 5 copies + writeback checks), on the FIFO/host threads. Whole-process CPU over 25 s:
  33.6 / 33.2 % of a core on vs 30.4 / 33.1 % off (within noise).
- GPU: one compute encode + one fullscreen decode of the 640x480 shadow map per frame, plus
  encodes of the two DCInvalidateRange'd screen buffers (~1.5 MB/frame read back). Negligible next
  to the scene.

### Test aids

- `LM_COPY_LOG=1`: every GXCopyTex (frame, dest, rects, format, caller LR), textures created
  over copied memory (same/different format), and efb_ram counters every 300 frames.
- `LM_FRAME_DUMP=<dir>,<first>,<last>[,<step>]`: writes guest frames `<first>..<last>` as
  native-resolution `.ppm` files, captured through the copy-to-RAM path (works with the toggle
  off too). Frame numbers count GXCopyDisp calls; runs drift a few frames apart, so align by
  content when comparing.
- `LM_EFB_RAM=0/1`, `LM_EFB_READBACK_ALL=1`.
