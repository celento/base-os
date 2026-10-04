# stb_image in BaseOS

Upstream: https://github.com/nothings/stb

Pinned revision: `2c980bb59875b0d32144a71867fbdebb2f77cd20`

Files: `stb_image.h` v2.30 with documented BaseOS stack/progress adaptations, and
unmodified `LICENSE`. SHA-256 of the upstream `stb_image.h` before that patch:
`594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3`.

`baseos.patch` is the complete difference from that pinned upstream file:

- Recursive GIF LZW prefix output becomes equivalent bounded iterative
  expansion (a 4096-byte suffix array), propagating invalid-prefix failure.
- The first-frame GIF state moves from the kernel stack into the same bounded
  allocator used for image pixels. Its temporary buffers are still freed.
- A default-no-op progress macro marks bounded byte/row/conversion work. The
  BaseOS wrapper services an optional callback, preserving audio playback while
  opening larger images. It never dispatches app actions or mutates the source.

These changes leave room for cooperative MP3 polling within the 64 KiB stack.
SHA-256 of the resulting vendored header:
`3cd095a09eb308e888db7fc3263dfb0ff3248854e2804b2326930ab636642e19`.

stb offers an MIT or public-domain dual license. BaseOS uses the MIT option.
The copyright and complete license are preserved in both upstream files.

`src/image_decode.c` enables JPEG, PNG, BMP, and the first GIF frame. HDR,
linear floating-point conversion, SIMD, stdio, and thread-local storage are
disabled. JPEG's scalar IDCT and color conversion are integer-only. The wrapper
and decoder are compiled with the same `-msoft-float` options as the kernel and
do not touch the native-program x87 state.

Allocation is redirected to a fixed, bounded, coalescing workspace. There is no
hosted libc or general-purpose kernel heap. JPEG, PNG and BMP decoding algorithms
are unchanged apart from progress callbacks; GIF uses the equivalent iterative
prefix-output implementation and arena-owned state above.
The compatibility headers apply to this translation unit only.

The decoder is a compatibility feature for ordinary imported files, not a
security boundary: it runs cooperatively in the kernel, as BaseOS's built-in
apps do. File size, dimensions, pixel count, and total workspace are bounded;
some otherwise valid high-memory variants may report a clear workspace error.
GIF animation, JPEG EXIF orientation, and ICC color management are not provided.
