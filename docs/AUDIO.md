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
| `src/recomp/hle_audio.cpp` | AI SDK HLE (DMA, callback pacing), DSP SDK stubs, JAudio DSP-driver hooks, the game's sound mode |
| `src/recomp/audio_dsp.cpp` | microcode stand-in: commands, voices, decoding, resampling, filters, mixing, effects |
| `src/recomp/audio_out.cpp` | output layouts (stereo / mono / discrete 5.1 / 7.1), windowed-sinc resampler, SDL3 stream, WAV capture |
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
worked out the port played every sound mirrored.

**In-game check (2026-10, third pass).** In the foyer the camera stops at the
room's side walls, so Luigi can stand at the far left or far right of the
screen (`--input "...,1950:STICKLEFT*200,2200:A,2300:A,2400:A,2500:A,
2600:STICKRIGHT*400,3050:A,..."`, frame dumps to confirm the screen position).
Pressing A makes him call out, a positional sound. With Luigi at the left edge
the call's voices (ARAM 0x0B8F60, 0x0E37A0) start with bus 0x0D00 / 0x0D60
target volumes 11753 / 5311 and 8184 / 3638; at the right edge the same samples
start with 5166 / 11818 and 3638 / 8184: the game pans screen-left to bus
0x0D00 by about 7 dB, mirror-symmetrically. In the recorded output
(`LM_AUDIO_WAV_RAW`, 50 ms bins above RMS 1000) the left section has 50 bins with L > R + 3 dB and none the
other way (loudest bins +5.6 dB), the right section 10 bins with R > L + 3 dB
and none the other way. So the slot order above puts screen-left sounds on the
left speaker.

## AI output and pacing

`AIInitDMA` records the block (560 stereo frames). A host thread waits until
fewer than about three blocks (at least 40 ms) are queued on the SDL3 audio
stream. It then pushes the current block and raises the AI DMA interrupt. The
sound card's clock therefore paces JAudio, and nothing drifts. If there is no
audio device (or `LM_AUDIO=0`), the interrupt runs off a timer at the console
rate.

**Output rate.** The AI's rates are derived from a 54 MHz clock: "48 kHz" is
54 MHz / 1124 = 48042.7 Hz and "32 kHz" (what `AISetDSPSampleRate` selects in
LM) two thirds of it, 32028.5 Hz. The port treats the blocks as 32028.5 Hz
audio (`kConsoleRate32` in `audio.h`). Since the AI interrupt paces JAudio, this
sets the game's audio clock: sequenced music runs at the console's tempo and
everything plays at the console's pitch (0.09%, 1.5 cents above the nominal
32000 Hz the port used before). The timer path without a device uses the same
rate.

**Resampling.** F1 > Sound > "High-quality resampler" (on by default) converts
the 32028.5 Hz stream to the device's rate with a polyphase windowed-sinc filter
in `audio_out.cpp`: 64 taps (32 zero crossings each side), Kaiser window
(beta 8), cutoff 0.92 of the lower Nyquist frequency, 512 phases with linear
interpolation, each phase normalised to unity DC gain. For 32028 -> 48000 Hz it
is flat (-0.1 dB) to 13.8 kHz and -80 dB from 16.0 kHz on, so the spectral
images of the 32 kHz signal between 16 and 24 kHz are removed. Off, the stream
goes to SDL at 32028 Hz and SDL resamples.

**Layouts.** The block is turned into the output layout first (see Surround):
stereo, mono (L = R = average), or discrete 5.1 / 7.1. "Swap left/right"
(F1 > Sound) and the PAL Hidden Mansion's mirror mode swap the left and right
channels of every pair (fronts, surrounds) on top. The volume sliders apply
after that.

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

