/* Original BaseOS SB16 driver, based on Creative's Sound Blaster Series
 * Hardware Programming Guide. See docs/MEDIA.md for hardware contract. */
#include "audio.h"
#include "media_mp3.h"
#include "platform.h"

/* Kept here as fallbacks for branches integrating the shared memory map.
 * The authoritative integrated definitions belong in src/layout.h. */
#ifndef AUDIO_DMA_BASE
#define AUDIO_DMA_BASE 0x710000
#define AUDIO_DMA_CAPACITY 0x10000
#define AUDIO_WORK_BASE 0x720000
#define AUDIO_WORK_CAPACITY 0x60000
#endif
#define RING_BYTES 65536u
#define HALF_BYTES (RING_BYTES / 2)
#define PCM_SAMPLES_PER_POLL 4096u
#define SB_BASE 0x220
#define DSP_WRITE (SB_BASE + 0x0c)
#define DSP_STATUS (SB_BASE + 0x0e)
#define DSP_READ (SB_BASE + 0x0a)
#define DSP_ACK16 (SB_BASE + 0x0f)
#define IO_LIMIT 10000u

#ifndef AUDIO_HOST_TEST
static inline uint8_t audio_in(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1,%0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}
static inline void audio_out(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0,%1" :: "a"(value), "Nd"(port) : "memory");
}
#define DMA_BUFFER ((uint8_t *)AUDIO_DMA_BASE)
#define SOURCE_BUFFER ((uint8_t *)AUDIO_WORK_BASE)
#else
extern uint8_t audio_in(uint16_t port);
extern void audio_out(uint16_t port, uint8_t value);
extern uint8_t audio_test_dma[RING_BYTES], audio_test_source[AUDIO_WORK_CAPACITY];
#define DMA_BUFFER audio_test_dma
#define SOURCE_BUFFER audio_test_source
#endif

static AudioStatus status;
static MediaWave wave;
static MediaMp3 mp3;
static AudioPcmReader pcm_reader;
static void *pcm_context;
static unsigned initialized, hardware_running, ready_mask, fill_half, fill_bytes, loading_paused;
static uint32_t dma_position, last_poll, last_progress, submitted_frames, output_frames;
/* A callback stream can use the entire uint32_t frame range. Its final silent
 * drain must not wrap the hardware-consumed count back to zero. */
static uint64_t dma_frames;
static int eof, source_eof;
static int16_t pcm_stage[PCM_SAMPLES_PER_POLL];
static unsigned stage_frames, resample_fraction;
static uint32_t stage_base;
static uint64_t resample_frame;

static void clear_pcm_stream(void) { pcm_reader = 0; pcm_context = 0; }
static void reset_playback(void) {
    /* QEMU's SB16 SAMPLE_RATE_MAX is 45000, so programming 48000 directly
     * silently changes pitch and duration. All source formats share this path. */
    status.output_rate = status.sample_rate > 45000 ? 44100 : status.sample_rate;
    ready_mask = fill_half = fill_bytes = submitted_frames = output_frames = loading_paused = 0;
    dma_frames = resample_frame = 0; eof = source_eof = 0;
    stage_frames = stage_base = resample_fraction = 0;
}

