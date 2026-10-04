# stb_image in BaseOS

Upstream: https://github.com/nothings/stb

Pinned revision: `2c980bb59875b0d32144a71867fbdebb2f77cd20`

Files: unmodified `stb_image.h` v2.30 and `LICENSE`.
SHA-256 of `stb_image.h`:
`594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3`.

stb offers an MIT or public-domain dual license. BaseOS uses the MIT option.
The copyright and complete license are preserved in both upstream files.

`src/image_decode.c` enables JPEG, PNG, BMP, and the first GIF frame. HDR,
linear floating-point conversion, SIMD, stdio, and thread-local storage are
disabled. JPEG's scalar IDCT and color conversion are integer-only. The wrapper
and decoder are compiled with the same `-msoft-float` options as the kernel and
do not touch the native-program x87 state.

Allocation is redirected to a fixed, bounded, coalescing workspace. There is no
hosted libc, general-purpose kernel heap, or decoder algorithm modification.
The compatibility headers apply to this translation unit only.

The decoder is a compatibility feature for ordinary imported files, not a
security boundary: it runs cooperatively in the kernel, as BaseOS's built-in
apps do. File size, dimensions, pixel count, and total workspace are bounded;
some otherwise valid high-memory variants may report a clear workspace error.
GIF animation, JPEG EXIF orientation, and ICC color management are not provided.
