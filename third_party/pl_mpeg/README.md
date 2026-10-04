# pl_mpeg in BaseOS

Upstream: https://github.com/phoboslab/pl_mpeg
Pinned revision: `c871f2be022ece7ef4f64230b4fb8e1fb9eb6023`
Author: Dominic Szablewski / https://phoboslab.org
License: MIT, as declared by the upstream header's SPDX identifier and README.
The standard license text is reproduced in `LICENSE`.

`pl_mpeg.h` is the upstream header with exactly two local changes, recorded in
`BASEOS-PATCHES.diff`:

1. Do not discard/memmove bytes from a fixed-memory buffer. The upstream video
   path called discard unconditionally, making EOF detection inconsistent and
   copying the remaining elementary stream on every decoded frame.
2. Flush the delayed reference picture at EOF even when the last coded picture
   was a B picture. The original condition lost the final presentation frame.

The wrapper also appends a decoder-local sequence-end code plus four zero guard
bytes to a complete extracted elementary stream. Ordinary MPEG-PS files may
legally omit an explicit video sequence end. The guard satisfies the decoder's
VLC lookahead and prevents a partially decoded final block. It never changes the
original source MPEG-PS data.

The untouched upstream header's SHA-256 is
`3a8cb30c83c2a1147719c30fe0c8b93da2987aa43140077c575b39aaa75fc2c9`.

BaseOS calls only the low-level video and fixed-memory buffer constructors.
Private memory primitives and a statically bounded arena replace hosted C
allocation; the decoder requests no allocations while playing frames. The
translation unit uses x87, saving/restoring complete x87 state and CR0 around
constructor/decode calls. No SSE, operating-system library or host media player
is used by the guest. The MPEG-PS framing adapter, timing, palette conversion,
window rendering and Media Player integration are original BaseOS code.

The library includes an MP2 decoder, but this milestone uses video only. Audio
packets are detected and skipped, and the player visibly says "no audio".

Verification: `tests/test_video.py` compares all Y/Cb/Cr planes in every output
frame against independently decoded FFmpeg references. `tools/video_test.py`
checks all guest frames, real keyboard transport, x87 preservation, responsive
desktop input, and screenshot capture in QEMU.