static int dsp_write(uint8_t value) {
    for (unsigned i = 0; i < IO_LIMIT; ++i) {
        if (!(audio_in(DSP_WRITE) & 0x80)) { audio_out(DSP_WRITE, value); return 1; }
    }
    return 0;
}
static int dsp_read(void) {
    for (unsigned i = 0; i < IO_LIMIT; ++i)
        if (audio_in(DSP_STATUS) & 0x80) return audio_in(DSP_READ);
    return -1;
}
static void mixer_write(uint8_t reg, uint8_t value) {
    audio_out(SB_BASE + 4, reg); audio_out(SB_BASE + 5, value);
}
static void acknowledge(void) { (void)audio_in(DSP_ACK16); }
static void hardware_stop(void) {
    if (status.available && hardware_running) {
        (void)dsp_write(0xd5); /* Pause high DMA; mask prevents stray requests. */
        audio_out(0xd4, 5);
        acknowledge();
    }
    hardware_running = 0;
}
static void fail(int error) {
    hardware_stop(); clear_pcm_stream(); status.state = AUDIO_ERROR; status.error = error;
}
static void hardware_volume(void) {
    /* Both legacy mixer registers are supported by QEMU SB16. */
    unsigned nibble = (status.volume * 15 + 50) / 100;
    mixer_write(0x22, (uint8_t)((nibble << 4) | nibble));
    mixer_write(0x04, 0xff);
}
int audio_init(void) {
    if (initialized) return status.available ? MEDIA_OK : MEDIA_NO_DEVICE;
    initialized = 1;
    status = (AudioStatus){.state = AUDIO_STOPPED, .volume = 75};
    audio_out(SB_BASE + 6, 1);
    /* ISA I/O cycles provide the >=3 us reset pulse without a scheduler wait. */
    for (unsigned i = 0; i < 64; ++i) audio_out(0x80, 0);
    audio_out(SB_BASE + 6, 0);
    if (dsp_read() != 0xaa || !dsp_write(0xe1)) return MEDIA_NO_DEVICE;
    int major = dsp_read(), minor = dsp_read();
    if (major < 4 || minor < 0) return MEDIA_NO_DEVICE;
    status.available = 1;
    mixer_write(0x80, 2); /* IRQ5 stays masked: completion is polled. */
    mixer_write(0x81, 0x22); /* Low DMA1, high DMA5 (floppy uses DMA2). */
    hardware_volume();
    acknowledge();
    return MEDIA_OK;
}
static uint32_t hardware_position(void) {
    /* High DMA5 count is in words. Reset flip-flop for each stable read pair;
     * retries handle a carry between the low/high byte reads. */
    uint16_t first = 0, second = 0;
    for (unsigned i = 0; i < 4; ++i) {
        audio_out(0xd8, 0); first = audio_in(0xc6); first |= (uint16_t)audio_in(0xc6) << 8;
        audio_out(0xd8, 0); second = audio_in(0xc6); second |= (uint16_t)audio_in(0xc6) << 8;
        if ((uint16_t)(first - second) < 128) break;
    }
    return (RING_BYTES - ((uint32_t)second + 1) * 2) & (RING_BYTES - 1);
}
static int hardware_start(void) {
    audio_out(0xd4, 5); /* Mask high DMA5 while programming. */
    audio_out(0xd8, 0);
    audio_out(0xd6, 0x59); /* Single transfers, auto-init, memory to device, ch5. */
    audio_out(0xc4, (AUDIO_DMA_BASE >> 1) & 255);
    audio_out(0xc4, (AUDIO_DMA_BASE >> 9) & 255);
    audio_out(0x8b, AUDIO_DMA_BASE >> 16);
    audio_out(0xd8, 0);
    audio_out(0xc6, (RING_BYTES / 2 - 1) & 255);
    audio_out(0xc6, (RING_BYTES / 2 - 1) >> 8);
    acknowledge();
    if (!dsp_write(0x41) || !dsp_write(status.output_rate >> 8) ||
        !dsp_write(status.output_rate & 255) || !dsp_write(0xd1)) return 0;
    audio_out(0xd4, 1); /* Unmask only DMA5. */
    hardware_running = 1;
    /* Auto-init length is a word count, including both stereo channels. */
    unsigned count = HALF_BYTES / 2 - 1;
    if (!dsp_write(0xb6) || !dsp_write(status.channels == 2 ? 0x30 : 0x10) ||
        !dsp_write(count & 255) || !dsp_write(count >> 8)) return 0;
    dma_position = 0; last_poll = last_progress = timer_ticks();
    return 1;
}
void audio_stop(void) {
    hardware_stop(); clear_pcm_stream(); status.state = AUDIO_STOPPED; status.error = MEDIA_OK;
    status.played_frames = 0; ready_mask = loading_paused = 0;
}
int audio_play_wav(const void *data, uint32_t bytes) {
    MediaWave parsed;
    if (!status.available) { status.error = MEDIA_NO_DEVICE; return MEDIA_NO_DEVICE; }
    if (bytes > AUDIO_WORK_CAPACITY) { status.error = MEDIA_TOO_LARGE; return MEDIA_TOO_LARGE; }
    int error = media_wave_open(&parsed, data, bytes);
    if (error) { status.error = error; return error; }
    hardware_stop(); clear_pcm_stream();
    const uint8_t *source = data;
    for (uint32_t i = 0; i < bytes; ++i) SOURCE_BUFFER[i] = source[i];
    error = media_wave_open(&wave, SOURCE_BUFFER, bytes);
    if (error) { fail(error); return error; }
    status.state = AUDIO_LOADING; status.error = MEDIA_OK;
    status.format = AUDIO_FORMAT_WAVE; status.sample_rate = wave.sample_rate;
    status.channels = wave.channels; status.bits_per_sample = wave.bits_per_sample;
    status.total_frames = wave.frames; status.played_frames = 0;
    status.underruns = 0;
    reset_playback();
    return MEDIA_OK;
}
int audio_play(const void *data, uint32_t bytes) {
    const uint8_t *p = data;
    if (p && bytes >= 4 && p[0] == 'R' && p[1] == 'I' && p[2] == 'F' && p[3] == 'F')
        return audio_play_wav(data, bytes);
    if (!status.available) { status.error = MEDIA_NO_DEVICE; return MEDIA_NO_DEVICE; }
    if (bytes > AUDIO_WORK_CAPACITY) { status.error = MEDIA_TOO_LARGE; return MEDIA_TOO_LARGE; }
    int error = media_mp3_open(&mp3, data, bytes);
    if (error) { status.error = error; return error; }
    hardware_stop(); clear_pcm_stream();
    for (uint32_t i = 0; i < bytes; ++i) SOURCE_BUFFER[i] = p[i];
    mp3.data = SOURCE_BUFFER;
    status.state = AUDIO_LOADING; status.error = MEDIA_OK;
    status.format = AUDIO_FORMAT_MP3; status.sample_rate = mp3.sample_rate;
    status.channels = mp3.channels; status.bits_per_sample = 16;
    status.total_frames = mp3.frames; status.played_frames = 0; status.underruns = 0;
    reset_playback();
    return MEDIA_OK;
}
int audio_play_pcm_stream(unsigned rate, unsigned channels, uint32_t total_frames,
                          AudioPcmReader reader, void *context) {
    if (!status.available) { status.error = MEDIA_NO_DEVICE; return MEDIA_NO_DEVICE; }
    if ((channels != 1 && channels != 2) || rate < 5000 || rate > 48000) {
        status.error = MEDIA_UNSUPPORTED; return MEDIA_UNSUPPORTED;
    }
    if (!reader || !total_frames) { status.error = MEDIA_BAD_FILE; return MEDIA_BAD_FILE; }
    hardware_stop();
    pcm_reader = reader; pcm_context = context;
    status.state = AUDIO_LOADING; status.error = MEDIA_OK;
    status.format = AUDIO_FORMAT_STREAM; status.sample_rate = rate;
    status.channels = channels; status.bits_per_sample = 16;
    status.total_frames = total_frames; status.played_frames = 0; status.underruns = 0;
    reset_playback();
    return MEDIA_OK;
}
/* Exactly one source read/decode, including the callback's explicit EOF read.
 * MP3 can legally produce no PCM while still having compressed input left. */
