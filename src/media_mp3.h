#ifndef MEDIA_MP3_H
#define MEDIA_MP3_H
#include <stdint.h>
#include "../third_party/minimp3/minimp3.h"

typedef struct {
    mp3dec_t decoder;
    const uint8_t *data;
    uint32_t bytes, cursor, frames, decoded_frames;
    unsigned sample_rate, channels, pcm_frames, pcm_cursor;
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
} MediaMp3;

/* MPEG-1/2/2.5 Layer III, CBR/VBR, mono/stereo; ID3v2 and ID3v1 accepted.
 * No heap; open validates frame boundaries and reads metadata without decoding. */
int media_mp3_open(MediaMp3 *mp3, const void *data, uint32_t bytes);
/* At most one compressed frame is decoded per call (up to 1152 frames).
 * Zero may mean a skipped reservoir frame; use media_mp3_finished for EOF. */
int media_mp3_read(MediaMp3 *mp3, int16_t *output, unsigned max_frames);
int media_mp3_finished(const MediaMp3 *mp3);
#endif
