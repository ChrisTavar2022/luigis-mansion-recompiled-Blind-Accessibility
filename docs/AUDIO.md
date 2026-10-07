# Audio

Luigi's Mansion uses JAudio (the `jaudio/` translation units in
`decomp/config/GLME01/splits.txt`). JAudio runs sequences, sound effects and
streams on the CPU and leaves the actual sample playback to a microcode on the
audio DSP. We do not emulate the DSP. Instead, `src/recomp/audio_dsp.cpp`
stands in for the microcode. It reads the per-voice parameter blocks that
JAudio writes into main memory and mixes the voices on the host. The finished
frames take the game's normal route to the audio interface (AI). From there,
`src/recomp/audio_out.cpp` plays them through SDL3.

Files:

| file | role |
|---|---|
| `src/recomp/hle_audio.cpp` | AI SDK HLE (DMA, callback pacing), DSP SDK stubs, JAudio DSP-driver hooks |
| `src/recomp/audio_dsp.cpp` | microcode stand-in: commands, voices, decoding, resampling, mixing, effect lines |
| `src/recomp/audio_out.cpp` | SDL3 output stream, WAV capture |
| `src/recomp/audio.h` | the interface between the three files above (and ARAM read access from `hle_ar.cpp`) |

## Frame protocol (what JAudio expects of the DSP)

Hooks in `tools/recomp/hooks.txt`:

- `DspBoot__Fv`, `DSPWaitFinish__Fv`, `DiplSec__FUl`: no-ops.
- `DSPSendCommands__FPUlUl(cmds, n)`: decodes the mailbox packet:
  - `0x81000000 | count, voices, interp table, AFC table, fx`: setup, sent by
    `DsetupTable`. In LM this is 64 voice blocks at `0x80451820`, the 4-tap
    resampling table at `0x80390C20`, the AFC coefficients at `0x80390BE0`, and
    4 effect lines at `0x80457820`.
  - `0x82000000 | subframes << 16 | mixer level, bufL, bufR`: sync frame, sent by
    `DsyncFrame`. LM uses 7 subframes of 80 samples (560 samples per frame).
    The mixer level is 4.12 fixed point (`DsetMixerLevel(f)` stores `f*4096`).
    LM uses 0x2000.
  - `0x80000000`: wait frame, ignored.
- `DSPReleaseHalt__Fv`: on hardware, the DSP halts after each subframe until the
  CPU has updated the voice blocks. Our hook mixes the next 80-sample subframe
  into `bufL`/`bufR`. It then posts the DSP interrupt by calling `DspSync__Fv`
  in interrupt context. That sends message 1 to JAudio's audio thread, which
  runs `UpdateDSP` (channel updates, player callbacks, then `DSPReleaseHalt`
  again). After the last subframe, `DspFrameEnd` queues the next frame.

JAudio fills a ring of three DSP buffers ahead of the AI. Every AI DMA interrupt
(`AudioSync` → message 0 → `Jac_UpdateDAC`) runs the following steps:

1. It hands the previously interleaved block to `AIInitDMA`.
2. It interleaves the next DSP buffer with `Jac_imixcopy`. The first DSP buffer
   goes in the first slot of each stereo frame.
3. It applies any CPU-side mix callback (`Jac_RegisterMixcallback`, used at boot).

## AI output and pacing

`AIInitDMA` records the block (560 stereo frames). A host thread waits until
fewer than about three blocks (at least 40 ms) are queued on the SDL3 audio
stream. It then pushes the current block, converting from big-endian, and
raises the AI DMA interrupt. The sound card's clock therefore paces JAudio, and
nothing drifts. If there is no audio device (or `LM_AUDIO=0`), the interrupt
runs off a timer at the real rate. The DSP rate comes from `AISetDSPSampleRate`
(LM: 32 kHz). SDL resamples to the device rate.

## Voice parameter block (0x180 bytes, big-endian)

Layout worked out from `jaudio/dspinterface.c` (`DSP_*`), `dspdriver.c`, and
the game's stream and movie players:

| offset | field | writer |
|---|---|---|
| 0x000 u16 | enabled (`DSP_PlayStart` 1, `DSP_PlayStop` 0) | CPU |
| 0x002 u16 | done: a one-shot ended or a stop finished; JAudio then runs the channel callback | DSP |
| 0x004 u16 | pitch, 0x1000 = 1 source sample per output sample (max 0x7FFF) | CPU |
| 0x008 u16 | restart request (set by PlayStart, cleared by the DSP) | both |
| 0x00C u16 | pause | CPU |
| 0x010 + 8i | 6 mixers: u16 bus, s16 target volume, s16 current volume, u16 ramp info | CPU / DSP (current) |
| 0x060 u16 | position fraction | DSP |
| 0x064 u16 | samples per block (16 for AFC, 1 for PCM) | CPU |
| 0x068 u32 | play position | DSP |
| 0x06C | samples left before the end or ring wrap (high halfword) | DSP |
| 0x074 u32 | ring voices: total samples left (A/V and stream sync) | CPU sets, DSP counts down |
| 0x100 u16 | source type: 0x05 AFC 2-bit, 0x09 AFC 4-bit, 0x08 PCM8, 0x10 PCM16 (ARAM); 0x21 PCM16 ring in main RAM; 0–3 oscillators | CPU |
| 0x102 u16 | loop flag | CPU |
| 0x104/0x106 s16 | AFC history at the loop start | CPU |
| 0x108 u16 | filter mode (0x20 = IIR, low bits = FIR length) | CPU |
| 0x10A u16 | stop request (`ForceStopDSPchannel`): fade out, then set done | CPU |
| 0x10C u32 | CPU-side frame counter | CPU |
| 0x110 u32 | loop start sample (ring voices: ring start address) | CPU |
| 0x114 u32 | end sample (ring voices: ring length << 16) | CPU |
| 0x118 u32 | ARAM byte address of the sample (ring voices: start address) | CPU |
| 0x11C u32 | sample count | CPU |
| 0x120 / 0x148 / 0x150 | FIR8, IIR4, distortion parameters | CPU |

