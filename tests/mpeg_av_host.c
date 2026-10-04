/* Real MPEG-1/MP2 adapter, mocked PCM driver and independently stepped clocks.
 * Inputs are original, normally encoded files, not a malformed-input corpus. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio.h"
#include "video.h"
#include "platform.h"

uint8_t video_test_arena[VIDEO_CAPACITY];
static uint32_t now, pit_fraction, audio_fraction;
static AudioStatus pcm = {.state=AUDIO_STOPPED,.available=1};
static AudioPcmReader pcm_reader;
static void *pcm_context;
static FILE *pcm_output, *video_output;
static unsigned decoded_frames, reader_calls, largest_request, reached_eof;
static unsigned pause_calls, stop_calls, play_calls, pre_pause_state;
uint32_t timer_ticks(void) { return now; }
const AudioStatus *audio_status(void) { return &pcm; }
uint32_t audio_position_ms(void) {
    return pcm.sample_rate ? (uint64_t)pcm.played_frames * 1000 / pcm.sample_rate : 0;
}
uint32_t audio_duration_ms(void) {
    return pcm.sample_rate ? (uint64_t)pcm.total_frames * 1000 / pcm.sample_rate : 0;
}
void audio_stop(void) {
    ++stop_calls; pcm.state=AUDIO_STOPPED; pcm_reader=NULL; pcm_context=NULL;
}
void audio_pause(int paused) {
    ++pause_calls;
    if (paused && (pcm.state==AUDIO_PLAYING || pcm.state==AUDIO_LOADING)) {
        pre_pause_state=pcm.state;pcm.state=AUDIO_PAUSED;
    } else if (!paused && pcm.state==AUDIO_PAUSED) pcm.state=(int)pre_pause_state;
}
int audio_play_pcm_stream(unsigned rate, unsigned channels, uint32_t total_frames,
                          AudioPcmReader reader, void *context) {
    ++play_calls;
    if (!pcm.available) return MEDIA_NO_DEVICE;
    assert(rate>=5000 && rate<=48000 && channels==2 && total_frames && reader);
    pcm=(AudioStatus){.state=AUDIO_LOADING,.available=1,.format=AUDIO_FORMAT_STREAM,
                     .sample_rate=rate,.channels=channels,.bits_per_sample=16,
                     .total_frames=total_frames};
    pcm_reader=reader; pcm_context=context;
    decoded_frames=reader_calls=largest_request=reached_eof=audio_fraction=0;
    return MEDIA_OK;
}
void audio_poll(void) {
    if (pcm.state!=AUDIO_LOADING && pcm.state!=AUDIO_PLAYING) return;
    if (!reached_eof && decoded_frames-pcm.played_frames<4096) {
        static const unsigned chunks[]={1,17,511,2048,1152,67,1023,2048};
        unsigned request=chunks[reader_calls % (sizeof chunks/sizeof chunks[0])];
        int16_t output[4096];
        assert(pcm_reader && request<=2048);
        int count=pcm_reader(pcm_context,output,request);
        ++reader_calls;
        if (request>largest_request) largest_request=request;
        if (count<0) fprintf(stderr,"PCM callback failed: %d after %u/%u frames\n",count,decoded_frames,pcm.total_frames);
        assert(count>=0 && (unsigned)count<=request);
        assert(decoded_frames+(unsigned)count<=pcm.total_frames);
        if (count) {
            if (pcm_output) assert(fwrite(output,sizeof(int16_t)*2,(unsigned)count,pcm_output)==(unsigned)count);
            decoded_frames+=(unsigned)count;
        } else {
            assert(decoded_frames==pcm.total_frames);
            reached_eof=1;
        }
    }
    if (pcm.state==AUDIO_LOADING && decoded_frames) pcm.state=AUDIO_PLAYING;
    if (reached_eof && pcm.played_frames==decoded_frames) {
        pcm.state=AUDIO_FINISHED; pcm_reader=NULL; pcm_context=NULL;
    }
}
static void advance(unsigned milliseconds, int pit, int hardware) {
    if (pit) {
        pit_fraction+=milliseconds*TIMER_HZ;
        now+=pit_fraction/1000; pit_fraction%=1000;
    }
    if (hardware && pcm.state==AUDIO_PLAYING) {
        audio_fraction+=milliseconds*pcm.sample_rate;
        unsigned count=audio_fraction/1000; audio_fraction%=1000;
        if (count>decoded_frames-pcm.played_frames) count=decoded_frames-pcm.played_frames;
        pcm.played_frames+=count;
    }
}
static unsigned number(const char *text) {
    char *end; unsigned long result=strtoul(text,&end,10);
    assert(*text && !*end && result<=UINT32_MAX); return (unsigned)result;
}
static void play_owned(const char *path) {
    FILE *input=fopen(path,"rb"); assert(input && !fseek(input,0,SEEK_END));
    long bytes=ftell(input); assert(bytes>0 && bytes<=VIDEO_MAX_FILE_BYTES);
    rewind(input); uint8_t *data=malloc((size_t)bytes);
    assert(data && fread(data,1,(size_t)bytes,input)==(size_t)bytes); fclose(input);
    assert(video_play(data,(uint32_t)bytes)==MEDIA_OK);
    memset(data,0xdd,(size_t)bytes); free(data);
    assert(video_status()->state==VIDEO_LOADING);
}
static void output_video_frame(const VideoFrame *frame) {
    const uint8_t *planes[3]={frame->y,frame->cb,frame->cr};
    for (unsigned c=0;c<3;++c) {
        unsigned w=frame->width>>(c!=0),h=frame->height>>(c!=0);
        unsigned stride=c?frame->chroma_stride:frame->y_stride;
        uintptr_t begin=(uintptr_t)video_test_arena,end=begin+sizeof(video_test_arena);
        assert(stride>=w && (uintptr_t)planes[c]>=begin);
        assert((uintptr_t)planes[c]+(h-1)*stride+w<=end);
        for (unsigned y=0;y<h;++y)
            assert(fwrite(planes[c]+y*stride,1,w,video_output)==w);
    }
}
static void load_pause(void) {
    unsigned plays=play_calls;
    video_pause(1);
    for (unsigned i=0;i<100;++i) {
        advance(10,1,1); audio_poll(); video_poll();
        assert(video_status()->state==VIDEO_PAUSED);
        assert(video_status()->displayed_frames==0 && video_position_ms()==0);
        assert(play_calls==plays);
    }
    video_pause(0); assert(video_status()->state==VIDEO_LOADING);
}
static void preroll_pause(void) {
    assert(video_status()->state==VIDEO_LOADING && pcm.state==AUDIO_LOADING);
    video_pause(1);
    assert(video_status()->state==VIDEO_PAUSED && pcm.state==AUDIO_PAUSED);
    unsigned calls=reader_calls;
    for (unsigned i=0;i<100;++i) {
        advance(10,1,1);audio_poll();video_poll();
        assert(reader_calls==calls && !pcm.played_frames);
        assert(video_status()->state==VIDEO_PAUSED && !video_status()->displayed_frames);
    }
    video_pause(0);
    assert(video_status()->state==VIDEO_LOADING && pcm.state==AUDIO_LOADING);
}
static void playback_pause(void) {
    unsigned displayed=video_status()->displayed_frames, position=video_position_ms();
    unsigned consumed=pcm.played_frames, generated=decoded_frames, calls=reader_calls;
    video_pause(1);
    assert(video_status()->state==VIDEO_PAUSED && pcm.state==AUDIO_PAUSED);
    for (unsigned i=0;i<150;++i) {
        advance(10,1,1); audio_poll(); video_poll();
        assert(video_status()->state==VIDEO_PAUSED && pcm.state==AUDIO_PAUSED);
        assert(video_status()->displayed_frames==displayed && video_position_ms()==position);
        assert(pcm.played_frames==consumed && decoded_frames==generated && reader_calls==calls);
    }
    video_pause(0);
    assert(video_status()->state==VIDEO_PLAYING && pcm.state==AUDIO_PLAYING);
    assert(video_status()->displayed_frames==displayed && video_position_ms()==position);
    assert(pcm.played_frames==consumed);
}
static void check_stop(const char *source) {
    pcm_output=NULL;
    for (unsigned pass=0;pass<3;++pass) {
        play_owned(source);
        for (unsigned polls=0;video_status()->displayed_frames<5;++polls) {
            assert(polls<10000); audio_poll(); advance(5,1,1); video_poll();
            assert(video_status()->state!=VIDEO_ERROR);
        }
        assert(pcm_reader);
        if (pass==1) {
            unsigned old_stops=stop_calls;
            play_owned(source);
            assert(stop_calls>old_stops && !pcm_reader && pcm.state==AUDIO_STOPPED);
            for (unsigned polls=0;video_status()->displayed_frames<5;++polls) {
                assert(polls<10000);audio_poll();advance(5,1,1);video_poll();
                assert(video_status()->state!=VIDEO_ERROR);
            }
            assert(pcm_reader);
        }
        unsigned stops=stop_calls;
        video_stop();
        assert(video_status()->state==VIDEO_STOPPED && pcm.state==AUDIO_STOPPED);
        assert(stop_calls>stops && !pcm_reader && !pcm_context);
        unsigned calls=reader_calls, frames=video_status()->displayed_frames;
        for (unsigned i=0;i<100;++i) {advance(10,1,1);audio_poll();video_poll();}
        assert(reader_calls==calls && video_status()->displayed_frames==frames);
    }
}
int main(int argc,char **argv) {
    /* source, PCM output, geometry/rate/count, audio metadata and mode:
     * mode 0=ordinary, 1=controls, 2=missing device, 3=unsupported audio. */
    assert(argc==15);
    unsigned width=number(argv[3]),height=number(argv[4]);
    unsigned fps_num=number(argv[5]),fps_den=number(argv[6]),frames=number(argv[7]);
    unsigned rate=number(argv[8]),channels=number(argv[9]),audio_frames=number(argv[10]);
    unsigned lead=number(argv[11]),video_start=number(argv[12]);
    unsigned mode=number(argv[13]),wrap=number(argv[14]);
    unsigned enabled=mode<2;
    if (wrap) now=UINT32_MAX-5;
    if (mode==2) pcm.available=0;
    pcm_output=fopen(argv[2],"wb"); assert(pcm_output);
    char video_path[4096];
    assert(snprintf(video_path,sizeof video_path,"%s.yuv",argv[2])<(int)sizeof video_path);
    video_output=fopen(video_path,"wb"); assert(video_output);
    play_owned(argv[1]);
    if (mode==1) load_pause();
    unsigned polls=0,shown=0,paused=0,frozen_audio=0,independent_audio=0,preroll_paused=0;
    unsigned first_clock=0,last_position=0,max_arena=0;
    while (video_status()->state!=VIDEO_FINISHED) {
        const VideoStatus *status=video_status();
        assert(status->state==VIDEO_LOADING || status->state==VIDEO_PLAYING);
        if (mode==1 && pcm_reader && status->state==VIDEO_LOADING && !preroll_paused) {
            preroll_pause();preroll_paused=1;
        }
        if (mode==1 && shown>=10 && !paused) {playback_pause();paused=1;}
        /* A stopped hardware clock cannot be replaced by PIT elapsed time. */
        if (mode==1 && shown>=15 && !frozen_audio) {
            unsigned old=shown;
            for (unsigned i=0;i<200;++i) {
                advance(5,1,0);audio_poll();video_poll();
                assert(video_status()->displayed_frames==old);
            }
            frozen_audio=1;
        }
        audio_poll();
        int tick_pit=1;
        if (mode==1 && shown>=20 && shown<30 && pcm.state==AUDIO_PLAYING) {
            tick_pit=0;independent_audio=1;
        }
        advance(5,tick_pit,1);
        unsigned before=video_status()->displayed_frames;
        video_poll();status=video_status();
        if (status->state==VIDEO_ERROR)
            fprintf(stderr,"video failed: %d, audio error %d after %u frames\n",status->error,status->audio_error,shown);
        assert(status->state!=VIDEO_ERROR && status->error==MEDIA_OK && ++polls<100000);
        assert(status->arena_bytes<=VIDEO_CAPACITY);
        if (status->arena_bytes>max_arena) max_arena=status->arena_bytes;
        assert(status->displayed_frames>=before && status->displayed_frames<=before+1);
        if (status->state!=VIDEO_LOADING) {
            assert(status->width==width && status->height==height && status->total_frames==frames);
            assert(status->fps_num==fps_num && status->fps_den==fps_den && status->audio_present);
            assert(status->audio_enabled==enabled);
            if (enabled) {
                assert(status->audio_sample_rate==rate && status->audio_channels==channels);
                assert(status->audio_error==MEDIA_OK);
                assert(status->audio_lead_frames==lead && status->video_start_ms==video_start);
                assert(pcm.sample_rate==rate && pcm.channels==2);
                assert(pcm.total_frames==audio_frames+lead);
                unsigned video_ms=(uint64_t)frames*1000*fps_den/fps_num+video_start;
                unsigned audio_ms=(uint64_t)(audio_frames+lead)*1000/rate;
                assert(video_duration_ms()==(video_ms>audio_ms?video_ms:audio_ms));
            } else {
                assert(status->audio_error==(mode==2?MEDIA_NO_DEVICE:MEDIA_UNSUPPORTED));
            }
        }
        assert(video_position_ms()>=last_position);last_position=video_position_ms();
        if (status->displayed_frames!=shown) {
            shown=status->displayed_frames;
            if (shown==1) first_clock=audio_position_ms();
            if (enabled && pcm.state==AUDIO_PLAYING) {
                int64_t target=(int64_t)video_start+(uint64_t)(shown-1)*1000*fps_den/fps_num;
                int64_t difference=(int64_t)audio_position_ms()-target;
                if (difference < -1 || difference > 20)
                    fprintf(stderr,"A/V cadence error: frame %u clock %u target %lld\n",shown,audio_position_ms(),(long long)target);
                assert(difference>=-1 && difference<=20);
            }
            const VideoFrame *frame=video_frame();
            assert(frame && frame->number==shown && frame->width==width && frame->height==height);
            output_video_frame(frame);
        }
    }
    assert(shown==frames && video_position_ms()==video_duration_ms());
    if (enabled) {
        /* Audio tail is part of playback, including the last padded MP2 frame. */
        assert(pcm.state==AUDIO_FINISHED && reached_eof);
        assert(decoded_frames==audio_frames+lead && pcm.played_frames==decoded_frames);
        assert(reader_calls && largest_request==2048);
        assert(first_clock>=video_start && first_clock<=video_start+20);
    } else assert(!decoded_frames);
    assert(!fclose(pcm_output));pcm_output=NULL;
    assert(!fclose(video_output));video_output=NULL;
    unsigned completed_calls=reader_calls;
    if (mode==1) {
        assert(paused && preroll_paused && frozen_audio && independent_audio && pause_calls>=4);
        check_stop(argv[1]);
    }
    printf("MPEG A/V PASS: %u video frames, %u PCM frames at %u Hz, %u callbacks, arena %u/%u%s\n",
           frames,audio_frames+lead,rate,completed_calls,max_arena,VIDEO_CAPACITY,
           mode==1?", owned input, independent clocks, pause/resume and stop/replay":"");
    return 0;
}
