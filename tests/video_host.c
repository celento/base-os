/* Host exercise of the real cooperative MPEG-1 engine with a virtual PIT.
 * Inputs are ordinary, locally generated valid program streams. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "video.h"
#include "audio.h"
#include "platform.h"

uint8_t video_test_arena[VIDEO_CAPACITY];
static uint32_t now;
static unsigned source_bytes;
uint32_t timer_ticks(void) { return now; }
static AudioStatus no_audio;
const AudioStatus *audio_status(void) { return &no_audio; }
void audio_stop(void) {}
void audio_pause(int paused) { (void)paused; }
uint32_t audio_position_ms(void) { return 0; }
uint32_t audio_duration_ms(void) { return 0; }
int audio_play_pcm_stream(unsigned rate,unsigned channels,uint32_t total,
                          AudioPcmReader reader,void *context) {
    (void)rate;(void)channels;(void)total;(void)reader;(void)context;
    return MEDIA_NO_DEVICE;
}


static unsigned number(const char *s) {
    char *end;
    unsigned long value = strtoul(s, &end, 10);
    assert(s[0] && !*end && value <= UINT32_MAX);
    return (unsigned)value;
}

static void play_owned(const char *path) {
    FILE *input = fopen(path, "rb");
    assert(input);
    assert(!fseek(input, 0, SEEK_END));
    long size = ftell(input);
    assert(size > 0 && size <= VIDEO_MAX_FILE_BYTES);source_bytes=(unsigned)size;
    rewind(input);
    uint8_t *data = malloc((size_t)size);
    assert(data && fread(data, 1, (size_t)size, input) == (size_t)size);
    fclose(input);
    assert(video_play(data, (uint32_t)size) == MEDIA_OK);
    /* Neither playback nor asynchronous header parsing may borrow the input. */
    memset(data, 0xdd, (size_t)size);
    free(data);
    assert(video_status()->state == VIDEO_LOADING);
}

static void check_plane(const uint8_t *p, unsigned stride, unsigned w, unsigned h) {
    uintptr_t begin = (uintptr_t)video_test_arena;
    uintptr_t end = begin + sizeof(video_test_arena);
    assert(p && stride >= w && w && h);
    assert((uintptr_t)p >= begin && (uintptr_t)p < end);
    assert((uintptr_t)p + (uintptr_t)(h - 1) * stride + w <= end);
}

static unsigned frame_hash(const VideoFrame *f) {
    assert(f);
    const uint8_t *planes[3] = {f->y, f->cb, f->cr};
    unsigned hash = 2166136261u;
    for (unsigned c = 0; c < 3; ++c) {
        unsigned width = f->width >> (c != 0), height = f->height >> (c != 0);
        unsigned stride = c ? f->chroma_stride : f->y_stride;
        check_plane(planes[c], stride, width, height);
        for (unsigned y = 0; y < height; ++y)
            for (unsigned x = 0; x < width; ++x)
                hash = (hash ^ planes[c][y * stride + x]) * 16777619u;
    }
    return hash;
}

static void output_frame(FILE *output, unsigned width, unsigned height, unsigned count) {
    const VideoFrame *f = video_frame();
    assert(f && f->number == count && f->width == width && f->height == height);
    const uint8_t *planes[3] = {f->y, f->cb, f->cr};
    for (unsigned c = 0; c < 3; ++c) {
        unsigned w = width >> (c != 0), h = height >> (c != 0);
        unsigned stride = c ? f->chroma_stride : f->y_stride;
        check_plane(planes[c], stride, w, h);
        for (unsigned y = 0; y < h; ++y)
            assert(fwrite(planes[c] + y * stride, 1, w, output) == w);
    }
}

static void check_loading_pause(void) {
    assert(video_status()->state == VIDEO_LOADING);
    video_pause(1);
    for (unsigned i = 0; i < TIMER_HZ; ++i) {
        ++now;
        video_poll();
        assert(video_status()->state == VIDEO_PAUSED);
        assert(video_status()->displayed_frames == 0 && video_position_ms() == 0);
    }
    video_pause(0);
    assert(video_status()->state == VIDEO_LOADING);
}

