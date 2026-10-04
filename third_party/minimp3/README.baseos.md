# minimp3 in BaseOS

Upstream: https://github.com/lieff/minimp3
Pinned revision: ea99364f61c14656440e8d77e9c233ccf3124633
Files: unmodified minimp3.h and LICENSE. License: CC0 1.0 Universal.

BaseOS builds the scalar decoder (MINIMP3_NO_SIMD) in src/media_mp3.c.
The MP3 file adapter accepts Layer III; src/video.c also uses the unmodified
Layer II implementation for MPEG-1 program-stream audio. The freestanding compatibility headers
replace unused hosted stdlib declarations and route the three memory primitives
to private allocation-free routines. No decoder algorithm is modified.
The adapters save/restore the FPU state for each decoded frame;
the rest of the kernel remains soft-float and requires no SSE.