**Saturation vs wrap.** The microcode runs in 16-bit mode (`SET16` at uc
0x0015) and switches to 40-bit mode (`SET40`) only around the resampler, the
IIR, the voice mixers, the output level (0x0130), AFC/PCM8 decoding and the
distortion. In 40-bit mode a read of an accumulator's middle word saturates; in
16-bit mode it is the raw word, so a result that overflows wraps around. The
passes that run in 16-bit mode are the bus-to-bus mixes of steps 4-8 (uc 0x03BD:
centre, chorus, rear-right, effect-line outputs), the effect-line FIR (0x01C2)
and the voice FIR (0x01E1). The port wraps there and saturates everywhere else,
like the microcode. LM's levels never overflow in the scenes tested
(`LM_AUDIO_LOG` reports any wrap; none in the foyer, the title or the Nursery),
so this only matters for a sound mixed louder than LM ever does.

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
- **Stop request (uc 0x05E0).** Every subframe each mixer's *target* volume
  becomes its current volume shifted right by one (`LRRD` reads the current
  word, `SN` stores the halved value one word lower, into the target; the shift
  is logical). The mixers then ramp from the current volume to that target as
  usual, so a stopped voice fades out smoothly, halving every subframe (a full
  volume 0x3C78 voice takes 15 subframes, 37 ms). The voice is done once all six
  current volumes are 0 at the start of a subframe (the test sums the shifted
  40-bit values, so a volume of 1 still counts). The halved targets are written
  back to main memory with the rest of the first 0x100 bytes.
- **Mixers (uc 0x05F6, 0x011E).** For each of the 6 mixers with a bus:
  - The delay's low byte moves one sample per subframe toward its high byte, and
    the mixer reads that many samples back (JAudio's pan delay; history is the
    last +0x0E samples of the previous subframes). If the current delay is
    larger than the history kept (uc 0x061C), the mixer is skipped for the
    subframe and its volume stays where it was. A mixer without a bus still
    ramps its volume (the microcode mixes it into scratch memory).
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

**The game's sound mode.** The options byte (`lbl_803CFAA0` + 0: 0 mono,
1 stereo, 2 surround; byte 1 is rumble) is applied by `fn_8005582C` /
`fn_800558C8` (at boot and when a save's options are loaded; they force mono
when the console's SRAM says mono) and by the options menu, all through
`fn_8018A70C(mode)` -> `fn_80190DB8(JAIBasic, mode)`, which

- stores the mode in JAIBasic + 0x41. `setParameterSeqSync` reads it when a
  sequence starts: in mode 2 the sequence's "dolby" external parameter
  (`Jam_SetExtParam(..., 0x10)`) comes from its entry in the sequence table
  (+0x14); in modes 0 and 1 it is 0. Sound effects do not depend on the mode.
- calls `Jac_SetOutputMode(0)` for mono, else 1. In JAudio's
  `UpdateEffecterParam` mode 0 centres the pan and zeroes the dolby send.
- sets the AFC streamer's mono flag (`fn_80194804`, 1 for mono).

The dolby parameter drives the voices' sends to the surround bus 0x0F40 (bus
routing in the bank/channel setup, `sin(dolby)` like the pan). So the only
difference between the stereo and surround modes is how much the *sequenced
music* sends to the surround bus; the stereo mode still has some surround
content from effect lines 2 and 3 (reverb returns, centre x 0.5 and surround x
0.5). Measured (`LM_AUDIO_LOG` bus report, foyer, 2 s windows): surround bus RMS
5-27 in stereo, 69-400 in surround (up to 1336 when the music swells), centre
unchanged. The rear-right bus 0x0FA0 and the chorus bus 0x0B00 were never used
in the scenes tested.

The DSP folds the surround bus into the stereo output in Dolby Surround form
(+S on the left, NOT S on the right; the centre at 0.707 in both). That stereo
pair is what a console outputs; a Pro Logic / Pro Logic II receiver unfolds it.

**F1 > Sound > Output** is the same setting as the game's own option:

| Output | game mode | what plays |
|---|---|---|
| Stereo | 1 | the game's stereo |
| Mono | 0 | the game's mono mode (centred pan, mono streams), both channels averaged |
| Surround (matrix-encoded stereo) | 2 | the console's Lt/Rt stereo, for a Pro Logic (II) decoder |
| Surround 5.1 (discrete) | 2 | FL, FR, C, LFE (silent), surround L/R |
| Surround 7.1 (discrete) | 2 | as 5.1, surround split over side and back pairs |

Hooks (`hle_audio.cpp`): `fn_8005582C` / `fn_800558C8` write the F1 choice into
the options byte before and after the original runs, so saved options never
override it and the options screen shows it; `fn_8018A70C` called from the
options menu changes F1 to the game's choice (keeping 5.1 / 7.1 when the game
picks surround) and saves the settings; a change in F1 is applied from JAudio's
audio thread (`DSPReleaseHalt`) by calling `fn_8018A70C` with the new mode and
updating the options byte. As on a console, a new mode applies to sequences
started afterwards. Test aid: `LM_AUDIO_SWITCH=<subframe>:<output>` changes the
F1 setting after that many DSP subframes (400 per second); with `LM_AUDIO_LOG`
the switch shows as `sound mode 2 (game asked 2, F1)` and the surround bus RMS
rises once the next sequence starts.

