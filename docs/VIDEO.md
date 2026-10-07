# MPEG-1 video and Layer II audio

BaseOS decodes actual MPEG-1 video inside its own C/x86 kernel with the
MIT-licensed `pl_mpeg` video decoder. MPEG-1 Layer II (MP2) audio uses the existing
CC0-licensed `minimp3` decoder's scalar Layer II path and the original BaseOS
Sound Blaster 16 driver. Frames use the desktop's software framebuffer and
compositor. No Linux/BSD subsystem, host player, codec service or external
transcoder is involved during guest playback.

## Supported files and controls

- MPEG-1 video in an MPEG-1 program stream, `.mpg` or `.mpeg`
- I, P and B pictures; sequence/GOP headers and user data
- Up to 640 × 480 visible pixels, including non-macroblock dimensions
- 24000/1001, 24, 25, 30000/1001, or 30 frames/second
- Up to 2 MiB per source file and 18,000 pictures
- Constant width, height, frame rate and pixel aspect ratio within a clip
- Optional MPEG-1 Layer II CBR audio, mono/stereo, 32/44.1/48 kHz
- First audio/video PTS alignment within a two-second relative start offset
- Full MP2 encoder priming and final padded frame are retained
- Letterboxed fit-to-window, MPEG-1 pixel-aspect correction and nearest-neighbor scaling
- Play, pause, resume, stop, replay, volume, progress and elapsed/total time
- Pause while loading/prefilling; playback continues while minimized

The UI says `MPEG-1 + MP2` only when audio has actually been enabled. A missing
SB16, unsupported MPEG audio, changing audio parameters or an unsupported start
offset falls back to visibly labeled video-only playback. Video without an audio
stream also works. WAV/MP3 remain separate transports; opening one stops MPEG,
and opening MPEG stops the previous audio transport. Closing calls
`player_close()`, which stops both.

MPEG-2 video, MP4/H.264/H.265, WebM, AVI, QuickTime, DVDs, transport streams,
MPEG-2 Layer II, embedded MP3, standalone `.mp2` files, variable-rate Layer II,
seeking and subtitles are not implemented. Audio/video timestamp discontinuities
or edits within a stream are not supported; timing assumes continuous streams
at the validated frame/sample rates after initial PTS alignment. This decoder
is for known ordinary files, not a hardened sandbox for untrusted compressed
input. Container/header checks and allocation bounds are not a security review
of third-party entropy decoding.

The engine owns a source copy. Editing, moving or deleting the filesystem item
does not invalidate active playback. Media Player lists WAV/MP3/MPEG together,
excluding Trash. Space pauses, Enter opens selection, S stops, R refreshes and
`+`/`-` change volume. The larger window also has volume buttons. Suggested outer
size is 640 × 614; resizing retains a fitted video image and usable controls.

## Timing and cooperative work

`video_play` checks the outer header and copies input. Subsequent loading polls
consume up to 16 small pack/system/PES units within a 32,768-byte budget,
or one larger legal PES unit (at most 65,535 payload bytes), scan at most
32,768 video bytes, or inspect one bounded batch of MP2 frames. Batching tiny
PES packets avoids spending one desktop tick on every 2 KiB packet. Every sequence
header is validated before decoder construction, including the actual presence
and full length of optional quantization matrices in the original stream.
Complete tiny single-frame clips do not need maximum-header-size padding.
No playback loop waits for an entire clip to decode.

Each video poll produces at most one display-order frame. The initial result
may need two reference pictures because MPEG reorders I/P/B pictures. Audio's
PCM callback decodes at most one 1,152-sample MP2 frame, or copies previously
decoded leftovers/leading silence. It is called at most once per audio poll.
There is no allocation during frame/audio decode and neither runs in an IRQ.

With MP2 enabled, the actual SB16 DMA position is the master clock. Initial PTS
differences insert sample-exact leading silence or delay video. Pausing freezes
both transports, including initial PCM buffering. The final video picture is
held through its full presentation interval and any remaining audio tail. If
audio finishes first, PIT time supplies only the remaining video interval.
Duration is the longer of the aligned video and complete decoded audio.

Every compressed picture is decoded for prediction correctness. If its next
picture is already due, the engine can omit a redundant redraw request and
let the desktop catch up using the newest prepared pixels. `displayed_frames`
counts produced display-order frames; `presentation_skips` counts deferred
redraw requests, not skipped decoder/reference pictures. If video falls more
than 500 ms behind its audible clock, playback stops with an explicit error.
Silent playback instead rebases after long desktop stalls and may run slower
than nominal. Real-time VGA playback is not guaranteed on every emulator host.

