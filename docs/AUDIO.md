# Audio

Luigi's Mansion uses JAudio (the `jaudio/` translation units in
`decomp/config/GLME01/splits.txt`). JAudio runs sequences, sound effects and
streams on the CPU and leaves the actual sample playback to a microcode on the
audio DSP. We do not emulate the DSP. Instead, `src/recomp/audio_dsp.cpp`
stands in for the microcode. It reads the per-voice parameter blocks that
JAudio writes into main memory and mixes the voices on the host. The finished
frames take the game's normal route to the audio interface (AI). From there,
`src/recomp/audio_out.cpp` plays them through SDL3.

The stand-in follows the game's own microcode: the `jdsp` image in the DOL
(`0x8038F340`, 0x17E0 bytes, identical on GLMP01), read with
`tools/dsp_disasm.py <main.dol>`. Microcode addresses below (`uc 0x0141`) refer
to that listing. The decomp's struct names for the parameter block come from
other emulators and are partly wrong; the microcode is the reference.

Files:

| file | role |
|---|---|
| `src/recomp/hle_audio.cpp` | AI SDK HLE (DMA, callback pacing), DSP SDK stubs, JAudio DSP-driver hooks |
| `src/recomp/audio_dsp.cpp` | microcode stand-in: commands, voices, decoding, resampling, filters, mixing, effects |
| `src/recomp/audio_out.cpp` | SDL3 output stream, channel order, WAV capture |
| `src/recomp/audio.h` | the interface between the three files above (and ARAM read access from `hle_ar.cpp`) |
| `tools/dsp_disasm.py` | DSP disassembler for reading the microcode |

## Frame protocol (what JAudio expects of the DSP)

Hooks in `tools/recomp/hooks.txt`:

- `DspBoot__Fv`, `DSPWaitFinish__Fv`, `DiplSec__FUl`: no-ops.
- `DSPSendCommands__FPUlUl(cmds, n)`: decodes the mailbox packet (uc 0x0027
  dispatches on the command byte):
  - `0x81000000 | count, voices, interp table, AFC table, fx` (uc 0x0349): setup,
    sent by `DsetupTable`. In LM this is 64 voice blocks at `0x80451820`
    (`CH_BUF`), the resampling/oscillator table `DSPRES_FILTER` at `0x80390C20`,
    the AFC coefficients at `0x80390BE0`, and 4 effect lines at `0x80457820`
    (`FX_BUF`).
  - `0x82000000 | subframes << 16 | mixer level, bufL, bufR` (uc 0x04EB): sync
    frame, sent by `DsyncFrame`. LM uses 7 subframes of 80 samples (560 samples
    per frame). The mixer level is 4.12 fixed point (`DsetMixerLevel(f)` stores
    `f*4096`); LM uses 0x2000. The effect-line table is read once per frame.
  - `0x80000000`: wait frame, ignored.
- `DSPReleaseHalt__Fv`: on hardware, the DSP waits for a mail at the start of
  each subframe (uc 0x0502) until the CPU has updated the voice blocks. Our hook
  mixes the next 80-sample subframe into `bufL`/`bufR`. It then posts the DSP
  interrupt by calling `DspSync__Fv` in interrupt context (the microcode raises it
  right after the voice loop, uc 0x064D). That sends message 1 to JAudio's audio
  thread, which runs `UpdateDSP` (channel updates, player callbacks, then
  `DSPReleaseHalt` again). After the last subframe, `DspFrameEnd` queues the next
  frame.

JAudio fills a ring of three DSP buffers ahead of the AI. Every AI DMA interrupt
(`AudioSync` → message 0 → `Jac_UpdateDAC`) hands the previous block to
`AIInitDMA`, interleaves the next DSP buffer with `Jac_imixcopy`, and applies any
CPU-side mix callback (`Jac_RegisterMixcallback`, used for the boot jingle).

## Channel order

