# Audio playback

BaseOS has an original, allocation-free Sound Blaster 16 driver and PCM WAVE
reader. It targets QEMU's SB16 at port `0x220`, IRQ5, low DMA1/high DMA5. IRQ5
remains masked: the cooperative main loop reads the high-DMA position, refills a
64 KiB double buffer and acknowledges the DSP's interrupt latch. The floppy
controller continues using its independent DMA2 channel.

## Supported files

- RIFF/WAVE PCM format 1, unsigned 8-bit or signed little-endian 16-bit
- Mono or stereo, 5,000 through 48,000 samples/second on QEMU SB16
- Optional chunks, odd-byte chunk padding, and either fmt/data order
- Files are validated before replacement of the current playback
- Playback owns a copy, so changing or deleting the source file is safe

Other codecs, WAVE extensible, floating-point WAVE, recording, MIDI and video are
not implemented by this initial audio core. Normal BaseOS files still have a
16,383-byte limit. The playback arena can accept up to 384 KiB from a future
larger-file source, but that does not enlarge the filesystem or add streaming
file I/O. Uncompressed CD-rate stereo therefore fits only very short clips in
the existing volume.

`tools/make_audio_example.py build/chime.wav` generates an original one-second
melody that fits the filesystem. After booting a fresh disk once and stopping
QEMU, import it with:

```
python3 tools/volume.py build/baseos.img import build/chime.wav /chime.wav
```

## Integration

Include `audio.h`, initialize after memory validation, call `audio_poll()` in the
desktop loop and in the non-reentrant `platform_poll()` input collection hook.
Use `audio_play(data, bytes)`, `audio_pause(1/0)`, `audio_stop()` and
`audio_set_volume(0..100)`. `audio_status()` reports device availability, state,
format, rate, channel count, source bit depth, played/total frames and errors.
Position/duration helpers return milliseconds. Playback and volume are global.

Each poll converts at most 4,096 PCM samples; playback does not wait in a busy
loop. The first eight polls prefill the ring. At 44.1 kHz stereo, each half holds
about 186 ms. Callers should service audio every 70 ms or better. A missed refill
or a delay longer than the complete ring stops playback with an explicit
underrun error instead of silently repeating stale music. Native programs still
run synchronously and may hold the desktop for up to two seconds; callers
should pause audio around native execution until the scheduler can service
media during that interval. Do not run `audio_poll()` from an IRQ handler.

A 50 ms zero-filled drain preserves the end of the clip before deactivating the
QEMU voice. Pause/resume retains the DMA cursor. Missing devices, busy DSP ports,
and device faults return bounded errors and do not block the desktop.

Memory reservations:

- `0x710000..0x720000`: 64 KiB audio ISA DMA ring
- `0x720000..0x780000`: owned audio input, 384 KiB

## QEMU and verification

For live audio, add `-audiodev DRIVER,id=sound -device sb16,audiodev=sound`, using
an installed QEMU backend such as `coreaudio`, `pa`, or `alsa`. A no-speaker test
can use `-audiodev none,id=sound -device sb16,audiodev=sound`. Hardware without an
SB16 is detected and the rest of the desktop remains usable.

```
make
python3 tools/audio_test.py build
python3 -m unittest discover -s tests -p test_audio.py
```

The QEMU check uses disposable images only and leaves its evidence directory
printed to the console. It records actual emulated SB16 output with QEMU's WAV
audio backend, checks an 8-bit file imported into the guest filesystem, then
checks all 88,200 frames of a two-second 16-bit stereo waveform sample-for-sample
across multiple DMA refills. The fixture redraws the desktop while playing and
checks exact completion without underruns. Host tests cover parser conversion,
container boundary validation and driver state transitions under ASan/UBSan.
Some ptrace-based sandboxes require `ASAN_OPTIONS=detect_leaks=0`; this disables
LeakSanitizer only, not address/undefined-behavior checks.

Hardware reference: Creative's *Sound Blaster Series Hardware Programming
Guide*, chapters 2–4 and 6:
https://www.phatcode.net/res/243/files/sbhwpg.pdf
QEMU backend/reference implementation:
https://www.qemu.org/docs/master/system/invocation.html
https://gitlab.com/qemu-project/qemu/-/blob/master/hw/audio/sb16.c

The driver and WAV parser are original project code; no third-party source is
included in this initial implementation.
