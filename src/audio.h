#ifndef AUDIO_H
#define AUDIO_H
#include <stdint.h>
#include "media.h"

enum { AUDIO_STOPPED, AUDIO_LOADING, AUDIO_PLAYING, AUDIO_PAUSED, AUDIO_FINISHED, AUDIO_ERROR };
enum { AUDIO_FORMAT_NONE, AUDIO_FORMAT_WAVE, AUDIO_FORMAT_MP3 };
typedef struct {
    int state, error, available, format;
    unsigned sample_rate, channels, bits_per_sample;
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
/* Run from main loop and platform_poll. Never call from an interrupt handler.
 * Every call converts at most 4096 PCM samples or decodes one MP3 frame. Poll every <= 70 ms while
 * playing; the 64 KiB ring also tolerates ordinary framebuffer/disk work. */
void audio_poll(void);
void audio_pause(int paused);
void audio_stop(void);
void audio_set_volume(unsigned percent);
const AudioStatus *audio_status(void);
uint32_t audio_capacity_bytes(void);
void audio_clear_error(void);
uint32_t audio_position_ms(void);
uint32_t audio_duration_ms(void);
#endif