**Discrete 5.1 / 7.1.** The DSP stand-in records, per output sample, what the
centre, surround and rear-right buses added to each side at the output level
(`SideSample` in `audio.h`; one store per DSP output buffer). `Jac_imixcopy` is
hooked to tag the AI block it fills with the DSP buffer it came from
(`outTagBlock`). When the block plays, the fronts are the AI samples minus those
parts (so the CPU-side mix callback, e.g. the boot jingle, stays in the fronts),
the centre speaker plays the centre bus at full level (it was a phantom centre
at 0.707 in each side), and each surround speaker plays the surround bus at 1
(5.1) or 0.707 (7.1, side and back pairs), which keeps the power the matrix gave
it; the rear-right bus (unused by LM) goes to the right surround(s). LFE is
silent: the game has no LFE bus. Rebuilding Lt/Rt from a 5.1 capture without
resampling (`FL + 0.707 C + S`, `FR + 0.707 C - S`) matches the console stereo
to 1 LSB on the left and 3 LSB on the right (the NOT's -1, doubled by the
output level). The layout needs a device with 6 / 8 channels (F1 shows the
device's channel count); 7.1 falls back to 5.1 on a 6-channel device and both
fall back to the matrix-encoded stereo on a stereo device. Without a device
(`LM_AUDIO=0`, WAV captures) the chosen layout is used as is.

## DVD audio streaming

JAudio's `StreamMain` (jaudio/streamctrl.c) drives DTK streaming through
`DVDPrepareStreamAsync` / `AISetStream*`, but the disc has no DTK tracks and LM
never uses it. Those SDK calls stay stubs. The game's music is sequenced (BMS
via JAudio) plus AFC streams through DSP ring voices, as described above.

## Verification

Use `LM_AUDIO_WAV=build/x.wav` to record the output (what goes to the device),
`LM_AUDIO_WAV_RAW=build/y.wav` to record the AI stream (the console's stereo at
32028 Hz) and `LM_AUDIO_LOG=1` to log the sound mode, per-bus RMS, wraps,
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

Third pass (2026-10): sound mode, surround, resampler, rate, microcode details.
Captures with scratch APPDATA/card and `LM_BACKGROUND=1`; `LM_AUDIO_WAV` now
records what goes to the device (all channels, device rate) and
`LM_AUDIO_WAV_RAW` the AI stream itself (stereo, left first, 32028 Hz).

- **Channel order**: confirmed in game (see Channel order).
- **Sound mode / surround**: see Surround for the bus measurements. Foyer, 30 s
  of music (70-100 s): stereo mode L/R correlation 0.977 (side/mid 0.11),
  surround mode 0.773 (side/mid 0.39): the matrix-encoded surround music. In
  5.1 the same passage has FL = FR (correlation 1.000, RMS 640 each), centre RMS
  16 and each surround RMS 193: the music's stereo width in that room comes
  entirely from its surround send.
- **Resampler**: energy between 16.5 and 24 kHz (images of the 32 kHz source)
  relative to the total, foyer, 104 s at 44.1 kHz: -82.8 dB with the windowed
  sinc, -52.4 dB through SDL3's resampler (the same input fed to an
  `SDL_AudioStream` offline). Levels are identical (RMS 1873 / 1886 both).
- **Before/after** (same scripted foyer walk, 113-117 s): old build RMS 1716 /
  1680, 1 clipped sample; new build 1857 / 1826, 1 clipped sample (different
  stretch of music: the runs are not sample-aligned).
- **Stop fades**: `LM_AUDIO_LOG` logs `voice N stopped: faded from L/R in K
  subframes`; e.g. 0x3C78 -> 0 in 15 subframes.
- **CPU**: the mixer stays at 0.22-0.27% of a core with the side outputs.

## Known gaps

- Oscillator sources (types 0-4, 6, 7, 0xB-0xD) are silent; LM does not use
  them.
- Non-looping AFC samples whose length is not a multiple of 16 lose their last
  partial frame on hardware; the port plays it.
- The options-menu -> F1 direction of the sound-mode sync was traced but not
  driven by a scripted test (the F1 -> game direction and the saved-options
  override were).
