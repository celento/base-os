# Audio and media playback

BaseOS has an original, allocation-free Sound Blaster 16 driver and PCM WAVE
reader, plus the CC0-licensed minimp3 decoder. It targets QEMU's SB16 at port `0x220`, IRQ5, low DMA1/high DMA5. IRQ5
remains masked: the cooperative main loop reads the high-DMA position, refills a
64 KiB double buffer and acknowledges the DSP's interrupt latch. The floppy
controller continues using its independent DMA2 channel.

## Supported files

- RIFF/WAVE PCM format 1, unsigned 8-bit or signed little-endian 16-bit
- Mono or stereo, 5,000 through 48,000 samples/second on QEMU SB16
- Optional chunks, odd-byte chunk padding, and either fmt/data order
- MP3: MPEG-1/2/2.5 Layer III CBR/VBR, mono/stereo, with ID3v2/ID3v1 tags
- Files are validated before replacement of the current playback
- Playback owns a copy, so changing or deleting the source file is safe

Other codecs, free-format MP3, WAVE extensible, floating-point WAVE, recording,
MIDI are not implemented. MPEG-1 video with optional Layer II audio is documented
in [VIDEO.md](VIDEO.md). MP3 encoder delay/padding is retained;
gapless trimming and seeking are not implemented. MP3 streams must keep the
same sample rate and channel count throughout. Sources above QEMU SB16's
45 kHz limit (including normal 48 kHz files) are linearly resampled to 44.1 kHz.
Source-rate metadata/time stay separate from `AudioStatus.output_rate`.

The default IDE data volume supports 2 MiB per file and roughly 8 MiB total
payload; its owned audio-input arena accepts up to 2 MiB. An explicitly mounted
[large storage profile](LARGE_VOLUMES.md), with verified high filesystem arenas, permits up to
**16 MiB of owned WAV/MP3 source data**. This is a file-size bound rather than a
duration guarantee: PCM sample rate/channels/bit depth or MP3 bitrate determine
how much playback fits. The optional legacy floppy-only volume retains its
original limits. Larger RAM alone does not increase audio capacity; a default
volume running with 128 MiB still uses the independent 2 MiB source arena.
No streaming file I/O, new codecs, larger MPEG/video bound, or larger network
download buffer is implied by the large audio profile.

`tools/make_audio_example.py build/chime.wav` generates an original one-second
melody that fits the filesystem. After booting a fresh disk once and stopping
QEMU, import it with:

```
python3 tools/volume.py build/baseos-data.img import build/chime.wav /chime.wav
```

## Integration

Include `audio.h`, initialize after memory validation, call `audio_poll()` in the
desktop loop and in the non-reentrant `platform_poll()` input collection hook.
Use `audio_play(data, bytes)`, `audio_pause(1/0)`, `audio_stop()` and
`audio_set_volume(0..100)`. `audio_status()` reports device availability, state,
format, source/output rates, channel count, source bit depth, played/total
source frames and errors.
Position/duration helpers return milliseconds. Playback and volume are global.

After `fs_load_disk()` selects its backend, call
`audio_configure_source_workspace(fs_large_profile(), fs_large_arenas_available())`.
Both conditions must hold before audio owns the vacated low filesystem arenas.
`fs_init()` alone is too early: it seeds the default low arenas before mount.
Configuration returns 1 only while `AUDIO_STOPPED`; loading, playing, paused,
finished and error states return 0 without changing the source pointer or
capacity. Stop before configuration. Any future filesystem reinitialization or
mount reconfiguration must first stop audio and release the expanded workspace
with `audio_configure_source_workspace(0, 0)`. Normal file saves and compaction do
not reconfigure mounts.

Successful replacement validates before stopping the prior transport. MP3 header
scans service the device-only poll every 64 frame headers while leaving the old
decoder intact; no callback occurs after the new decoder metadata is committed.
The owned copy then services devices after each at-most-16-KiB chunk with audio
stopped, so it cannot decode a partially replaced source. These hooks collect
input and service devices without dispatching app actions or filesystem writes.
Rejected candidates retain existing playback/source bytes and expose the exact
validation error. `audio_capacity_bytes()` reports the selected runtime limit.

Each poll converts at most 4,096 PCM samples, decodes at most one MP3 frame, or
calls an `AudioPcmReader` once. `audio_play_pcm_stream` validates a declared
source-frame count and supplies bounded interleaved signed-16 PCM; short reads
are allowed and explicit EOF must match that count. The caller owns the reader
context until stop, replacement, error or completion. Callback code must not
reenter the audio driver. A fixed 8 KiB staging buffer handles resampling.

