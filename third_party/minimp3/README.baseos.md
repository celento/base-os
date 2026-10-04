# minimp3 in BaseOS

Upstream: https://github.com/lieff/minimp3
Pinned revision: ea99364f61c14656440e8d77e9c233ccf3124633
Files: unmodified minimp3.h and LICENSE. License: CC0 1.0 Universal.

BaseOS builds the scalar MP3-only decoder (MINIMP3_NO_SIMD and
MINIMP3_ONLY_MP3) in src/media_mp3.c. The freestanding compatibility headers
replace unused hosted stdlib declarations and route the three memory primitives
to private allocation-free routines. No decoder algorithm is modified.
The x87 translation unit saves/restores the FPU state for each decoded frame;
the rest of the kernel remains soft-float and requires no SSE.