`Jac_VframeWork` calls `Jac_imixcopy(buf + 560, buf, dac, 560)`: the **second**
DSP buffer (bus 0x0D60) goes into the first 16-bit slot of each AI frame and bus
0x0D00 into the second. The console plays the first slot on the **right** (AI
frames are R, L). So bus 0x0D00 is left, which everything else agrees with:

- JAudio's default bus routing (`driverinterface.c`, `busConnect = 0x150, 0x210,
  ...`) feeds bus 0x0D00 with `sin(1 - pan)` and 0x0D60 with `sin(pan)`; with
  JAudio's pan 0 = left, 0x0D00 is the left speaker. Effect line 0 (fed by the
  `1 - pan` send) returns to 0x0D00, line 1 to 0x0D60.
- AFC stream and movie channel 0 go to 0x0D00.
- The Dolby Surround matrix puts the surround bus on 0x0D00 in phase and on
  0x0D60 inverted (the usual Lt/Rt convention).

`audio_out.cpp` therefore swaps the slots (`kRightFirst`). Before this was
worked out the port played every sound mirrored. "Swap left/right" (F1 > Sound)
and the PAL Hidden Mansion's mirror mode still swap on top.

## AI output and pacing

`AIInitDMA` records the block (560 stereo frames). A host thread waits until
fewer than about three blocks (at least 40 ms) are queued on the SDL3 audio
stream. It then pushes the current block, converting from big-endian, and
raises the AI DMA interrupt. The sound card's clock therefore paces JAudio, and
nothing drifts. If there is no audio device (or `LM_AUDIO=0`), the interrupt
runs off a timer at the real rate. The DSP rate comes from `AISetDSPSampleRate`
(LM: 32 kHz). SDL3 resamples it to the device rate with its band-limited
resampler.

## Voice parameter block (0x180 bytes, big-endian)

The DSP DMAs the whole block in for every voice every subframe (uc 0x0393) and
writes the first 0x100 bytes back (uc 0x039C), so fields above 0x100 are
read-only for the DSP.

| offset | field | writer |
|---|---|---|
| 0x000 u16 | enabled (`DSP_PlayStart` 1, `DSP_PlayStop` 0) | CPU |
| 0x002 u16 | done: the source ran out or a stop finished; the DSP skips the voice from then on, JAudio runs the channel callback | DSP |
| 0x004 u16 | pitch, 4.12 (0x1000 = 1 source sample per output sample, max 0x7FFF) | CPU |
| 0x008 u16 | reset (set by PlayStart; the decoder restarts; cleared unless paused) | both |
| 0x00C u16 | pause: play the constant sample at +0x66 instead of the source | CPU |
| 0x00E u16 | samples of history kept for the per-mixer delays (`DSP_SetMixerInitDelayMax`; LM: 16) | CPU |
| 0x010 + 8i | 6 mixers: u16 bus, s16 target volume, s16 current volume, u16 delay (high byte target, low byte current, in samples) | CPU / DSP |
| 0x060 u16 | resampler fraction (12 bits) | DSP |
| 0x066 s16 | last resampled sample (the paused voice's output) | DSP |
| 0x068 u32 | decode position in samples (ring voices: high half = position in the ring) | DSP |
| 0x06C | ring voices: samples before the ring wraps (high half) | DSP |
| 0x074 u32 | ARAM voices: frames left; ring voices: samples left (CPU sets, DSP counts down) | both |
| 0x078 s16[4] | resampler history | DSP |
| 0x080 s16[20] | FIR history | DSP |
| 0x0A8 s16[4] | IIR history | DSP |
| 0x0B0..0x0CF | AFC decoder state (partly decoded frame, its history) | DSP |
| 0x0D0..0x0EE | per-mixer delay history (not cleared by PlayStart) | DSP |
| 0x0F0 / 0x0F2 | distortion state (y, x) | DSP |
| 0x100 u16 | source type (see below) | CPU |
| 0x102 u16 | loop flag | CPU |
| 0x104/0x106 s16 | AFC history at the loop start | CPU |
| 0x108 u16 | filter mode: 0x20 = IIR on, low 5 bits = FIR taps | CPU |
| 0x10A u16 | stop request (`ForceStopDSPchannel`) | CPU |
| 0x110 u32 | loop start sample (ring voices: ring start address) | CPU |
| 0x114 u32 | end sample (ring voices: ring length << 16) | CPU |
| 0x118 u32 | ARAM address of the sample (ring voices: segment start address) | CPU |
| 0x11C u32 | sample count | CPU |
| 0x120 s16[24] | FIR coefficients (1.15) | CPU |
| 0x148 s16[4] | IIR coefficients (1.15) | CPU |
| 0x150 s16 | distortion gain (`DSP_SetDistFilter`, 0 = off) | CPU |

Source types the microcode handles (uc 0x052F): 0x09 AFC 9-byte frames, 0x08
PCM8 from ARAM, 0x20 PCM16 in main RAM at the output rate, 0x21 PCM16 ring in
main RAM (resampled). Everything else goes to an oscillator jump table (uc
0x0266); types 0x05 (2-bit AFC) and 0x10 (PCM16 in ARAM) land on an empty entry
there. LM only uses 0x09 and 0x21; the port still decodes 0x05/0x10 in case
another title needs them.

## Buses

Bus IDs are DSP data-memory addresses (JAudio's connect table at `0x80390BC0`).
Each holds one subframe (80 samples). Per subframe (uc 0x04FA..0x0697):

1. Bus 0x0F40 (surround) from the previous subframe starts the outputs: 0x0D00 =
   S, 0x0D60 = NOT S (= -S - 1). Buses 0x0CA0, 0x0F40, 0x0FA0, 0x0B00, 0x09A0 are
   cleared (uc 0x03D9).
2. Effect lines, input side (below).
3. Voices mix into their buses.
4. Bus 0x0B00 goes through the modulated delay and is added at 0.5 to R and L.
5. Bus 0x09A0 (center) is added at 0.707 (0x5A82) to L and R.
6. L gets the frame's mixer level (4.12, saturated) and is written to `bufL`.
7. Bus 0x0FA0 is subtracted from R; R gets the level and goes to `bufR`.
8. Effect lines, output side.

Bus 0x0CA0 feeds oscillator modulation and is never output.

## Voice pipeline

Per voice per subframe, if enabled and not done (uc 0x0509..0x064C):

- **Decoding.** `(pitch * 80 + frac) >> 12` new source samples are produced.
  AFC: each 9-byte frame holds 16 four-bit samples; the header's high nibble is
  the scale exponent and the low nibble picks a coefficient pair from the table
  JAudio passes at setup; each sample is `(n * 2^scale << 11 + c1*h1 + c2*h2) >>
  11`, saturated. When a voice loops, decoding jumps to the loop start and
  restores the history from 0x104/0x106. A one-shot that runs out is padded with
  zeros and flagged done in the same subframe. PCM8 samples are `s8 << 8`.
- **Ring voices (0x20/0x21, uc 0x08E8).** PCM16 in main RAM that the CPU refills:
  the game's AFC streamer (`fn_80194A80`) decodes `AudioRes/Stream/*.afc` into
  two rings, one per channel, and the movie player does the same. +0x74 counts
  the samples left; when it runs out, the voice holds its last sample and is
  flagged done. The read position (+0x68 high half) is relative to +0x118 in a
  ring of (+0x114 >> 16) samples that wraps to +0x110. The streamer reads back
  +0x6C (samples before the wrap) and +0x74.
- **Resampling (uc 0x0141).** A 4-tap polyphase filter over 4 samples of history
  plus the new samples, using JAudio's own 64-phase table (phase = top 6 bits of
  the 16-bit fraction), multiplier x2, rounded, saturated. From a ratio of 4.0
  (pitch 0x4000) up it takes the nearest sample instead. A restarted voice starts
  from a zero history, so its first sample comes out 3 samples late, as on
  hardware.
- **Pause.** The resampler output is replaced by the constant sample (+0x66).
- **Distortion (uc 0x0B90)** if +0x150 is nonzero: `y = sat(y' + (x - x') * g/128)`.
- **FIR (uc 0x01E1)** if the filter mode has taps: `y[n] = sum c[k] *
  x[n - taps + k]`, multiplier x2, rounded. The newest tap is x[n-1].
- **IIR (uc 0x01FC)** if mode bit 0x20 is set and the coefficients are not
  `{0x7FFF, 0, 0, 0}`: `y[n] = c0*x[n-1] + c1*x[n-2] + c2*y[n-1] + c3*y[n-2]`,
  multiplier x2, truncated, saturated.
- **Stop request (uc 0x05E0).** Every subframe the current volumes halve; the
  voice is done once they are all 0. (The microcode then ramps back toward the
  target within the subframe, which would keep a stopped voice audible; JAudio
  stops updating targets once a stop is requested, so the port keeps the halved
  volume to give the intended fade.)
- **Mixers (uc 0x05F6, 0x011E).** For each of the 6 mixers with a bus:
  - The delay's low byte moves one sample per subframe toward its high byte, and
    the mixer reads that many samples back (JAudio's pan delay; history is the
    last +0x0E samples of the previous subframes).
  - Volume ramp: a 16.16 step of `(target - current) / 32`, applied every second
    sample, so the ramp ends after 64 samples; the current volume becomes the
    target.
  - Each sample adds `(volume * sample) >> 16` to the bus, saturated. The
    multiplier is **not** doubled here, so a full-volume voice adds half its
    amplitude; the 4.12 mixer level of 2.0 restores it at the output.

## Effect lines (uc 0x0446, 0x047D)

JAudio's `DFX_SetFxLine` fills four 0x20-byte entries: `+0 enabled, +2 size (in
subframes), +4 ring buffer in main RAM (size * 160 bytes), +8 bus A, +A volume
A, +C bus B, +E volume B, +10 s16 FIR[8]`. Line n uses bus `0x0DC0 + 0x60 * n`
(80 samples plus 8 of history), so the voice sends 0x0DC0 / 0x0E20 / 0x0E80 /
0x0EE0 feed lines 0-3. Per subframe, for each enabled line:

1. The bus's last 8 samples move to its front; the ring slot of this subframe
   (written `size` subframes ago) is fetched behind them.
2. Enabled bit 0: the 8-tap FIR runs over the bus in place (`y[n] = sum c[k] *
   b[n + k]`, x2, rounded).
3. The result is mixed into bus A and bus B at their 1.15 volumes (x2, rounded).
4. Enabled bit 1: the FIR runs again.
5. Voices add their sends on top.
6. After the voices, the bus is written back to the ring slot, and the slot
   index advances (modulo size).

This makes a feedback delay: the stored signal is the filtered delayed signal
plus the new sends. LM's configuration:

| line | input | size | outputs | FIR |
|---|---|---|---|---|
| 0 | 0x0DC0 | 32 (80 ms) | L x 0.25 | tap 6 = 0.5 |
| 1 | 0x0E20 | 32 | R x 0.25 | tap 6 = 0.5 |
| 2 | 0x0E80 | 24 (60 ms) | center x 0.5, surround x 0.5 | tap 6 = 0.62 |
| 3 | 0x0EE0 | 40 (100 ms) | center x 0.5, surround x 0.5 | tap 6 = 0.5 |

The ring buffers are the game's own (in main RAM), as on hardware.

The modulated delay on bus 0x0B00 (uc 0x0B11, 0x0B24): the bus is written into a
160-sample ring (two halves), and read back with a nearest-sample pointer at
1 - 1/128 or 1 + 1/128 speed, switching when it comes within 16 / 48 samples of
the write side. The result goes at 0.5 into L and R. The ring wrap (the microcode
uses a DSP circular address register) is modelled as modulo 160.

F1 > Sound > "Reverb / echo" turns off the effect lines and the modulated delay
(`LM_AUDIO_FX=0` does too). "Voice filters" turns off distortion/FIR/IIR. Both
are on by default. The whole mixer costs about 0.2% of one core
(`LM_AUDIO_LOG=1` prints it); the filters are skipped when the game does not
ask for them.

## Surround

The game keeps its own sound mode (a byte in its options: 0 mono, 1 stereo, 3
surround; forced to mono when `OSGetSoundMode` says mono) and hands it to its
sound code in `fn_8005582C` → `fn_8018A70C(0/1/2)`; what mode 2 changes in the
routing has not been traced. Even in stereo, some voices (JAudio's "dolby"
parameter) and effect lines 2/3 feed the surround bus 0x0F40, which the DSP
folds into the stereo output in Dolby Surround form (+L, -R; center at 0.707).
The port reproduces that matrix exactly, so a Pro Logic (II) decoder can unfold
it. The port's own "Output" setting does no decoding: "Surround" there plays like
"Stereo" (its tooltip says so).

## DVD audio streaming

JAudio's `StreamMain` (jaudio/streamctrl.c) drives DTK streaming through
`DVDPrepareStreamAsync` / `AISetStream*`, but the disc has no DTK tracks and LM
never uses it. Those SDK calls stay stubs. The game's music is sequenced (BMS
via JAudio) plus AFC streams through DSP ring voices, as described above.

## Verification

Use `LM_AUDIO_WAV=build/x.wav` to record the output and `LM_AUDIO_LOG=1` to log
setup, effect-line configuration, each voice start (with its filter, delay and
mixer settings), voices that start using filters or delays later, a dump of
playing voices every ~2 s, and the mixer's CPU cost.

Second pass (2026-10), comparing captures from the old and new mixer (scratch
APPDATA/card, `LM_BACKGROUND=1`): title screen with no input (60 s), foyer
(`--input "600:START*10,1000:A*6,..."`, 100 s), and the Nursery with the
Poltergust (`LM_TEST_STATE=poltergust LM_START_MAP=2 LM_START_APPEAR=1
LM_SKIP_CUTSCENES=1 LM_SKIP_AUTO=30`, A every 300 retraces, then R holds,
175 s):

- **Levels.** Everything mixed by the DSP is 6 dB quieter than before (the old
  mixer doubled the voice products). Title thunder no longer clips (old: 173
  clipped samples in 60 s, peak 32768; new: none, peak 28652). Foyer: 240 clipped
  samples before, none after. The CPU-mixed boot jingle is unchanged.
- **Channel order.** Sounds the game pans left now come out on the left (for
  example a voice with mixer volumes L 0x37B1 / R 0x0E56 in the Nursery).
- **Effects.** Effect lines 2 and 3 (center + surround) and the surround bus
  were missing; with them the foyer music is no longer mono (L/R correlation
  0.92-0.95 instead of 1.000).
- **Filters.** LM sets IIR mode with the pass-through coefficients on almost
  every voice; no scene tested used FIR, IIR, distortion or pan delays. Forcing
  a low-pass configuration on every voice (a temporary test hack) gave sane,
  stable output (spectral centroid 2400 → 900 Hz).

## Known gaps

- Oscillator sources (types 0-4, 6, 7, 0xB-0xD) are silent; LM does not use
  them.
- Saturation vs wrap-around: the microcode runs some passes (effect-line
  mixing, FIR) in 16-bit accumulator mode, where an overflow would wrap; the port
  saturates everywhere.
- A force-stopped voice fades by halving (see the voice pipeline) instead of the
  microcode's halve-then-ramp-back.
- Non-looping AFC samples whose length is not a multiple of 16 lose their last
  partial frame on hardware; the port plays it.
- The AI's "32 kHz" is nominal; the exact console rate (slightly above 32 kHz)
  is not modelled. Pacing follows the host device anyway.