static int read_source(int16_t *output, unsigned max_frames) {
    if (source_eof) return 0;
    int frames;
    if (status.format == AUDIO_FORMAT_STREAM) {
        frames = pcm_reader(pcm_context, output, max_frames);
        if (frames < 0) return frames;
        if ((unsigned)frames > max_frames ||
            (unsigned)frames > status.total_frames - submitted_frames ||
            (!frames && submitted_frames != status.total_frames)) return MEDIA_BAD_FILE;
        source_eof = !frames;
    } else if (status.format == AUDIO_FORMAT_MP3) {
        frames = media_mp3_read(&mp3, output, max_frames);
        if (frames < 0) return frames;
        source_eof = media_mp3_finished(&mp3);
    } else {
        frames = (int)media_wave_read(&wave, output, max_frames);
        source_eof = wave.frame_cursor == wave.frames;
    }
    submitted_frames += (unsigned)frames;
    if (source_eof && status.format != AUDIO_FORMAT_STREAM) status.total_frames = submitted_frames;
    return frames;
}
static int resample_step(int16_t *output, unsigned max_frames) {
    /* Keep a one-frame interpolation lookahead across polls and DMA halves.
     * Compact first, then make at most one bounded source read into free space. */
    unsigned discard = resample_frame - stage_base < stage_frames ?
        (unsigned)(resample_frame - stage_base) : stage_frames;
    stage_frames -= discard; stage_base += discard;
    for (unsigned i = 0; i < stage_frames * status.channels; ++i)
        pcm_stage[i] = pcm_stage[i + discard * status.channels];
    unsigned room = PCM_SAMPLES_PER_POLL / status.channels - stage_frames;
    if (!source_eof && room) {
        int frames = read_source(pcm_stage + stage_frames * status.channels, room);
        if (frames < 0) return frames;
        stage_frames += (unsigned)frames;
    }
    unsigned frames = 0;
    while (frames < max_frames && resample_frame < submitted_frames) {
        unsigned index = (unsigned)(resample_frame - stage_base);
        if (index >= stage_frames || (index + 1 == stage_frames && !source_eof)) break;
        unsigned next = index + 1 < stage_frames ? index + 1 : index;
        for (unsigned c = 0; c < status.channels; ++c) {
            int first = pcm_stage[index * status.channels + c];
            int second = pcm_stage[next * status.channels + c];
            output[frames * status.channels + c] = (int16_t)(
                (first * (int)(status.output_rate - resample_fraction) +
                 second * (int)resample_fraction) / (int)status.output_rate);
        }
        ++frames;
        resample_fraction += status.sample_rate;
        resample_frame += resample_fraction / status.output_rate;
        resample_fraction %= status.output_rate;
    }
    eof = source_eof && resample_frame >= submitted_frames;
    return (int)frames;
}
static void fill_step(void) {
    if (ready_mask == 3) return;
    unsigned remaining_samples = (HALF_BYTES - fill_bytes) / 2;
    unsigned budget = remaining_samples < PCM_SAMPLES_PER_POLL ? remaining_samples : PCM_SAMPLES_PER_POLL;
    int16_t *output = (int16_t *)(DMA_BUFFER + fill_half * HALF_BYTES + fill_bytes);
    int frames;
    if (status.output_rate != status.sample_rate) frames = resample_step(output, budget / status.channels);
    else {
        frames = read_source(output, budget / status.channels);
        eof = source_eof;
    }
    if (frames < 0) { fail(frames); return; }
    output_frames += (unsigned)frames;
    unsigned samples = (unsigned)frames * status.channels;
    if (eof) for (; samples < budget; ++samples) output[samples] = 0;
    fill_bytes += samples * 2;
    if (fill_bytes == HALF_BYTES) {
        ready_mask |= 1u << fill_half;
        fill_half ^= 1; fill_bytes = 0;
    }
}
void audio_poll(void) {
    if (status.state == AUDIO_LOADING) {
        fill_step();
        if (status.state == AUDIO_ERROR) return;
        if (ready_mask == 3) {
            if (!hardware_start()) { fail(MEDIA_DEVICE_ERROR); return; }
            status.state = AUDIO_PLAYING;
        }
        return;
    }
    if (status.state != AUDIO_PLAYING) return;
    uint32_t now = timer_ticks();
    /* A full ring may wrap to its old position after a long blocking call.
     * Detect that ambiguity using elapsed time, and never replay stale music. */
    unsigned ring_ticks = RING_BYTES * TIMER_HZ / (status.output_rate * status.channels * 2);
    if (now - last_poll >= ring_ticks) {
        ++status.underruns; fail(MEDIA_UNDERRUN); return;
    }
    last_poll = now;
    uint32_t position = hardware_position();
    uint32_t delta = (position - dma_position) & (RING_BYTES - 1);
    if (delta) last_progress = now;
    else if (now - last_progress > TIMER_HZ) { fail(MEDIA_DEVICE_ERROR); return; }
    dma_frames += delta / (status.channels * 2);
    if (eof && dma_frames >= output_frames) status.played_frames = submitted_frames;
    else {
        uint32_t consumed = (uint32_t)dma_frames;
        /* Split the ratio to retain full source-frame range without 64-bit
         * division helpers in the freestanding i386 kernel. */
        status.played_frames = (consumed / status.output_rate) * status.sample_rate +
            (consumed % status.output_rate) * status.sample_rate / status.output_rate;
    }
    /* Let the device/backend drain its final PCM before disabling the voice.
     * The remainder of the ring is zero-filled, so this adds only silence. */
    if (eof && dma_frames >= (uint64_t)output_frames + status.output_rate / 20) {
        status.played_frames = submitted_frames;
        hardware_stop(); clear_pcm_stream(); status.state = AUDIO_FINISHED; return;
    }
    unsigned previous_half = dma_position / HALF_BYTES, current_half = position / HALF_BYTES;
    if (previous_half != current_half) {
        if (!(ready_mask & (1u << current_half))) {
            ++status.underruns; fail(MEDIA_UNDERRUN); return;
        }
        ready_mask &= ~(1u << previous_half);
        fill_half = previous_half; fill_bytes = 0;
        acknowledge();
    }
    dma_position = position;
    fill_step();
}
void audio_pause(int paused) {
    if (paused && status.state == AUDIO_LOADING) {
        loading_paused = 1; status.state = AUDIO_PAUSED;
    } else if (paused && status.state == AUDIO_PLAYING) {
        audio_poll();
        if (status.state != AUDIO_PLAYING) return;
        if (!dsp_write(0xd5)) { fail(MEDIA_DEVICE_ERROR); return; }
        status.state = AUDIO_PAUSED;
    } else if (!paused && status.state == AUDIO_PAUSED) {
        if (loading_paused) {
            loading_paused = 0; status.state = AUDIO_LOADING; return;
        }
        if (!dsp_write(0xd6)) { fail(MEDIA_DEVICE_ERROR); return; }
        last_poll = last_progress = timer_ticks(); status.state = AUDIO_PLAYING;
    }
}
void audio_set_volume(unsigned percent) {
    status.volume = percent > 100 ? 100 : percent;
    if (status.available) hardware_volume();
}
uint32_t audio_capacity_bytes(void) { return AUDIO_WORK_CAPACITY; }
void audio_clear_error(void) {
    if (status.state == AUDIO_ERROR) audio_stop();
    else status.error = MEDIA_OK;
}
const AudioStatus *audio_status(void) { return &status; }
uint32_t audio_position_ms(void) {
    return status.sample_rate ? (status.played_frames / status.sample_rate) * 1000 +
        (status.played_frames % status.sample_rate) * 1000 / status.sample_rate : 0;
}
uint32_t audio_duration_ms(void) {
    return status.sample_rate ? (status.total_frames / status.sample_rate) * 1000 +
        (status.total_frames % status.sample_rate) * 1000 / status.sample_rate : 0;
}
_Static_assert(AUDIO_DMA_BASE % 65536 == 0 && AUDIO_DMA_CAPACITY >= RING_BYTES &&
               AUDIO_DMA_BASE + AUDIO_DMA_CAPACITY <= 0x1000000, "invalid audio ISA DMA arena");
_Static_assert(DMA_BASE + DMA_CAPACITY <= AUDIO_DMA_BASE, "floppy/audio DMA overlap");
_Static_assert(AUDIO_DMA_BASE + AUDIO_DMA_CAPACITY <= AUDIO_WORK_BASE &&
               AUDIO_WORK_BASE + AUDIO_WORK_CAPACITY <= FS_IMG_BASE, "audio arena overlap");