static void check_pause(void) {
    unsigned frames = video_status()->displayed_frames;
    unsigned position = video_position_ms();
    unsigned hash = frame_hash(video_frame());
    video_pause(1);
    assert(video_status()->state == VIDEO_PAUSED);
    for (unsigned i = 0; i < 3 * TIMER_HZ; ++i) {
        ++now;
        video_poll();
        assert(video_status()->state == VIDEO_PAUSED);
        assert(video_status()->displayed_frames == frames);
        assert(video_position_ms() == position);
        assert(frame_hash(video_frame()) == hash);
    }
    video_pause(0);
    assert(video_status()->state == VIDEO_PLAYING);
    /* Resume must preserve the current presentation until the next due frame. */
    assert(video_status()->displayed_frames == frames);
    assert(video_position_ms() == position);
    assert(frame_hash(video_frame()) == hash);
}

static void stop_and_replay(const char *path) {
    video_stop();
    assert(video_status()->state == VIDEO_STOPPED);
    for (unsigned pass = 0; pass < 3; ++pass) {
        play_owned(path);
        unsigned polls = 0;
        while (video_status()->displayed_frames < 4) {
            if (video_status()->state == VIDEO_PLAYING) ++now;
            video_poll();
            assert(++polls < 10000 && video_status()->state != VIDEO_ERROR);
        }
        video_stop();
        assert(video_status()->state == VIDEO_STOPPED);
        unsigned count = video_status()->displayed_frames;
        unsigned position = video_position_ms();
        now += TIMER_HZ;
        for (unsigned i = 0; i < 20; ++i) video_poll();
        assert(video_status()->state == VIDEO_STOPPED);
        assert(video_status()->displayed_frames == count && video_position_ms() == position);
    }
    video_clear_error();
    assert(video_status()->error == MEDIA_OK);
}

