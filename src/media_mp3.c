#include "media.h"
#include "media_mp3.h"
#include <stddef.h>

static void *mp3_copy(void *to, const void *from, size_t n) {
    uint8_t *d = to; const uint8_t *s = from;
    for (size_t i = 0; i < n; ++i) d[i] = s[i];
    return to;
}
static void *mp3_move(void *to, const void *from, size_t n) {
    uint8_t *d = to; const uint8_t *s = from;
    if ((uintptr_t)d < (uintptr_t)s) return mp3_copy(to, from, n);
    while (n) { --n; d[n] = s[n]; }
    return to;
}
static void *mp3_zero(void *to, int value, size_t n) {
    uint8_t *d = to;
    for (size_t i = 0; i < n; ++i) d[i] = (uint8_t)value;
    return to;
}
#define memcpy mp3_copy
#define memmove mp3_move
#define memset mp3_zero
#define MINIMP3_NO_SIMD
#define MINIMP3_ONLY_MP3
#define MINIMP3_IMPLEMENTATION
#include "../third_party/minimp3/minimp3.h"
#undef memcpy
#undef memmove
#undef memset

/* Each call owns the x87 only while decoding one frame. It neither leaks
 * decoder registers to a native program nor inherits that program's rounding
 * mode or pending exceptions. The PIT interrupt handler does not use the FPU. */
#ifndef MEDIA_MP3_HOST_TEST
typedef struct { uint8_t bytes[108]; } FpuState;
static unsigned fpu_begin(FpuState *saved) {
    unsigned previous;
    __asm__ volatile("mov %%cr0,%0" : "=r"(previous));
    unsigned enabled = (previous & ~12u) | 2u;
    __asm__ volatile("mov %0,%%cr0" :: "r"(enabled) : "memory");
    __asm__ volatile("fnsave %0\n\tfninit" : "=m"(*saved) :: "memory");
    return previous;
}
static void fpu_end(const FpuState *saved, unsigned previous) {
    __asm__ volatile("frstor %0" :: "m"(*saved) : "memory");
    __asm__ volatile("mov %0,%%cr0" :: "r"(previous) : "memory");
}
static int have_fpu(void) {
    unsigned a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
    return (d & 1) != 0;
}
#endif

static int frame_info(const uint8_t *h, unsigned *rate, unsigned *channels,
                      unsigned *frames, unsigned *bytes) {
    static const unsigned rates[3] = {44100, 48000, 32000};
    static const unsigned kbps[2][15] = {
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
        {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}
    };
    unsigned version = (h[1] >> 3) & 3, bit_rate = h[2] >> 4, sample_rate = (h[2] >> 2) & 3;
    if (h[0] != 255 || (h[1] & 0xe0) != 0xe0 || (h[1] & 6) != 2 ||
        version == 1 || !bit_rate || bit_rate == 15 || sample_rate == 3) return 0;
    *rate = rates[sample_rate] >> (version == 3 ? 0 : version == 2 ? 1 : 2);
    *channels = (h[3] & 0xc0) == 0xc0 ? 1 : 2;
    *frames = version == 3 ? 1152 : 576;
    *bytes = (version == 3 ? 144000 : 72000) * kbps[version == 3][bit_rate] / *rate + ((h[2] >> 1) & 1);
    return 1;
}
int media_mp3_open(MediaMp3 *mp3, const void *file, uint32_t bytes) {
    if (!mp3 || !file || bytes < 4) return MEDIA_BAD_FILE;
#ifndef MEDIA_MP3_HOST_TEST
    if (!have_fpu()) return MEDIA_UNSUPPORTED;
#endif
    const uint8_t *p = file;
    uint32_t cursor = 0, total = 0;
    if (bytes >= 10 && p[0] == 'I' && p[1] == 'D' && p[2] == '3') {
        if (p[3] < 2 || p[3] > 4 || ((p[6] | p[7] | p[8] | p[9]) & 0x80)) return MEDIA_BAD_FILE;
        uint32_t tag_bytes = (uint32_t)p[6] << 21 | (uint32_t)p[7] << 14 | (uint32_t)p[8] << 7 | p[9];
        if (tag_bytes > bytes - 10) return MEDIA_BAD_FILE;
        cursor = 10 + tag_bytes;
        if (p[3] == 4 && (p[5] & 0x10)) {
            if (bytes - cursor < 10) return MEDIA_BAD_FILE;
            cursor += 10;
        }
    }
    uint32_t first = cursor, end = bytes;
    if (end >= 128 && p[end-128]=='T' && p[end-127]=='A' && p[end-126]=='G') end -= 128;
    unsigned initial_rate = 0, initial_channels = 0;
    while (cursor < end) {
        unsigned rate, channels, frames, frame_bytes;
        if (end - cursor < 4 || !frame_info(p + cursor, &rate, &channels, &frames, &frame_bytes) ||
            frame_bytes > end - cursor) return MEDIA_BAD_FILE;
        if (!initial_rate) { initial_rate = rate; initial_channels = channels; }
        if (rate != initial_rate || channels != initial_channels) return MEDIA_UNSUPPORTED;
        if (UINT32_MAX - total < frames) return MEDIA_TOO_LARGE;
        total += frames; cursor += frame_bytes;
    }
    if (!total) return MEDIA_BAD_FILE;
    mp3_zero(mp3, 0, sizeof(*mp3));
    mp3->data = p; mp3->bytes = end; mp3->cursor = first;
    mp3->sample_rate = initial_rate; mp3->channels = initial_channels; mp3->frames = total;
    mp3dec_init(&mp3->decoder);
    return MEDIA_OK;
}
int media_mp3_finished(const MediaMp3 *mp3) {
    return mp3->cursor >= mp3->bytes && mp3->pcm_cursor >= mp3->pcm_frames;
}
int media_mp3_read(MediaMp3 *mp3, int16_t *output, unsigned max_frames) {
    if (!mp3 || !output || !max_frames) return 0;
    if (mp3->pcm_cursor >= mp3->pcm_frames) {
        if (mp3->cursor >= mp3->bytes) return 0;
        mp3dec_frame_info_t info;
#ifndef MEDIA_MP3_HOST_TEST
        FpuState saved;
        unsigned previous = fpu_begin(&saved);
#endif
        int frames = mp3dec_decode_frame(&mp3->decoder, mp3->data + mp3->cursor,
            (int)(mp3->bytes - mp3->cursor), mp3->pcm, &info);
#ifndef MEDIA_MP3_HOST_TEST
        fpu_end(&saved, previous);
#endif
        if (info.frame_bytes <= 0 || (unsigned)info.frame_bytes > mp3->bytes - mp3->cursor ||
            frames < 0 || frames > 1152) return MEDIA_BAD_FILE;
        if (frames && ((unsigned)info.hz != mp3->sample_rate || (unsigned)info.channels != mp3->channels))
            return MEDIA_UNSUPPORTED;
        mp3->cursor += (unsigned)info.frame_bytes;
        mp3->pcm_cursor = 0; mp3->pcm_frames = (unsigned)frames;
        mp3->decoded_frames += (unsigned)frames;
    }
    unsigned frames = mp3->pcm_frames - mp3->pcm_cursor;
    if (frames > max_frames) frames = max_frames;
    mp3_copy(output, mp3->pcm + mp3->pcm_cursor * mp3->channels, frames * mp3->channels * 2);
    mp3->pcm_cursor += frames;
    return (int)frames;
}
