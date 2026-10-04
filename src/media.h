#ifndef MEDIA_H
#define MEDIA_H
#include <stdint.h>

/* Allocation-free, byte-oriented RIFF/WAVE reader. Input remains owned by caller. */
typedef struct {
    const uint8_t *data;
    uint32_t data_bytes, frames, sample_rate, frame_cursor;
    uint16_t channels, bits_per_sample, block_align;
} MediaWave;

enum {
    MEDIA_OK = 0,
    MEDIA_BAD_FILE = -1,
    MEDIA_UNSUPPORTED = -2,
    MEDIA_TOO_LARGE = -3,
    MEDIA_NO_DEVICE = -4,
    MEDIA_DEVICE_ERROR = -5,
    MEDIA_UNDERRUN = -6
};

/* Accepts PCM RIFF/WAVE, 8/16-bit mono/stereo, 5--48 kHz. Unknown RIFF chunks
 * and their required odd-byte padding are skipped. Rejects inconsistent or
 * truncated containers rather than reading beyond the supplied byte count. */
int media_wave_open(MediaWave *wave, const void *file, uint32_t bytes);
/* Converts at most max_frames to interleaved signed 16-bit PCM. */
uint32_t media_wave_read(MediaWave *wave, int16_t *output, uint32_t max_frames);
const char *media_error_string(int error);
#endif
