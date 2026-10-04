/* Original bounded MPEG-1 program-stream adapter. The video decoder itself is
 * the pinned MIT-licensed pl_mpeg implementation; see third_party/pl_mpeg. */
#include "video.h"
#include "platform.h"
#include "gfx.h"
#include <stddef.h>

#define STREAM_OFFSET VIDEO_MAX_FILE_BYTES
#define DECODER_OFFSET (2u * VIDEO_MAX_FILE_BYTES)
#define DECODER_BYTES 0x200000u
#define PIXELS_OFFSET (DECODER_OFFSET + DECODER_BYTES)
#define SCAN_BUDGET 32768u
#define MAX_FRAMES 18000u
#ifndef VIDEO_HOST_TEST
#define ARENA ((uint8_t *)VIDEO_BASE)
#else
extern uint8_t video_test_arena[VIDEO_CAPACITY];
#define ARENA video_test_arena
#endif
#define SOURCE ARENA
#define STREAM (ARENA + STREAM_OFFSET)
#define PIXELS (ARENA + PIXELS_OFFSET)
static unsigned allocated;
static void *video_copy(void *to, const void *from, size_t n) {
    uint8_t *d = to; const uint8_t *s = from;
    for (size_t i = 0; i < n; ++i) d[i] = s[i];
    return to;
}
static void *video_move(void *to, const void *from, size_t n) {
    uint8_t *d = to; const uint8_t *s = from;
    if ((uintptr_t)d < (uintptr_t)s) return video_copy(to, from, n);
    while (n) { --n; d[n] = s[n]; }
    return to;
}
static void *video_zero(void *to, int value, size_t n) {
    uint8_t *d = to;
    for (size_t i = 0; i < n; ++i) d[i] = (uint8_t)value;
    return to;
}
/* Only the fixed-memory buffer and low-level video constructor are called.
 * Their exact maximum allocation is statically checked after the include;
 * no allocation is requested by frame decode. The checks remain defensive. */
static void *video_alloc(size_t size) {
    if (size > DECODER_BYTES - 16 || allocated > DECODER_BYTES - 16 - size) return NULL;
    uint8_t *block = ARENA + DECODER_OFFSET + allocated;
    *(size_t *)block = size;
    allocated += ((unsigned)size + 31u) & ~15u;
    return block + 16;
}
static void *video_realloc(void *old, size_t size) {
    void *next = video_alloc(size);
    if (old && next) {
        size_t before = *(size_t *)((uint8_t *)old - 16);
        video_copy(next, old, before < size ? before : size);
    }
    return next;
}
static int video_abs(int n) { return n<0?-n:n; }
#define abs video_abs
#define memcpy video_copy
#define memmove video_move
#define memset video_zero
#define PLM_NO_STDIO
#define PLM_MALLOC video_alloc
#define PLM_REALLOC video_realloc
#define PLM_FREE(pointer) ((void)(pointer))
#define PL_MPEG_IMPLEMENTATION
#include "../third_party/pl_mpeg/pl_mpeg.h"
#undef abs
#undef memcpy
#undef memmove
#undef memset

_Static_assert(sizeof(plm_buffer_t) + sizeof(plm_video_t) + 96u +
    ((VIDEO_MAX_WIDTH+15u)&~15u) * ((VIDEO_MAX_HEIGHT+15u)&~15u) * 9u / 2u < DECODER_BYTES,
    "video decoder does not fit its bounded arena");
_Static_assert(PIXELS_OFFSET + VIDEO_MAX_WIDTH * VIDEO_MAX_HEIGHT <= VIDEO_CAPACITY,
    "video output does not fit its reserved arena");

#ifndef VIDEO_HOST_TEST
typedef struct { uint8_t bytes[108]; } VideoFpuState;
static unsigned fpu_begin(VideoFpuState *saved) {
    unsigned previous;
    __asm__ volatile("mov %%cr0,%0" : "=r"(previous));
    unsigned enabled = (previous & ~12u) | 2u;
    __asm__ volatile("mov %0,%%cr0" :: "r"(enabled) : "memory");
    __asm__ volatile("fnsave %0\n\tfninit" : "=m"(*saved) :: "memory", "st", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)", "st(7)");
    return previous;
}
static void fpu_end(const VideoFpuState *saved, unsigned previous) {
    __asm__ volatile("frstor %0" :: "m"(*saved) : "memory", "st", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)", "st(7)");
    __asm__ volatile("mov %0,%%cr0" :: "r"(previous) : "memory");
}
static int have_fpu(void) {
    unsigned a,b,c,d;
    __asm__ volatile("cpuid" : "=a"(a),"=b"(b),"=c"(c),"=d"(d) : "a"(1));
    return (d & 1) != 0;
}
#else
typedef unsigned VideoFpuState;
static unsigned fpu_begin(VideoFpuState *saved) { (void)saved; return 0; }
static void fpu_end(const VideoFpuState *saved, unsigned previous) { (void)saved;(void)previous; }
static int have_fpu(void) { return 1; }
#endif