### Compositor integration

`player_tick()` services video and returns exactly one of:

- `0`: no client redraw requested
- `PLAYER_CHANGED` (`1`): redraw the full client with `player_draw`
- `PLAYER_VIDEO_FRAME` (`2`): viewport/progress-only redraw is sufficient

`player_draw_playback(x,y,w,h)` draws only the video rectangle and progress/time
labels. The caller may use it only for an unobscured, frontmost Media Player,
with no overlapping menu, dialog, launcher, drag, saver or other overlay, and
must restore/save the cursor around the update. Otherwise use the unchanged
full-client/full-desktop path. Volume, library refresh and state changes request
full redraws. Hidden players still need ticks; `audio_poll()` remains independent.

The partial path avoids repainting unrelated windows each video frame.

## Memory and FPU contract

Video work stays within the reserved 9 MiB region `0x3600000..0x3f00000`:

| Offset | Reservation |
| --- | --- |
| 0 MiB | 2 MiB owned MPEG-PS source |
| 2 MiB | 2 MiB extracted video stream |
| 4 MiB | 2 MiB bounded decoder arena, including MP2 state/PCM |
| 6 MiB | Up to 307,200 fixed-palette pixels |
| 7 MiB | 2 MiB extracted MPEG audio stream |

Static assertions cover the decoder objects, three 640 × 480 YUV420 reference
frames and MP2 PCM storage. The driver separately owns its existing 64 KiB ISA
DMA ring and a fixed 8 KiB resampling staging buffer. No heap is introduced.

Only the scalar decoder translation units use x87. Constructors and frame/audio
decode save/restore all 108 bytes of x87 state plus CR0; explicit x87 register
clobbers prevent compiler allocation across that boundary. Desktop drawing
remains soft-float and no SSE is required. BT.601 conversion uses ordered color
dithering and the existing gray ramp; video never changes the desktop palette.

### 48 kHz output

QEMU SB16 clamps requested rates above 45,000 Hz. The driver now linearly
resamples all higher-rate WAV, MP3 and callback PCM sources to 44,100 Hz using a
fixed staging buffer and a one-sample lookahead. Source sample rate, frame counts
and time remain source-based; `AudioStatus.output_rate` reports the actual
hardware rate. DMA deadlines and the 50 ms final drain use that output rate.
Output length is `ceil(source_frames × output_rate / source_rate)` with the last
sample held for interpolation. This prevents the previous low-pitch/slow 48 kHz
behavior. It is bounded linear interpolation, not a high-order mastering filter.

QEMU's rate constraint is documented in its official implementation:
https://raw.githubusercontent.com/qemu/qemu/master/hw/audio/sb16.c

## Decoder provenance

`third_party/pl_mpeg/README.md` records its pinned revision, untouched upstream
hash, MIT license and exact local patch. Three tested fixes preserve fixed-memory
EOF accounting, flush the final delayed reference after a B picture, and require
only the sequence-header matrices actually present with checked bounds. A
local elementary-stream terminator/zero guard supplies final VLC lookahead;
the original MPEG-PS bytes remain unchanged.

Layer II uses the already-pinned, unmodified minimp3 header with its MP3-only
compile restriction removed. Its MP3 file adapter still accepts Layer III only;
MPEG video's MP2 adapter validates Layer II boundaries separately. Independent
comparison exposed significant normalization/reconstruction differences in
pl_mpeg's unused MP2 path, so that path is not used or patched. Minimp3 Layer II
matches the reference within two signed-16 sample levels in the checked files.

## Verification

```
make
python3 -m unittest discover -s tests -p test_video.py
python3 -m unittest discover -s tests -p test_mpeg_av.py
python3 -m unittest discover -s tests -p test_video_draw.py
python3 -m unittest discover -s tests -p test_player.py
ASAN_OPTIONS=detect_leaks=0 python3 -m unittest discover -s tests -p test_audio.py
python3 tools/video_test.py build
python3 tools/mpeg_av_test.py build
python3 tools/mpeg_av_test.py build --controls
python3 tools/mpeg_av_test.py build --rate 48000
python3 tools/mpeg_av_test.py build --rate 32000 --channels 1
python3 tools/mpeg_av_test.py build --rate 48000 --native
python3 tools/video_input_test.py build
```