The wave format byte in JAudio's `Wave_` maps to the block via two tables at
`0x804D7920` and `0x804D7928`: format 0 → AFC 9-byte, 1 → AFC 5-byte,
2 → PCM8, 3 → PCM16.

Bus IDs are DSP-memory addresses taken from JAudio's table at `0x80390BC0`.
Voices use 0x0D00 (left), 0x0D60 (right), and 0x0DC0 / 0x0E20 (effect sends).

## Mixing

- **AFC decoding.** Each 9-byte frame holds 16 four-bit samples (5-byte frames
  hold 2-bit samples). The header's high nibble is the scale exponent and the
  low nibble picks a coefficient pair from the table JAudio passes at setup.
  Each sample is `(n * 2^scale << 11 + c1*h1 + c2*h2) >> 11`, clamped. 2-bit
  samples use `<< 13`. When a voice loops, decoding jumps to the loop start and
  restores the history from 0x104/0x106.
- **Resampling.** A 4-tap polyphase filter uses JAudio's own 64-phase table and
  a 16.16 phase step of `pitch << 4`.
- **Volumes.** Volumes are 1.15. The current volume ramps linearly to the target
  over each subframe. A stop request ramps to 0 and then sets done.
- **Output.** `(left/right bus * level) >> 12`, saturated to s16.
- **Ring voices (type 0x21).** These are PCM16 rings in main RAM that the CPU
  refills. LM's streamed music uses them: the game's AFC streamer
  (`fn_80194A80`, a JAudio DSP-player callback) decodes `AudioRes/Stream/*.afc`
  into two rings, one per channel. The movie player uses them too. The CPU reads
  back 0x6C (where the DSP is in the ring) and 0x74 (samples left).
  `fn_80189054`, "is the streamed BGM ready", now completes on its own, so the
  temporary hook that forced it true has been removed.
- **Effect lines (approximation).** These are delay lines configured by
  `DFX_SetFxLine`. LM sets up four lines. Lines 0 and 1 output to left and right
  at volume 0x2000, with 32-subframe delays and a single FIR tap of 0x3FFF.
  Lines 2 and 3 output to buses that never reach the output. We feed line n
  from send bus n (0x0DC0 / 0x0E20). The delayed signal goes to the line's
  output buses and back into the delay through the FIR, which gives a decaying
  echo. The real topology is a guess. `LM_AUDIO_FX=0` turns the lines off.

## DVD audio streaming

JAudio's `StreamMain` (jaudio/streamctrl.c) drives DTK streaming through
`DVDPrepareStreamAsync` / `AISetStream*`, but the disc has no DTK tracks and LM
never uses it. Those SDK calls stay stubs. The game's music is sequenced (BMS
via JAudio) plus AFC streams through DSP ring voices, as described above.

## Verification

Use `LM_AUDIO_WAV=build/x.wav` to record the output and `LM_AUDIO_LOG=1` to log
setup, each voice start, and a dump of playing voices every ~2 s.

Checks on GLME01, using
`lmrecomp --input "600:START*10,900:A"` and an RMS/peak analysis of the WAV per
second:

- **Boot jingle (CPU mix callback).** Plays.
- **Title screen.** Thunder (loud, a few clipped samples) is followed by the
  sequenced title music. Its RMS is about 1000–1200, with peaks around 3000.
- **Attract movie.** Its audio plays (two ring voices at 44.1 kHz,
  pitch 0x160C).
- **After START.** The opening stream starts. The cutscene and foyer play
  sequenced music and effects. The game no longer needs the BGM-ready hook.
- **AFC decoder.** Run offline on `TMansion.afc`, it gives sane levels
  (RMS ~8300, no overflow).

## Known gaps

- Per-voice FIR/IIR/distortion filters are not applied. The IIR configs seen so
  far are pass-through.
- The effect-line topology is approximate (see above). Lines 2 and 3 are
  ignored.
- Oscillator sources (types 0–3) are silent. None have been seen in use so far.
- The channel order in the AI frame (first slot = bus 0x0D00 = left) is
  consistent with the stream data (AFC channel 0 → 0x0D00) but has not been
  checked against hardware.
- In one gameplay run, the left stream channel went silent for about 5 s and
  the problem did not reproduce. A ring voice may end early when the game
  restarts it; watch for it.
- Thunder on the title screen clips slightly at the 4.12 master level. The DSP
  saturates the same way, but hardware headroom has not been compared.
