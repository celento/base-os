#ifndef AUDIO_H
#define AUDIO_H
#include <stdint.h>
#include "media.h"

enum { AUDIO_STOPPED, AUDIO_LOADING, AUDIO_PLAYING, AUDIO_PAUSED, AUDIO_FINISHED, AUDIO_ERROR };
enum { AUDIO_FORMAT_NONE, AUDIO_FORMAT_WAVE, AUDIO_FORMAT_MP3, AUDIO_FORMAT_STREAM };
typedef int (*AudioPcmReader)(void *context, int16_t *output, unsigned max_frames);
typedef struct {
    int state, error, available, format;
    unsigned sample_rate, channels, bits_per_sample;
    /* Source frame/time fields retain sample_rate; output_rate is the SB16 rate.
     * Sources above QEMU's 45 kHz ceiling are resampled to 44.1 kHz. */
    unsigned output_rate;
    uint32_t total_frames, played_frames;
    unsigned volume, underruns;
} AudioStatus;

/* QEMU SB16 at 0x220, IRQ5 masked/polled, high DMA5. No heap or IRQ changes.
 * Initialize once after platform memory validation. Missing hardware is safe. */
int audio_init(void);
/* Validates and takes an owned copy; caller can edit/delete the source file.
 * Work is scheduled by audio_poll, not by a blocking playback loop. */
int audio_play(const void *data, uint32_t bytes);
int audio_play_wav(const void *data, uint32_t bytes);
/* Stream interleaved signed 16-bit PCM at 5--48 kHz, mono/stereo. The reader
 * returns up to max_frames (short reads are allowed), 0 at EOF, or a negative
 * MEDIA error. EOF must agree with nonzero total_frames, otherwise playback
 * fails with MEDIA_BAD_FILE. Arguments are checked before replacing playback.
 * The caller owns context until stop, replacement, finish, or error. The reader
 * must not reenter audio functions; each audio_poll calls it at most once. */
int audio_play_pcm_stream(unsigned rate, unsigned channels, uint32_t total_frames,
                          AudioPcmReader reader, void *context);
/* Run from main loop and platform_poll. Never call from an interrupt handler.
 * Every call reads at most 4096 source PCM samples, decodes one MP3 frame, or
 * calls the PCM reader once for at most 4096 samples. Resampling also writes
 * at most 4096 output samples using fixed staging. Service at the normal
 * 70-Hz PIT cadence, plus hooks during long rendering/I/O. A 48-kHz MPEG
 * frame supplies only 24 ms of PCM; a brief 70-ms gap is not a steady cadence.
 * The 64 KiB ring tolerates ordinary bounded framebuffer/disk work. */
void audio_poll(void);
/* Pausing also suspends initial prefill; resume continues that loading phase. */
void audio_pause(int paused);
void audio_stop(void);
void audio_set_volume(unsigned percent);
const AudioStatus *audio_status(void);
uint32_t audio_capacity_bytes(void);
void audio_clear_error(void);
uint32_t audio_position_ms(void);
uint32_t audio_duration_ms(void);
#endif
