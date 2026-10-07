# Image Viewer and standard formats

The built-in Image Viewer decodes ordinary JPEG, PNG, BMP, GIF, and BaseOS Paint
(`BOS1`) files. It runs inside the existing C/x86 kernel and draws through the
software-rendered, 256-color desktop. No Linux layer, external image program,
floating-point runtime, or host image service is involved.

## Open and inspect an image

Import an image into the data disk while QEMU is stopped. Boot a new data disk
once before importing. For example:

```sh
python3 tools/volume.py build/baseos-data.img import photo.jpg /Pictures/photo.jpg
```

Open it through the desktop's Image Viewer/file association. The viewer title
shows the file name; the status line shows the detected format, original pixel
dimensions, and display scale. The file bytes are used only during a synchronous
open. Decoded pixels remain owned by the viewer after filesystem compaction,
source replacement, or another app's save.

Controls:

| Control | Action |
| --- | --- |
| Fit button, `F`, or `0` | Fit the image to the available area, up to 400% |
| 1:1 button or `1` | Actual pixel size |
| `+` / `-` or toolbar buttons | Zoom from 10% through 400% |
| Arrow keys | Pan a zoomed image |
| Mouse wheel, Page Up / Page Down | Pan vertically |
| Home | Return to the top-left at the current zoom |

The client works from 360 × 200 pixels, with a default 720 × 520 client size.
Fit adapts when the window is resized. Scaling uses nearest-neighbor sampling.

## Supported formats and limits

- JPEG: baseline, progressive, and grayscale examples are verified.
- PNG: RGB, RGBA, grayscale, gray-alpha, indexed color, monochrome, Adam7
  interlacing, and 16-bit grayscale converted to 8-bit output are verified.
- BMP: ordinary uncompressed RGB and indexed-color examples are verified.
- GIF: the first frame only, including transparency. The status line explicitly
  says `GIF (first frame)`; animated playback is not implemented.
- BOS1: BaseOS Paint's existing palette-indexed format is preserved. Its `.pbm`
  filename suffix does not mean it is a standard Netpbm PBM file.

The input file may be at most 2 MiB. Each side may be at most 1024 pixels, and the
image may contain at most 786,432 pixels. This includes both 1024 × 768 and
768 × 1024 portrait images. Larger files or dimensions get a visible limit
message rather than being silently truncated.

Decoding also has a fixed 6.25 MiB workspace budget. Some valid variants need
more temporary storage and will get a visible memory-limit message even when
their dimensions fit. For example, a verified 1024 × 768 progressive JPEG with
4:2:0 sampling fits, whereas the equivalent 4:4:4 progressive example exceeds
the budget. A 1024 × 512 GIF is verified; larger GIFs can hit the workspace
budget sooner than JPEG or PNG. The viewer does not partially display a failed
decode or silently keep the previous file under a new title.

Standard images are converted to the fixed color-cube and gray portion of the
desktop palette. Serpentine error diffusion preserves average colors and
gradients; the result is still limited by the 256-color desktop. Theme changes
do not recolor a loaded standard image. Transparency is composited onto a
source-aligned light checkerboard. BOS1 palette bytes are retained unchanged.

Not implemented: GIF animation, JPEG EXIF orientation, ICC color profiles,
color-managed or HDR display, image editing/export, TIFF, WebP, or AVIF.

## Decoder provenance and memory

The decoder is stb_image v2.30, pinned to upstream revision
`2c980bb59875b0d32144a71867fbdebb2f77cd20`, used under its MIT license. The license,
upstream hash, and complete small stack/progress patch are preserved in
[`third_party/stb`](../third_party/stb/README.baseos.md). JPEG, PNG and BMP decoder
algorithms are unchanged, with optional cooperative progress callbacks added.
GIF prefix output uses an equivalent bounded iterative routine instead of
recursion, and its state lives in the bounded arena, leaving ample stack space
for audio service while decoding an image.

