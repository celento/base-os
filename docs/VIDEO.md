# MPEG-1 video playback

BaseOS decodes actual MPEG-1 video inside its own C/x86 kernel using the
MIT-licensed `pl_mpeg` decoder. The Media Player presents video frames through
the same software framebuffer and window compositor as the rest of the desktop.
No Linux/BSD subsystem, host video player, codec service, or external transcoder
is involved at playback time.

This milestone is **video only**. MPEG audio packets are detected but skipped;
the player explicitly displays **"Video only (no audio)"**. WAV/MP3 audio playback
continues to work as a separate transport. Opening a video stops the audio
transport, and opening WAV/MP3 stops video. There is no A/V synchronization claim.

## Supported files and controls

- MPEG-1 video in an MPEG-1 program stream, `.mpg` or `.mpeg`
- I, P and B pictures; sequence/GOP headers and user data
- Up to 640 × 480 visible pixels, including dimensions not divisible by 16
- 23.976 (24000/1001), 24, 25, 29.97 (30000/1001), or 30 frames/second
- Up to 2 MiB per source file and 18,000 pictures
- Constant width, height, frame rate and pixel aspect ratio within a clip
- Letterboxed fit-to-window, pixel-aspect correction and nearest-neighbor scaling
- Play, pause, resume, stop, replay, progress, elapsed/total time and completion
- Pause during asynchronous loading, and background playback while minimized

MPEG-2 video, MP4/H.264/H.265, WebM, AVI, QuickTime, DVDs, MPEG transport streams,
interlaced MPEG-2, seeking, subtitles and MPEG audio output are not implemented.
The decoder is suitable for known ordinary media files, not a hardened sandbox
for untrusted compressed input. Metadata/container preflight and fixed allocation
limits are not a security review of the upstream decoder.

The playback engine owns a copy of the source. Moving, editing or deleting the
filesystem item does not invalidate an already playing clip. The library lists
WAV/MP3/MPEG files together and excludes Trash. Space toggles pause, Enter opens
the selected item, S stops, and R refreshes. Closing the player should call
`player_close()`; that stops both transports. Suggested window size is 640 × 614.

## Cooperative execution and storage

`video_play` checks the outer header and takes a source copy. Loading then
processes one pack/system/PES unit (at most 65,535 payload bytes) or scans at most
32,768 elementary-stream bytes per `video_poll` call. Every sequence header is
checked before decoder construction, bounding all frame allocations.

During playback, a call produces at most one displayed frame. The initial
output may require two coded reference pictures because MPEG reorders I/P/B
pictures. There is no loop that decodes an accumulated wall-clock backlog.
A pause preserves the current frame and shifts the playback origin on resume.
Integer rational timing avoids 23.976/29.97 frame-rate drift and handles PIT
counter wrap. The final frame remains on screen for its full presentation
period before completion.

If the desktop is more than half a second behind schedule, playback rebases its
clock and keeps the complete picture sequence. It may run slower than nominal
on a busy emulator; real-time 640 × 480 playback is not guaranteed. No compressed
frames or reference pictures are discarded to catch up. In the documented
320 × 240 QEMU check, the largest decoder poll took one 70-Hz PIT tick, with zero late resynchronizations; this is
an observation, not a hard real-time guarantee.

`player_tick()` services video even when the window is hidden. The existing main
loop still calls `audio_poll()` independently. Neither decoder runs in an IRQ.
The video translation unit alone uses x87, saving and restoring all 108 bytes of
x87 state and the original CR0 flags around constructors and decode calls.
Other desktop code stays soft-float. Explicit x87 register clobbers prevent
compiler allocation across the save/restore boundary.

All persistent video work is in the reserved 9 MiB region
`0x3600000..0x3f00000`:

| Offset | Reservation |
| --- | --- |
| 0 MiB | 2 MiB owned MPEG-PS source |
| 2 MiB | 2 MiB extracted video elementary stream |
| 4 MiB | 2 MiB bounded decoder arena |
| 6 MiB | Up to 307,200 bytes of palette pixels |

A static assertion proves the fixed-memory buffer, decoder object and three
640 × 480 YUV420 reference frames fit their decoder reservation. There is no
heap outside this region and no allocation while frames are decoded. The
remaining reservation is unused. Presentation converts BT.601 YCbCr to the
existing fixed palette using ordered color dithering and a finer gray ramp;
video never changes the desktop palette.

## Upstream fixes

`third_party/pl_mpeg/README.md` records the exact pinned revision, upstream hash,
license and local patch. Reference testing found and fixed two upstream issues:
fixed-memory buffers were unnecessarily shifted while their EOF length stayed
unchanged, and a final delayed reference picture was not flushed after a B
picture. A decoder-local sequence-end plus zero guard also supplies required
lookahead for ordinary MPEG-PS files without an explicit elementary-stream end.
It does not modify the source MPEG-PS file.

## Verification and original sample generation

```
make
python3 -m unittest discover -s tests -p test_video.py
python3 -m unittest discover -s tests -p test_video_draw.py
python3 -m unittest discover -s tests -p test_player.py
python3 tools/video_test.py build
```

The reference suite generates original changing YUV imagery using FFmpeg's
MPEG-1 encoder, then compares every visible Y/Cb/Cr sample to FFmpeg's independent
decoder. Its nine tests compare 2,483 frames, including full final pictures,
I/P/B, non-macroblock dimensions, one-frame clips, VGA, multiplexed MP2 detection,
40-second rational-rate timing, PIT wrap, owned-source disposal, pause while
loading/playing, late-poll recovery, and stop/replay. Ordinary valid MPEG-2 and
above-limit dimensions are rejected. It uses ASan and UBSan with no malformed
corpus or deliberate memory-fault probes. Observed worst per-plane MAE was
0.6745, RMS 1.0496, and peak difference 8 sample levels. Small differences reflect
scalar integer IDCT/prediction rounding; all final blocks are compared too.

The QEMU test boots disposable images with the actual data-disk driver. It
imports an original three-second 320 × 240 MPEG-1 clip, verifies all 75 canonical
guest frame hashes against the host decoder, injects real PS/2 keyboard
pause/resume/stop/replay, checks desktop terminal input during playback, and
checks x87 control-word/register preservation. It captures paused, stopped and
finished player screenshots in the printed evidence directory. These are actual
emulated framebuffer captures, not browser mockups. The build's persistent
images are never opened by the test.

To create an original sample for normal playback:

```
python3 tools/video_fixture.py build/video-demo
```

Shut QEMU down, then import the generated file into an initialized data disk:

```
python3 tools/volume.py build/baseos-data.img mkdir /Video
python3 tools/volume.py build/baseos-data.img import build/video-demo/sample.mpg /Video/sample.mpg
```

Boot again and choose the file in Media Player. FFmpeg is only a host-side
fixture generator and independent test reference; BaseOS directly decodes the
MPEG-1 bytes presented to it. It does not transcode users' files or relabel other
formats as MPEG-1.