All fixtures are original locally generated content. FFmpeg encodes them and
provides an independent decoder reference; it is never a guest runtime dependency.
Host tests use ASan/UBSan, ordinary valid media and deterministic controls, with
no malformed corpus or intentional memory-fault probes. `detect_leaks=0` handles
the execution environment's unsupported LeakSanitizer/ptrace combination; address
and undefined-behavior checks remain enabled.

The tiny-stream regression includes valid 16x16 one-frame solid-color clips
with 41-byte elementary streams, plus custom intra-only, non-intra-only and
both-matrix variants (105, 105 and 169 bytes). Every decoded Y/Cb/Cr sample is
compared with FFmpeg; all four sequence-header flag combinations are checked.

The video reference suite covers every Y/Cb/Cr plane and final block, I/P/B,
fractional rates over 40 seconds, PIT wrap, VGA, padded dimensions, single frames,
non-square pixels, source disposal, controls and valid unsupported formats. The
A/V suite covers 13 ordinary fixture variants, every audio sample and video
frame, six sample-rate/channel pairs, initial PTS offsets, shorter/longer audio,
independent clocks, replacement, loading/preroll/playback pauses and silent
fallback. Observed MP2 worst block RMS was 0.542 LSB and peak difference 2 LSB.

QEMU checks boot disposable real BaseOS images and use the actual IDE driver,
MPEG decoders, SB16 DMA, compositor and PS/2 keyboard. They compare all 75 guest
video-frame hashes and every audible PCM sample, verify full tails, x87 state,
responsive terminal input, pause/resume/stop/replay, and capture real framebuffer
screenshots. The 48 kHz run compares every output frame with the linear
44.1 kHz reference and checks that the original 271 Hz tone keeps its pitch.
The build's persistent disks are never opened by these tests.

`--native` additionally runs two independent C counter applications and two
separate live-x87 context probes through the same A/V loop. It checks counter
progress and separate saved documents, clean x87 task exit, one-terminal close
isolation, task stop, full audio/video completion and unchanged decoder output.
The 48 kHz run also verifies every output PCM frame and all 75 video frames with
no audio underrun. The guest fixture uses the real scheduler, system calls, filesystem and compositor APIs;
production keyboard/compositor routing is checked separately.

## Production desktop input regression

`tools/video_input_test.py` boots the unmodified production kernel and performs
all actions through real PS/2 keyboard/mouse input. Bounded reads of named ELF
data symbols observe state; no test kernel, guest function calls or memory
writes are injected. It records the kernel/ELF/boot hashes, build identity,
original fixture metadata, input events, screenshots and a JSON result. It
never opens the build's persistent boot/data disks.

The test plays a valid 1.60 MiB, 15-second MPEG. Coverage includes app/file launch, visible frame/audio
progress, pause/resume, maximization, File-menu/launcher/Open-dialog pixel
preservation, minimized background playback, physical taskbar restore, close
clearing both transports and callback/frame state, MPEG stop/replay, MP3/WAV
selection and audio-only close.

Two production regressions explain specific assertions:

1. Before packet batching, the same ordinary 1.60 MiB stream remained in
   `VIDEO_LOADING` after 15 seconds, with no decoder/device error. Bounded
   32 KiB batching fixed startup; both host loading-poll and production-time
   assertions retain that coverage.
2. Before unconditional compositor servicing, an isolated Open-dialog run
   stopped at frame 121 with one audio underrun. Its visible moving video rows
   were 157–188, with none divisible by 64. The old hook ran only through
   changed framebuffer writes at 64-row boundaries, starving PCM refills.
   The fixed compositor services its existing hook at entry and while scanning,
   independently of changed rows. The same modal test now advances audio/video
   without an underrun or overwriting the modal's pixels.

The test takes raw PPM screendumps and encodes PNG on the host afterward to
minimize observer-induced emulator stalls. It uses typed QMP PS/2 key events
with explicit hold times. The output directory printed by each run retains its
result even when a test fails.

## Generate and import an original sample

```
python3 tools/mpeg_av_fixture.py build/video-demo --rate 44100
```

After shutting QEMU down, import into an initialized data disk:

```
python3 tools/volume.py build/baseos-data.img mkdir /Video
python3 tools/volume.py build/baseos-data.img import build/video-demo/av.mpg /Video/av.mpg
```

Boot again and choose it in Media Player. `tools/video_fixture.py` also generates
silent video. These tools produce test/demo assets; BaseOS does not transcode
user files or relabel other codecs as MPEG-1.