static VideoStatus status;
static VideoFrame frame;
static plm_video_t *decoder;
static unsigned file_bytes, cursor, stream_bytes, scan_cursor, phase, video_stream;
static unsigned first_tick, pause_tick, paused_loading;
static unsigned rate_code, aspect_code;
static const unsigned fps_num[] = {0,24000,24,25,30000,30};
static const unsigned fps_den[] = {1,1001,1,1,1001,1};
static const unsigned aspect[] = {0,10000,6735,7031,7615,8055,8437,8935,9157,9815,10255,10695,10950,11575,12051};

static int fail(int error) { status.error = error; status.state = VIDEO_ERROR; return 1; }
static unsigned be16(const uint8_t *p) { return (unsigned)p[0]*256u+p[1]; }
static int start_code(const uint8_t *p) { return p[0]==0 && p[1]==0 && p[2]==1; }
int video_play(const void *data, uint32_t bytes) {
    const uint8_t *p = data;
    int error = MEDIA_OK;
    if (!p || bytes < 16 || !start_code(p) || p[3] != 0xba) error = MEDIA_BAD_FILE;
    else if (bytes > VIDEO_MAX_FILE_BYTES) error = MEDIA_TOO_LARGE;
    else if ((p[4] & 0xf0) != 0x20 || !have_fpu()) error = MEDIA_UNSUPPORTED;
    if (error) { status.error = error; return error; }
    video_copy(SOURCE, data, bytes);
    status = (VideoStatus){.state=VIDEO_LOADING, .fps_den=1, .aspect_num=1, .aspect_den=1};
    frame = (VideoFrame){0}; decoder = NULL; allocated = 0;
    file_bytes = bytes; cursor = stream_bytes = scan_cursor = 0;
    phase = video_stream = rate_code = aspect_code = paused_loading = 0;
    return MEDIA_OK;
}

/* Each poll consumes one pack/system/PES unit. Only video payload is copied;
 * audio packets are detected and skipped, never presented as audible playback.
 * All length arithmetic is checked before the low-level decoder sees bytes. */
static int demux_step(void) {
    if (cursor == file_bytes) { phase=1; return 1; }
    if (file_bytes-cursor < 4 || !start_code(SOURCE+cursor)) return fail(MEDIA_BAD_FILE);
    unsigned type = SOURCE[cursor+3];
    if (type == 0xb9) { cursor=file_bytes;phase=1;return 1; }
    if (type == 0xba) {
        if (file_bytes-cursor < 12) return fail(MEDIA_BAD_FILE);
        if ((SOURCE[cursor+4]&0xf0)!=0x20) return fail(MEDIA_UNSUPPORTED);
        cursor+=12;return 0;
    }
    if (type < 0xbb || file_bytes-cursor < 6) return fail(MEDIA_BAD_FILE);
    unsigned length=be16(SOURCE+cursor+4), begin=cursor+6;
    if (length>file_bytes-begin) return fail(MEDIA_BAD_FILE);
    unsigned end=begin+length;cursor=end;
    if (type >= 0xc0 && type <= 0xdf) status.audio_present=1;
    if (type < 0xe0 || type > 0xef) return 0;
    if (!video_stream) video_stream=type;
    if (type != video_stream) return 0;
    while (begin<end && SOURCE[begin]==0xff) ++begin;
    if (begin<end && (SOURCE[begin]&0xc0)==0x40) {
        if (end-begin<2) return fail(MEDIA_BAD_FILE);
        begin+=2;
    }
    if (begin==end) return fail(MEDIA_BAD_FILE);
    unsigned header;
    if ((SOURCE[begin]&0xf0)==0x20) header=5;
    else if ((SOURCE[begin]&0xf0)==0x30) header=10;
    else if (SOURCE[begin]==0x0f) header=1;
    else return fail(MEDIA_UNSUPPORTED);
    if (header>end-begin) return fail(MEDIA_BAD_FILE);
    begin+=header;
    if (end-begin>VIDEO_MAX_FILE_BYTES-stream_bytes) return fail(MEDIA_TOO_LARGE);
    video_copy(STREAM+stream_bytes,SOURCE+begin,end-begin);
    stream_bytes+=end-begin;
    return 0;
}

