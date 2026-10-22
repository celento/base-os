#include "kernel/uaudio.h"
#include "kernel/uproc.h"
#include "kernel/audio.h"
#include "kernel/kheap.h"
#include "kernel/platform.h"
#include "kernel/fs.h"
#include "../sdk/baseos_sys.h"

/* About 186 ms at 44.1 kHz: the program's queue ahead of the device's own
 * 64 KiB ring. Gaps are filled with this many frames of silence at a time. */
#define RING_FRAMES 8192u
#define GAP_FRAMES 256u

typedef struct UStream {
    uint32_t handle, pid;
    unsigned rate, channels;
    int16_t *ring;               /* RING_FRAMES * channels samples */
    unsigned head, count;        /* frames */
    uint32_t paced_at;           /* without a device: when the queue was last drained */
    struct UStream *next;
} UStream;

static UStream *streams;
static uint32_t next_handle = 1;

static UStream *owned(Uproc *process, uint32_t handle) {
    for (UStream *stream = streams; stream; stream = stream->next)
        if (stream->handle == handle && stream->pid == process->pid) return stream;
    return 0;
}

/* AudioPcmReader for the stream that plays; silence while the queue is empty. */
static int read_frames(void *context, int16_t *output, unsigned max_frames) {
    UStream *stream = context;
    unsigned frames = stream->count < max_frames ? stream->count : max_frames;
    unsigned channels = stream->channels;
    if (!frames) {
        frames = max_frames < GAP_FRAMES ? max_frames : GAP_FRAMES;
        kmemset(output, 0, (int)(frames * channels * 2));
        return (int)frames;
    }
    for (unsigned i = 0; i < frames; i++) {
        const int16_t *from = stream->ring + ((stream->head + i) % RING_FRAMES) * channels;
        for (unsigned c = 0; c < channels; c++) output[i * channels + c] = from[c];
    }
    stream->head = (stream->head + frames) % RING_FRAMES;
    stream->count -= frames;
    thread_wake_channel((uintptr_t)stream, ~0u);
    return (int)frames;
}

static int playing(UStream *stream) {
    return audio_stream_context() == stream;
}

/* Make stream the one that plays: always when opened, later only when the
 * device is idle, so two streams never take it from each other in turn. */
static void claim_device(UStream *stream, int always) {
    if (playing(stream) || !audio_status()->available) return;
    int state = audio_status()->state;
    if (!always && (state == AUDIO_LOADING || state == AUDIO_PLAYING || state == AUDIO_PAUSED)) return;
    audio_play_pcm_stream(stream->rate, stream->channels, 0xFFFFFFFFu, read_frames, stream);
    stream->paced_at = timer_milliseconds();
}

/* A stream that does not play loses its frames at its rate, as if heard. */
static void pace(UStream *stream) {
    uint32_t now = timer_milliseconds();
    if (playing(stream)) { stream->paced_at = now; return; }
    uint32_t elapsed = now - stream->paced_at;
    if (elapsed > 1000u) elapsed = 1000u;
    uint32_t played = elapsed * stream->rate / 1000u;
    if (!played) return;
    stream->paced_at = now;
    if (played > stream->count) played = stream->count;
    stream->head = (stream->head + played) % RING_FRAMES;
    stream->count -= played;
}

int uaudio_open(Uproc *process, uint32_t rate, uint32_t channels) {
    if (rate < BOS_AUDIO_RATE_MIN || rate > BOS_AUDIO_RATE_MAX || (channels != 1 && channels != 2))
        return -BOS_EINVAL;
    UStream *stream = kmalloc(sizeof *stream);
    if (!stream) return -BOS_ENOMEM;
    stream->ring = kmalloc(RING_FRAMES * channels * 2u);
    if (!stream->ring) { kfree(stream); return -BOS_ENOMEM; }
    stream->handle = next_handle++;
    stream->pid = process->pid;
    stream->rate = rate;
    stream->channels = channels;
    stream->paced_at = timer_milliseconds();
    stream->next = streams;
    streams = stream;
    claim_device(stream, 1);
    return (int)stream->handle;
}

int uaudio_write(Uproc *process, uint32_t handle, uint32_t frames, uint32_t count) {
    UStream *stream = owned(process, handle);
    if (!stream) return -BOS_EBADF;
    if (count > 0x1000000u) return -BOS_EINVAL;
    uint32_t done = 0;
    while (done < count) {
        claim_device(stream, 0);
        pace(stream);
        if (stream->count == RING_FRAMES) {
            /* Woken as the device drains the queue, or after a short wait
             * when no device drains it. */
            thread_wait((uintptr_t)stream, timer_milliseconds() + 10);
            if (process->state != UPROC_RUNNING) return -BOS_EINTR;
            stream = owned(process, handle);
            if (!stream) return done ? (int)done : -BOS_EBADF;
            continue;
        }
        unsigned room = RING_FRAMES - stream->count, take = count - done < room ? count - done : room;
        unsigned frame_bytes = stream->channels * 2u;
        int result = vm_prepare(&process->space, frames + done * frame_bytes, take * frame_bytes, 0);
        if (result) return done ? (int)done : result;
        const int16_t *source = (const int16_t *)(uintptr_t)(frames + done * frame_bytes);
        for (unsigned i = 0; i < take; i++) {
            int16_t *to = stream->ring + ((stream->head + stream->count + i) % RING_FRAMES) * stream->channels;
            for (unsigned c = 0; c < stream->channels; c++) to[c] = source[i * stream->channels + c];
        }
        stream->count += take;
        done += take;
    }
    return (int)done;
}

int uaudio_queued(Uproc *process, uint32_t handle) {
    UStream *stream = owned(process, handle);
    if (!stream) return -BOS_EBADF;
    pace(stream);
    return (int)stream->count;
}

static void free_stream(UStream *stream) {
    if (audio_stream_context() == stream) audio_stop();
    thread_wake_channel((uintptr_t)stream, ~0u);
    kfree(stream->ring);
    kfree(stream);
}

int uaudio_close(Uproc *process, uint32_t handle) {
    for (UStream **link = &streams; *link; link = &(*link)->next) {
        UStream *stream = *link;
        if (stream->handle == handle && stream->pid == process->pid) {
            *link = stream->next;
            free_stream(stream);
            return 0;
        }
    }
    return -BOS_EBADF;
}

void uaudio_release_owner(uint32_t pid) {
    for (UStream **link = &streams; *link;) {
        UStream *stream = *link;
        if (stream->pid == pid) { *link = stream->next; free_stream(stream); }
        else link = &stream->next;
    }
}