Every poll is bounded; playback does not wait in a busy loop. WAV needs eight
polls to prefill the ring; compressed audio takes additional frame-decode polls.
At 44.1 kHz stereo, each half holds about 186 ms. Service audio at the normal
70-Hz PIT cadence and from bounded rendering/I/O hooks. A 1,152-sample MPEG frame
at 48 kHz provides only 24 ms of audio: a brief 70 ms service gap is a tolerance,
not a sustainable polling interval. Refill service must not depend on which
screen rows happen to change or whether a modal obscures the video.

A missed refill or a delay longer than the complete ring stops playback with an
explicit underrun error rather than repeating stale music. Host overload,
including competing emulators or expensive synchronous capture work, can cause
this safe stop even with a valid file. Run timing-sensitive QEMU checks in an
isolated emulator. The legacy synchronous `exec` path pauses audio around its
up-to-two-second run; asynchronous `start` tasks use bounded slices alongside
media, as verified in [NATIVE_TASKS.md](NATIVE_TASKS.md) and [VIDEO.md](VIDEO.md).
Do not run `audio_poll()` from an IRQ handler.

A 50 ms zero-filled drain preserves the end of the clip before deactivating the
QEMU voice. Pause/resume retains the DMA cursor. Missing devices, busy DSP ports,
and device faults return bounded errors and do not block the desktop.

Memory reservations:

- `0x710000..0x720000`: 64 KiB audio ISA DMA ring
- `0x1700000..0x1900000`: default/legacy owned audio input, 2 MiB
- `0x2000000..0x3000000`: large-profile owned audio input, 16 MiB, only when
  filesystem data and staging both use their verified high arenas

## QEMU and verification

For live audio, add `-audiodev DRIVER,id=sound -device sb16,audiodev=sound`, using
an installed QEMU backend such as `coreaudio`, `pa`, or `alsa`. A no-speaker test
can use `-audiodev none,id=sound -device sb16,audiodev=sound`. Hardware without an
SB16 is detected and the rest of the desktop remains usable.