static int reject_valid_unsupported(const char *path) {
    FILE *input = fopen(path, "rb");
    assert(input && !fseek(input, 0, SEEK_END));
    long size = ftell(input);
    assert(size > 0 && size <= VIDEO_MAX_FILE_BYTES);source_bytes=(unsigned)size;
    rewind(input);
    uint8_t *data = malloc((size_t)size);
    assert(data && fread(data, 1, (size_t)size, input) == (size_t)size);
    fclose(input);
    int result = video_play(data, (uint32_t)size);
    memset(data, 0xdd, (size_t)size);
    free(data);
    if (result == MEDIA_OK) {
        unsigned polls = 0;
        while (video_status()->state == VIDEO_LOADING) {
            video_poll();
            assert(++polls < 10000);
            assert(video_status()->displayed_frames == 0);
        }
        assert(video_status()->state == VIDEO_ERROR);
    } else assert(result == MEDIA_UNSUPPORTED);
    assert(video_status()->error == MEDIA_UNSUPPORTED);
    assert(video_status()->arena_bytes <= VIDEO_CAPACITY);
    video_clear_error();
    assert(video_status()->error == MEDIA_OK);
    puts("Valid unsupported media rejected cleanly.");
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--reject"))
        return reject_valid_unsupported(argv[2]);
    assert(argc == 9 || argc == 11);
    unsigned width = number(argv[3]), height = number(argv[4]);
    unsigned fps_num = number(argv[5]), fps_den = number(argv[6]);
    unsigned expected = number(argv[7]), mode = number(argv[8]);
    unsigned controls = mode & 1, audio = (mode >> 1) & 1;
    if (mode & 4) now = UINT32_MAX - 5; /* Legal PIT counter rollover. */
    assert(width && height && fps_num && fps_den && expected);
    FILE *output = fopen(argv[2], "wb");
    assert(output);
    play_owned(argv[1]);
    if (controls) check_loading_pause();
    unsigned count = 0, polls = 0, loading_polls = 0, first_tick = 0, paused = 0, late = 0;
    unsigned previous_position = 0, max_arena = 0;
    while (video_status()->state != VIDEO_FINISHED) {
        const VideoStatus *s = video_status();
        assert(s->state == VIDEO_LOADING || s->state == VIDEO_PLAYING);
        if (s->state==VIDEO_LOADING) ++loading_polls;
        if (controls && count >= 10 && !paused) {
            check_pause();
            paused = 1;
        }
        if (controls && count >= 30 && !late) {
            /* An ordinary stalled desktop must not decode an unbounded backlog. */
            now += TIMER_HZ * 2;
            late = 1;
        } else if (s->state == VIDEO_PLAYING) {
            ++now;
        }
        video_poll();
        s = video_status();
        if (s->state == VIDEO_ERROR)
            fprintf(stderr, "video error %d: %s after %u frames\n",
                    s->error, video_error_string(s->error), count);
        assert(s->state != VIDEO_ERROR && s->error == MEDIA_OK);
        assert(++polls < 100000);
        assert(s->arena_bytes <= VIDEO_CAPACITY);
        if (s->arena_bytes > max_arena) max_arena = s->arena_bytes;
        assert(s->displayed_frames >= count && s->displayed_frames <= count + 1);
        if (s->state != VIDEO_LOADING) {
            /* Ordinary 2 KiB MPEG-PS packets must be batched. One packet per
             * 70-Hz desktop turn made a valid 1.6 MiB clip time out on open. */
            assert(loading_polls<=source_bytes/16384u+32u);
            assert(s->width == width && s->height == height);
            if (argc==11) assert(s->aspect_num*number(argv[10])==s->aspect_den*number(argv[9]));
            assert(s->fps_num == fps_num && s->fps_den == fps_den);
            assert(s->total_frames == expected);
            assert(s->audio_present == audio);
            assert(video_duration_ms() == (uint64_t)expected * 1000 * fps_den / fps_num);
        }
        if (s->state != VIDEO_FINISHED) {
            uint64_t shown = s->displayed_frames ? s->displayed_frames - 1 : 0;
            assert(video_position_ms() == shown * 1000 * fps_den / fps_num);
        }
        assert(video_position_ms() >= previous_position);
        previous_position = video_position_ms();
        if (s->displayed_frames != count) {
            ++count;
            assert(count <= expected);
            if (count == 1) first_tick = now;
            if (!controls) {
                /* Exact rational cadence must stay within two 70-Hz ticks;
                 * rounding fps to 24/30 would drift beyond this on long clips. */
                int64_t error = (int64_t)(now - first_tick) * fps_num
                              - (int64_t)(count - 1) * TIMER_HZ * fps_den;
                assert(error >= -(int64_t)fps_num * 2 && error <= (int64_t)fps_num * 2);
            }
            output_frame(output, width, height, count);
        }
    }
    assert(!fclose(output));
    assert(count == expected && video_status()->displayed_frames == expected);
    assert(video_position_ms() == video_duration_ms());
    assert(max_arena > 0 && max_arena <= VIDEO_CAPACITY);
    if (!controls) {
        int64_t error = (int64_t)(now - first_tick) * fps_num
                      - (int64_t)expected * TIMER_HZ * fps_den;
        assert(error >= -(int64_t)fps_num * 2 && error <= (int64_t)fps_num * 2);
        assert(video_status()->late_resyncs == 0);
    } else {
        assert(paused && late && video_status()->late_resyncs == 1);
        stop_and_replay(argv[1]);
    }
    printf("MPEG-1 PASS: %u frames, %ux%u, %u/%u fps, %u polls, arena %u/%u bytes%s\n",
           count, width, height, fps_num, fps_den, polls, max_arena, VIDEO_CAPACITY,
           controls ? ", owned input, pause/resume, late resync and stop/replay" : "");
    return 0;
}