/* Validate every sequence header before constructing the decoder. Resolution
 * or rate changes, MPEG-2 extensions, and D pictures are outside this bounded
 * MPEG-1 contract. Picture counting gives exact frame-based duration without
 * trusting container timestamp gaps. */
static int scan_step(void) {
    unsigned end=scan_cursor+SCAN_BUDGET;
    if (end>stream_bytes) end=stream_bytes;
    for (unsigned i=scan_cursor;i<end && i+4<=stream_bytes;++i) {
        if (!start_code(STREAM+i)) continue;
        unsigned code=STREAM[i+3];
        if (code==0xb5) return fail(MEDIA_UNSUPPORTED);
        if (code==0xb3) {
            if (stream_bytes-i<12) return fail(MEDIA_BAD_FILE);
            unsigned w=(unsigned)STREAM[i+4]*16+(STREAM[i+5]>>4);
            unsigned h=((unsigned)STREAM[i+5]&15)*256+STREAM[i+6];
            unsigned rate=STREAM[i+7]&15, par=STREAM[i+7]>>4;
            if (!w || !h || !rate || !par || par>14) return fail(MEDIA_BAD_FILE);
            if (w>VIDEO_MAX_WIDTH || h>VIDEO_MAX_HEIGHT || rate>5) return fail(MEDIA_UNSUPPORTED);
            if (status.width && (w!=status.width || h!=status.height || rate!=rate_code || par!=aspect_code))
                return fail(MEDIA_UNSUPPORTED);
            status.width=w;status.height=h;rate_code=rate;aspect_code=par;
            status.fps_num=fps_num[rate];status.fps_den=fps_den[rate];
            status.aspect_num=aspect[par];status.aspect_den=10000;
        } else if (code==0) {
            if (!status.width || stream_bytes-i<8) return fail(MEDIA_BAD_FILE);
            unsigned picture=(STREAM[i+5]>>3)&7;
            if (picture<1 || picture>3) return fail(MEDIA_UNSUPPORTED);
            if (!status.total_frames && picture!=1) return fail(MEDIA_UNSUPPORTED);
            if (++status.total_frames>MAX_FRAMES) return fail(MEDIA_TOO_LARGE);
        }
    }
    scan_cursor=end;
    if (end<stream_bytes) return 0;
    if (!status.total_frames || stream_bytes<140) return fail(MEDIA_BAD_FILE);
    /* The fixed maximum follows from checked geometry, and the only three
     * constructor allocations are statically proven to fit DECODER_BYTES. */
    /* Legal program streams need not finish with sequence_end. pl_mpeg's
     * final VLC lookahead still needs a terminator and zero guard bytes to
     * decode the last block completely. These are decoder-local bytes, not
     * modifications to the original MPEG-PS file. */
    if (stream_bytes > VIDEO_MAX_FILE_BYTES-8u) return fail(MEDIA_TOO_LARGE);
    static const uint8_t terminal[8]={0,0,1,0xb7,0,0,0,0};
    video_copy(STREAM+stream_bytes,terminal,sizeof terminal);
    VideoFpuState saved;unsigned previous=fpu_begin(&saved);
    plm_buffer_t *buffer=plm_buffer_create_with_memory(STREAM,stream_bytes+sizeof terminal,0);
    decoder=plm_video_create_with_buffer(buffer,0);
    int valid=plm_video_has_header(decoder);
    fpu_end(&saved,previous);
    if (!valid) return fail(MEDIA_BAD_FILE);
    status.arena_bytes=PIXELS_OFFSET+status.width*status.height;
    first_tick=timer_ticks();status.state=VIDEO_PLAYING;phase=2;
    return 1;
}
static unsigned frame_ticks(unsigned n) { return n*status.fps_den*TIMER_HZ/status.fps_num; }
static unsigned frame_ms(unsigned n) {
    if (!status.fps_num) return 0;
    unsigned scaled=n*status.fps_den;
    return scaled/status.fps_num*1000u+(scaled%status.fps_num)*1000u/status.fps_num;
}
static int clamp(int v) { return v<0?0:v>255?255:v; }
static void palette_frame(plm_frame_t *decoded) {
    /* BT.601 studio-range YCbCr, then ordered dither into the OS's fixed
     * 5x5x5 color cube. Gray uses its finer 32-level ramp. No palette writes. */
    static const int dither[16]={-30,2,-22,10,18,-14,26,-6,-18,14,-26,6,30,-2,22,-10};
    for (unsigned y=0;y<status.height;++y) for (unsigned x=0;x<status.width;++x) {
        int l=(int)decoded->y.data[y*decoded->y.width+x]-16;
        unsigned c=(y/2)*decoded->cb.width+x/2;
        int cb=(int)decoded->cb.data[c]-128,cr=(int)decoded->cr.data[c]-128;
        int r=clamp((298*l+409*cr+128)>>8),g=clamp((298*l-100*cb-208*cr+128)>>8),b=clamp((298*l+516*cb+128)>>8);
        uint8_t color;
        if (r-g<12 && g-r<12 && b-g<12 && g-b<12) color=(uint8_t)(PAL_GRAY+(g*31+127)/255);
        else {
            int d=dither[(y&3)*4+(x&3)];
            unsigned qr=(unsigned)clamp(r+d+32)/64,qg=(unsigned)clamp(g+d+32)/64,qb=(unsigned)clamp(b+d+32)/64;
            /* 255/64 is 3, but the final 32 levels select the white endpoint. */
            if (r+d>=224) qr=4;
            if (g+d>=224) qg=4;
            if (b+d>=224) qb=4;
            color=(uint8_t)(PAL_CUBE+qr*25+qg*5+qb);
        }
        PIXELS[y*status.width+x]=color;
    }
}
int video_poll(void) {
    if (status.state==VIDEO_LOADING) return phase?scan_step():demux_step();
    if (status.state!=VIDEO_PLAYING) return 0;
    unsigned now=timer_ticks(),due=frame_ticks(status.displayed_frames);
    if ((int32_t)(now-first_tick-due)<0) return 0;
    if (now-first_tick-due>TIMER_HZ/2) {
        first_tick=now-due;++status.late_resyncs;
    }
    if (status.displayed_frames==status.total_frames) { status.state=VIDEO_FINISHED;return 1; }
    VideoFpuState saved;unsigned previous=fpu_begin(&saved);
    plm_frame_t *decoded=plm_video_decode(decoder);
    fpu_end(&saved,previous);
    if (!decoded) return fail(MEDIA_BAD_FILE);
    ++status.displayed_frames;
    frame=(VideoFrame){.y=decoded->y.data,.cb=decoded->cb.data,.cr=decoded->cr.data,
        .pixels=PIXELS,.width=decoded->width,.height=decoded->height,
        .y_stride=decoded->y.width,.chroma_stride=decoded->cb.width,.number=status.displayed_frames};
    palette_frame(decoded);
    return 1;
}
void video_pause(int paused) {
    if (paused && (status.state==VIDEO_PLAYING || status.state==VIDEO_LOADING)) {
        paused_loading=status.state==VIDEO_LOADING;pause_tick=timer_ticks();status.state=VIDEO_PAUSED;
    } else if (!paused && status.state==VIDEO_PAUSED) {
        first_tick+=timer_ticks()-pause_tick;
        status.state=paused_loading?VIDEO_LOADING:VIDEO_PLAYING;
    }
}
void video_stop(void) {
    status.state=VIDEO_STOPPED;status.error=MEDIA_OK;status.displayed_frames=0;
    frame=(VideoFrame){0};decoder=NULL;
}
void video_clear_error(void) { if (status.state==VIDEO_ERROR) video_stop(); else status.error=MEDIA_OK; }
const VideoStatus *video_status(void) { return &status; }
const VideoFrame *video_frame(void) { return frame.y?&frame:NULL; }
uint32_t video_position_ms(void) {
    if (status.state==VIDEO_FINISHED) return video_duration_ms();
    return status.displayed_frames?frame_ms(status.displayed_frames-1):0;
}
uint32_t video_duration_ms(void) { return frame_ms(status.total_frames); }
const char *video_error_string(int error) {
    switch (error) {
    case MEDIA_BAD_FILE:return "Cannot read this MPEG-1 program-stream video.";
    case MEDIA_UNSUPPORTED:return "Use MPEG-1 video, up to 640 x 480 and 30 fps.";
    case MEDIA_TOO_LARGE:return "Video limit: 2 MiB and 18,000 frames.";
    default:return "Video playback stopped.";
    }
}