```
make
python3 tools/audio_test.py build
python3 tools/mp3_test.py build  # additionally needs ffmpeg with libmp3lame
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

The driver and WAV parser are original project code. minimp3 is pinned in
`third_party/minimp3/` with its complete CC0 1.0 license and source attribution.
It is compiled scalar in a separate x87 translation unit. The MP3 file adapter
accepts Layer III; the MPEG-video adapter separately uses its Layer II decoder. Every frame
saves/restores the complete x87 state and CR0 flags; other kernel code remains
soft-float. This uses the existing Pentium-or-newer machine contract, without
requiring SSE or a hosted C runtime.

The MP3 test creates an original short audio fixture through ffmpeg/libmp3lame,
imports it into the guest filesystem and captures actual SB16 output. It checks
playback completion, no underruns, x87 environment preservation, and agreement
with a separately decoded host PCM reference. Normal MPEG-1 stereo 44.1 kHz,
MPEG-2 stereo 22.05 kHz, and MPEG-2.5 mono 8 kHz fixtures have been verified.

## Media Player client

`src/player.c` is a singleton client-area app. The desktop owns its window and
calls `player_init`, `player_draw`, `player_click`, `player_key`, and
`player_tick`. `player_open_file(id)` opens and plays a filesystem item directly;
`player_refresh()` updates its alphabetical WAV/MP3/MPEG-1 library. The library skips
Trash descendants, preserves selection across refreshes, checks file identity
before opening a previously selected slot, and shows the actual decoder input
limit. It can accept larger filesystem files later without a new player API.

Controls include Play/Pause, Stop, Clear error, Refresh, a clickable volume
slider, elapsed/total time and progress. Arrow keys move through the library;
Enter opens the selected file; Space toggles playback; `+`/`-` change volume;
`R` refreshes and `S` stops. A single list click selects a track. The source
name remains visible if its file is moved or changed during playback because
the audio engine owns a separate input copy.

The caller must keep servicing `audio_poll()` even if the player window is
minimized or closed. `player_tick()` services bounded video playback and reports visual changes.
`player_close()` stops both transports. Suggested outer size is `PLAYER_W` ×
`PLAYER_H` (640×614), with a minimum client area 420×414. MPEG-1 video with supported MP2 audio uses the same SB16 driver; missing or
unsupported audio is visibly labeled video-only. The optional
`PLAYER_VIDEO_FRAME`/`player_draw_playback` compositor path repaints only its
viewport and progress. Stable audio position changes similarly report
`PLAYER_AUDIO_PROGRESS`; `player_draw_audio_progress` restores and repaints only
the opaque progress/time strip, avoiding accumulated antialiasing or stale text.
Both paths require a frontmost, unobscured, stationary client with no ordinary
desktop dirty work or overlays. Audio state, error, volume, title, metadata,
library/selection changes and the regular library refresh keep full redraws;
exposed background clients also keep full composition. Host tests compare
repeated partial updates with full rendering pixel-for-pixel, including progress
shrink/reset, small/large durations, different geometries and card themes.
See [VIDEO.md](VIDEO.md).

`python3 tools/player_test.py build` exercises real QEMU playback, keyboard
pause/resume, volume, playlist selection and stopping. It also captures the
client inside the native BaseOS window chrome. `test_player.py` checks legal
input workflows and rendering bounds under ASan/UBSan.

## Host scheduling load

The normal desktop played the full original 18-second Harbor MP3 and nine-second
MPEG soundtrack with sample-accurate captures and no underruns when run as one
QEMU instance. A development stress run with several concurrent emulators
triggered the explicit underrun stop. This is not a real-time guarantee: a host
that cannot service the guest often enough may stop playback with an error.
The driver does not hide the problem by replaying stale DMA data.

## Large-source host verification

`ASAN_OPTIONS=detect_leaks=0 python3 -m unittest discover -s tests -p 'test_audio*.py'`
checks the default transport and the large owned-source path under ASan/UBSan.
The large test also consumes and verifies every PCM sample of exact-capacity
2 MiB and 16 MiB WAV sources through EOF. It generates an original
Harbor-derived three-minute 192 kb/s MP3
with FFmpeg, then compares every simulated DMA sample with a separate decoder
through EOF. It also checks exact 2 MiB/16 MiB WAV boundaries, a legal ID3-padded
MP3 at exactly 16 MiB, complete byte ownership after source mutation/deletion,
all four active-profile/available-memory combinations, replacement rejection,
configuration refusal during live playback, preservation of the default buffer,
continued old-decoder refills while rejecting a real rate/channel-changing MP3
candidate, pause/resume/volume and the unchanged
one-frame-per-poll decode bound. These are host-driver checks; real SB16/QEMU
capture evidence is recorded separately after running the production workload.

## Large-source production QEMU verification

`python3 tools/large_audio_input_test.py build` runs fresh default and large
volumes serially with 128 MiB RAM and the ordinary production desktop. Use
`--profile default` or `--profile large` to run one case. The runner requires
FFmpeg, QEMU and the existing production-input/reference-decoder dependencies.
All commands use normal PS/2 input. Named-symbol memory reads observe source
ownership and state; no guest calls, guest memory writes, test kernel, fuzzing
or intentional memory-fault probes are used. Saved user images are never opened.

The October 4 verification used production kernel `19f5028`:

- Default 16 MiB volume plus 128 MiB RAM kept its 2 MiB source at 23–25 MiB,
  alongside the live low filesystem pool and staging. An ordinary filesystem
  save succeeded during playback. Every one of 796,032 stereo PCM frames was
  verified; RMS difference 0.021133 LSB, peak 1, zero reported underruns.
- The explicitly mounted 64 MiB large volume selected the owned 16 MiB source
  at 32–48 MiB and high filesystem pool/staging. Its original Harbor-derived
  192 kb/s MP3 was 4,320,862 bytes. Playback finished all 7,939,584 stereo PCM
  frames (15,879,168 samples), including the complete encoded tail, in
  180.035918 seconds. RMS difference was 0.020736 LSB, peak 1; zero underruns.
- During that complete large capture, Files copied and synchronized an exact
  2 MiB document. Terminal deleted a preceding 2 MiB padding file and then the
  playing MP3 itself, each with compaction and a verified disk save. Playback
  remained independent of both operations. A protected native counter stayed
  active through EOF and durably saved increasing values 11, 61 and 116.

The production input harness waits for actual focused-window creation and
completed close before selecting taskbar coordinates. Two initial large
attempts stopped on harness observation races at those transitions; their
partial captures are not counted as complete-playback evidence. No audio code
change was needed for the final complete run. The retained measured results
are in [LARGE_AUDIO_RESULTS.json](session-2026-10-04/LARGE_AUDIO_RESULTS.json),
with a [workload screenshot](../screenshots/large-audio-files-native.png).
This is evidence for a bounded QEMU workload, not arbitrary-media compatibility,
real-hardware timing, streaming, MP4/H.264 or Internet video support.
