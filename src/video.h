#ifndef VIDEO_H
#define VIDEO_H
#include <stdint.h>
#include "media.h"

enum { VIDEO_STOPPED, VIDEO_LOADING, VIDEO_PLAYING, VIDEO_PAUSED, VIDEO_FINISHED, VIDEO_ERROR };
#define VIDEO_MAX_WIDTH 640u
#define VIDEO_MAX_HEIGHT 480u
#define VIDEO_MAX_FILE_BYTES (2u * 1024u * 1024u)
typedef struct {
    int state, error;
    unsigned width, height, fps_num, fps_den;
    unsigned aspect_num, aspect_den;
    unsigned total_frames, displayed_frames, audio_present;
    unsigned arena_bytes, late_resyncs, presentation_skips;
    unsigned audio_enabled, audio_sample_rate, audio_channels, audio_total_frames;
    unsigned audio_lead_frames, video_start_ms;
    int audio_error;
} VideoStatus;
typedef struct {
    const uint8_t *y, *cb, *cr;
    const uint8_t *pixels; /* BaseOS fixed 256-color palette, width-byte stride. */
    unsigned width, height, y_stride, chroma_stride, number;
} VideoFrame;

/* MPEG-1 program-stream video, <=640x480, <=30 fps, <=2 MiB input.
 * Optional constant-format MPEG-1 Layer II audio through the SB16 stream.
 * Unsupported/missing audio devices fall back to explicitly silent video.
 * Owns a source copy.
 * There is no heap; all stream/decoder/image storage is in VIDEO_BASE. */
int video_play(const void *data, uint32_t bytes);
/* Cooperative load: one <=65535-byte PES packet or 32768-byte scan per call.
 * Playback: at most one displayed frame per call (first output may require
 * two reference pictures). No frame-accumulating catch-up loop. */
int video_poll(void);
void video_pause(int paused);
void video_stop(void);
void video_clear_error(void);
const VideoStatus *video_status(void);
/* Read-only YUV420 frame; valid until the next video_poll or video_play. */
const VideoFrame *video_frame(void);
uint32_t video_position_ms(void);
uint32_t video_duration_ms(void);
const char *video_error_string(int error);
/* Letterboxed palette rendering, respects the supplied client rectangle. */
void video_draw(int x, int y, int w, int h);
#endif