`IMAGE_BASE = 0x1900000`, `IMAGE_CAPACITY = 0x700000` is a dedicated, E820-checked
7 MiB arena. Its first 786,432 bytes hold the displayed palette pixels; the rest
is a bounded, coalescing decoder workspace. The decoder's temporary pointers
never escape to the filesystem or other apps. There is no general-purpose heap.

HDR, linear-float conversion, SIMD, stdio, and thread-local storage are disabled.
The decoder uses the normal kernel `-msoft-float` build. Generated object checks
verify there are no external helper symbols or FPU/SIMD instructions. A real
QEMU test also verifies normal caller x87 control/register state survives an
open. The checked build's largest decoder frame is about 7.8 KiB (PNG); GIF's
iterative prefix expansion uses about 4.1 KiB. Palette error-diffusion rows use
about 12 KiB after decoding has returned.

Image Viewer temporarily installs `platform_poll` as the decoder's progress
hook and also polls every eight color-conversion rows. This collects input and
services audio/network work without dispatching applications or changing the
filesystem. An optional hook API is available to other synchronous callers;
the previous hook is always restored after an open, including failed opens.

The decoder remains a cooperative built-in kernel component, not a security
sandbox. The size and workspace limits are resource limits, not a claim that
arbitrary hostile files are safe to run through a kernel decoder.

## Reproducible checks

```sh
make
python3 -m unittest discover -s tests -p test_image_formats.py -v
python3 tools/image_test.py build
python3 tools/image_test.py build --maximum
python3 tools/image_test.py build --maximum --audio
python3 tools/image_test.py build --audio --audio-format mp3
```

The host suite uses AddressSanitizer and UndefinedBehaviorSanitizer with 23
ordinary, original image fixtures. Pillow generated independent reference
pixels; Pillow is not required to run the suite. Lossless formats match exactly
(unused RGB under transparent GIF pixels is ignored). JPEG output stays within
3 channel levels and below 0.15 mean channel error against the independent
reference. Valid dimension/workspace-limit examples are rejected cleanly.

The viewer host test checks source ownership, fit/actual size/zoom/pan, resizing,
offscreen clipping, theme stability, unsupported-format errors, close/reopen,
and preservation of BOS1 pixels. Leak detection can be enabled with
`ASAN_OPTIONS=detect_leaks=1`; the default disables LeakSanitizer because some
instrumented execution environments do not support its process inspection.

The QEMU test creates disposable boot and IDE images; it never reads or changes
the normal saved disk images. It checks independent reference pixels in the
32-bit guest, decodes all five formats including progressive JPEG and a large
GIF, replaces a source file after opening it, exercises controls, checks x87
state, and captures actual JPEG, PNG, transparency and viewer screenshots. It
prints its output directory. This fixture tests the real kernel viewer
client; desktop launch/file-association integration is a separate UI check.
The `--maximum` pass uses actual 1024 × 768 JPEG and PNG files in the guest.

The `--audio` check opens six maximum-size JPEG/PNG files during real 44.1 kHz
stereo SB16 playback, asserts no underrun, and checks the captured waveform
sample-for-sample. One checked run took 16–22 timer ticks per image open, with
no more than one tick between audio-service calls. This is roughly 229–314 ms
per complete image open, while audio continued normally. Timings depend on the
QEMU host. `--audio-rate 48000` additionally checks service gaps and continuity;
the original QEMU SB16 backend clamps that requested sample rate, so this mode
does not claim exact pitch/duration without the separate audio resampler.

The MP3 variant opens JPEG, PNG, and 1024 × 512 GIF files twelve times while
playing a generated 44.1 kHz MP3. It compares actual SB16 output to a host
minimp3 decode. A checked run had zero underruns, one-tick maximum service gap,
and RMS error 0.03 over 163,756 interleaved samples. This optional MP3 check
requires ffmpeg; the standard image and WAV tests do not.

Regenerate the original fixtures and their independent references, if needed:

```sh
python3 tools/make_image_examples.py
```

Only fixture regeneration requires Pillow. The artwork and reference files are
original BaseOS test material covered by the project's MIT license.
