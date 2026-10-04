#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define AUDIO_HOST_TEST
#include "../src/audio.c"

uint8_t audio_test_dma[RING_BYTES],audio_test_source[AUDIO_WORK_CAPACITY];
uint8_t audio_test_large_source[AUDIO_LARGE_WORK_CAPACITY];
static unsigned background_polls;
static void (*background_check)(void);
void platform_poll(void) {
    ++background_polls;
    if (background_check) background_check();
    uint32_t decoded = mp3.decoded_frames, read = wave.frame_cursor;
    audio_poll();
    assert(mp3.decoded_frames - decoded <= 1152);
    assert(wave.frame_cursor - read <= PCM_SAMPLES_PER_POLL);
}
static uint8_t response[3],read_index,response_length;
static unsigned now,position,flip,present=1,busy,rate_bytes,programmed_rate;
uint32_t timer_ticks(void){return now;}
void audio_out(uint16_t port,uint8_t value) {
    if(port==SB_BASE+6&&value==0) {response[0]=0xaa;response_length=1;read_index=0;}
    if(port==DSP_WRITE&&value==0xe1) {response[0]=4;response[1]=5;response_length=2;read_index=0;}
    if(port==0xd8)flip=0;
    if(port==DSP_WRITE) {
        if(rate_bytes){programmed_rate=(programmed_rate<<8)|value;rate_bytes--;}
        else if(value==0x41){rate_bytes=2;programmed_rate=0;}
    }
}
uint8_t audio_in(uint16_t port) {
    if(!present)return 0xff;
    if(port==DSP_STATUS)return read_index<response_length?0x80:0;
    if(port==DSP_READ)return response[read_index++];
    if(port==DSP_WRITE)return busy?0x80:0;
    if(port==0xc6){unsigned count=32767-position/2;unsigned value=(count>>((flip++&1)*8))&255;return value;}
    return 0;
}
static uint8_t file[200000];
static void u16(unsigned p,unsigned v){file[p]=v;file[p+1]=v>>8;}
static void u32(unsigned p,unsigned v){u16(p,v);u16(p+2,v>>16);}
static unsigned make_wave(unsigned frames) {
    unsigned size=frames*2;memset(file,0,size+44);
    memcpy(file,"RIFF",4);u32(4,size+36);memcpy(file+8,"WAVEfmt ",8);
    u32(16,16);u16(20,1);u16(22,1);u32(24,22050);u32(28,44100);u16(32,2);u16(34,16);
    memcpy(file+36,"data",4);u32(40,size);
    for(unsigned i=0;i<frames;i++)u16(44+i*2,(i%100)*500-25000);
    return size+44;
}
static void start(unsigned frames) {
    position=0;unsigned bytes=make_wave(frames);assert(audio_play(file,bytes)==0);
    assert(audio_status()->state==AUDIO_LOADING);
    /* Playback owns its bytes even when source is destroyed. */
    memset(file,0,bytes);
    for(unsigned i=0;i<8;i++)audio_poll();
    assert(audio_status()->state==AUDIO_PLAYING);
    assert(((int16_t *)audio_test_dma)[0]==-25000);
}
typedef struct {
    unsigned frames, cursor, channels, chunk, calls, eof_calls, tone;
    int error;
    const int16_t *samples;
} TestPcmStream;
static int16_t stream_sample(unsigned frame,unsigned channel) {
    int value=(int)(frame%701)*70-24500;
    return (int16_t)(channel?-value:value);
}
static int16_t source_sample(const TestPcmStream *stream,unsigned frame,unsigned channel) {
    if(stream->samples)return stream->samples[frame*stream->channels+channel];
    if(!stream->tone)return stream_sample(frame,channel);
    /* An exact 1 kHz triangle wave sampled at 48 kHz, without a math library. */
    int phase=(int)(frame%48),value=phase<24?phase*2000-24000:72000-phase*2000;
    return (int16_t)(channel?-value:value);
}
static int read_pcm(void *context,int16_t *output,unsigned max_frames) {
    TestPcmStream *stream=context;
    assert(stream&&output&&max_frames&&max_frames*stream->channels<=PCM_SAMPLES_PER_POLL);
    stream->calls++;
    if(stream->error)return stream->error;
    unsigned count=stream->frames-stream->cursor;
    if(count>stream->chunk)count=stream->chunk;
    if(count>max_frames)count=max_frames;
    for(unsigned i=0;i<count;i++)for(unsigned c=0;c<stream->channels;c++)
        output[i*stream->channels+c]=source_sample(stream,stream->cursor+i,c);
    stream->cursor+=count;
    if(!count)stream->eof_calls++;
    return (int)count;
}
static int oversized_result(void *context,int16_t *output,unsigned max_frames) {
    (void)context;(void)output;
    /* Exercise the result contract without writing outside the supplied buffer. */
    return (int)max_frames+1;
}
static void poll_stream(TestPcmStream *stream) {
    unsigned before=stream->calls;
    audio_poll();assert(stream->calls-before<=1);
}
static void load_stream(TestPcmStream *stream,unsigned rate,unsigned declared) {
    position=0;
    assert(!audio_play_pcm_stream(rate,stream->channels,declared,read_pcm,stream));
    assert(!stream->calls&&audio_status()->state==AUDIO_LOADING);
    assert(audio_status()->format==AUDIO_FORMAT_STREAM&&audio_status()->bits_per_sample==16);
    assert(audio_status()->sample_rate==rate&&audio_status()->channels==stream->channels);
    assert(audio_status()->output_rate==(rate>45000?44100:rate));
    assert(audio_status()->total_frames==declared&&audio_status()->played_frames==0);
}
static void prefill_stream(TestPcmStream *stream) {
    for(unsigned i=0;audio_status()->state==AUDIO_LOADING;i++) {
        assert(i<1000);poll_stream(stream);
    }
    assert(audio_status()->state==AUDIO_PLAYING);
    assert(programmed_rate==audio_status()->output_rate);
}
static void advance_stream(TestPcmStream *stream,unsigned frames) {
    position=(position+frames*stream->channels*2)%RING_BYTES;
    now++;poll_stream(stream);
}
static void test_short_stream(void) {
    TestPcmStream stream={.frames=3000,.channels=2,.chunk=701};
    load_stream(&stream,44100,stream.frames);
    poll_stream(&stream);assert(stream.calls==1&&stream.cursor==701);
    unsigned saved_fill=fill_bytes;
    audio_pause(1);assert(audio_status()->state==AUDIO_PAUSED&&!hardware_running);
    now+=1000;poll_stream(&stream);audio_pause(1);
    assert(stream.calls==1&&fill_bytes==saved_fill);
    audio_pause(0);assert(audio_status()->state==AUDIO_LOADING);
    prefill_stream(&stream);assert(stream.eof_calls==1&&stream.calls==6);
    assert(audio_duration_ms()==68);
    for(unsigned i=0;i<RING_BYTES/4;i++)for(unsigned c=0;c<2;c++)
        assert(((int16_t *)audio_test_dma)[i*2+c]==(i<stream.frames?stream_sample(i,c):0));
    unsigned calls=stream.calls;
    audio_set_volume(37);assert(audio_status()->volume==37);
    advance_stream(&stream,2999);assert(audio_status()->played_frames==2999);
    audio_pause(1);assert(audio_status()->state==AUDIO_PAUSED);
    now+=1000;poll_stream(&stream);assert(audio_status()->played_frames==2999);
    audio_pause(0);assert(audio_status()->state==AUDIO_PLAYING);
    advance_stream(&stream,1);assert(audio_status()->played_frames==3000);
    advance_stream(&stream,2204);assert(audio_status()->state==AUDIO_PLAYING);
    advance_stream(&stream,1);assert(audio_status()->state==AUDIO_FINISHED);
    assert(stream.calls==calls&&!pcm_reader&&!pcm_context);
    assert(audio_status()->played_frames==stream.frames&&audio_status()->total_frames==stream.frames);
    assert(!audio_status()->underruns&&audio_status()->volume==37);
}
static void test_stream_refills(unsigned channels) {
    TestPcmStream stream={.frames=88200,.channels=channels,.chunk=1152};
    load_stream(&stream,44100,stream.frames);prefill_stream(&stream);
    unsigned consumed=0;
    while(audio_status()->state==AUDIO_PLAYING) {
        assert(consumed<stream.frames+5000);
        /* Check every DMA sample before emulating the device consuming it. */
        for(unsigned i=0;i<512;i++)for(unsigned c=0;c<channels;c++) {
            unsigned sample=((position/2)+i*channels+c)%(RING_BYTES/2);
            assert(((int16_t *)audio_test_dma)[sample]==
                   (consumed+i<stream.frames?stream_sample(consumed+i,c):0));
        }
        consumed+=512;advance_stream(&stream,512);
        assert(audio_status()->total_frames==stream.frames);
        assert(audio_status()->played_frames==(consumed<stream.frames?consumed:stream.frames));
    }
    assert(audio_status()->state==AUDIO_FINISHED&&!audio_status()->underruns);
    assert(stream.cursor==stream.frames&&stream.eof_calls==1&&!pcm_reader&&!pcm_context);
}
static void test_stream_ring_boundary(void) {
    TestPcmStream stream={.frames=RING_BYTES/4,.channels=2,.chunk=2048};
    load_stream(&stream,44100,stream.frames);prefill_stream(&stream);
    assert(stream.calls==8&&!stream.eof_calls&&!eof);
    advance_stream(&stream,HALF_BYTES/4);
    assert(stream.calls==9&&stream.eof_calls==1&&eof);
    for(unsigned i=0;i<3;i++)poll_stream(&stream);
    assert(ready_mask==3&&stream.calls==9);
    advance_stream(&stream,HALF_BYTES/4);
    for(unsigned i=0;i<3;i++)poll_stream(&stream);
    advance_stream(&stream,2205);
    assert(audio_status()->state==AUDIO_FINISHED&&audio_status()->played_frames==stream.frames);
}
static int16_t resampled_sample(const TestPcmStream *stream,unsigned frame,unsigned channel) {
    uint64_t scaled=(uint64_t)frame*48000;
    unsigned index=(unsigned)(scaled/44100),fraction=(unsigned)(scaled%44100);
    if(index>=stream->frames)return 0;
    unsigned next=index+1<stream->frames?index+1:index;
    return (int16_t)((source_sample(stream,index,channel)*(int)(44100-fraction)+
                      source_sample(stream,next,channel)*(int)fraction)/44100);
}
static void check_resampled_playback(TestPcmStream *stream) {
    unsigned expected_frames=(unsigned)(((uint64_t)stream->frames*44100+47999)/48000);
    unsigned consumed=0,crossings=0,finish=expected_frames+2205;
    int previous=-1;
    while(audio_status()->state==AUDIO_PLAYING) {
        assert(consumed<finish);
        unsigned count=finish-consumed;
        if(count>512)count=512;
        else if(count>1)count--; /* Check the last drain frame exactly. */
        for(unsigned i=0;i<count;i++)for(unsigned c=0;c<stream->channels;c++) {
            unsigned sample=((position/2)+i*stream->channels+c)%(RING_BYTES/2);
            int16_t value=((int16_t *)audio_test_dma)[sample];
            assert(value==resampled_sample(stream,consumed+i,c));
            if(!c&&consumed+i<expected_frames) {
                if(previous<0&&value>=0)crossings++;
                previous=value;
            }
        }
        consumed+=count;advance_stream(stream,count);
        unsigned expected=(unsigned)((uint64_t)consumed*48000/44100);
        if(expected>stream->frames)expected=stream->frames;
        assert(audio_status()->played_frames==expected);
        assert(audio_status()->total_frames==stream->frames&&audio_status()->sample_rate==48000);
    }
    assert(audio_status()->state==AUDIO_FINISHED&&!audio_status()->underruns);
    assert(output_frames==expected_frames&&consumed==finish);
    assert(audio_position_ms()==audio_duration_ms());
    if(stream->tone&&stream->frames%48000==0)assert(crossings==stream->frames/48);
}
static void test_resampled_stream(unsigned channels,unsigned frames,unsigned chunk) {
    TestPcmStream stream={.frames=frames,.channels=channels,.chunk=chunk,.tone=1};
    load_stream(&stream,48000,stream.frames);poll_stream(&stream);
    unsigned saved_fraction=resample_fraction,saved_stage=stage_frames,saved_calls=stream.calls;
    audio_pause(1);now+=1000;poll_stream(&stream);
    assert(audio_status()->state==AUDIO_PAUSED&&stream.calls==saved_calls);
    assert(resample_fraction==saved_fraction&&stage_frames==saved_stage);
    audio_pause(0);prefill_stream(&stream);
    assert(audio_duration_ms()==frames/48);
    check_resampled_playback(&stream);
    assert(stream.cursor==stream.frames&&stream.eof_calls==1&&!pcm_context);
}
static void test_resampled_wave(void) {
    TestPcmStream reference={.frames=48000,.channels=1,.tone=1};
    unsigned bytes=make_wave(reference.frames);u32(24,48000);u32(28,96000);
    for(unsigned i=0;i<reference.frames;i++)u16(44+i*2,(uint16_t)source_sample(&reference,i,0));
    position=0;assert(!audio_play_wav(file,bytes));memset(file,0,bytes);
    prefill_stream(&reference);assert(audio_status()->format==AUDIO_FORMAT_WAVE);
    assert(audio_status()->sample_rate==48000&&audio_status()->output_rate==44100);
    check_resampled_playback(&reference);assert(!reference.calls);
}
static void test_stream_replacement(void) {
    TestPcmStream stream={.frames=80000,.channels=1,.chunk=4096};
    load_stream(&stream,22050,stream.frames);prefill_stream(&stream);
    unsigned calls=stream.calls;
    assert(audio_play_pcm_stream(4999,1,1000,read_pcm,&stream)==MEDIA_UNSUPPORTED);
    assert(audio_play_pcm_stream(48001,1,1000,read_pcm,&stream)==MEDIA_UNSUPPORTED);
    assert(audio_play_pcm_stream(22050,3,1000,read_pcm,&stream)==MEDIA_UNSUPPORTED);
    assert(audio_play_pcm_stream(22050,1,0,read_pcm,&stream)==MEDIA_BAD_FILE);
    assert(audio_play_pcm_stream(22050,1,1000,0,&stream)==MEDIA_BAD_FILE);
    assert(audio_play(file,AUDIO_WORK_CAPACITY+1)==MEDIA_TOO_LARGE);
    assert(audio_play_wav(file,AUDIO_WORK_CAPACITY+1)==MEDIA_TOO_LARGE);
    assert(audio_status()->state==AUDIO_PLAYING&&pcm_context==&stream&&hardware_running);
    assert(stream.calls==calls&&audio_status()->total_frames==stream.frames);
    audio_clear_error();assert(audio_status()->error==MEDIA_OK);
    advance_stream(&stream,HALF_BYTES/2);assert(stream.calls==calls+1);
    unsigned bytes=make_wave(1000);
    assert(!audio_play(file,bytes));assert(!pcm_reader&&!pcm_context);
    calls=stream.calls;
    for(unsigned i=0;i<8;i++)audio_poll();
    assert(stream.calls==calls&&audio_status()->format==AUDIO_FORMAT_WAVE);
    /* Invalid stream arguments preserve a different active transport too. */
    assert(audio_play_pcm_stream(22050,1,0,read_pcm,&stream)==MEDIA_BAD_FILE);
    assert(audio_status()->state==AUDIO_PLAYING&&audio_status()->format==AUDIO_FORMAT_WAVE);
    stream=(TestPcmStream){.frames=80000,.channels=1,.chunk=4096};
    load_stream(&stream,22050,stream.frames);
    audio_pause(1);assert(audio_status()->state==AUDIO_PAUSED);
    bytes=make_wave(1000);assert(!audio_play_wav(file,bytes));
    assert(!pcm_reader&&!pcm_context&&!loading_paused);
    stream=(TestPcmStream){.frames=80000,.channels=1,.chunk=4096};
    load_stream(&stream,22050,stream.frames);poll_stream(&stream);
    audio_stop();calls=stream.calls;
    poll_stream(&stream);audio_pause(0);poll_stream(&stream);
    assert(stream.calls==calls&&audio_status()->state==AUDIO_STOPPED&&!pcm_reader&&!pcm_context);
    stream=(TestPcmStream){.frames=80000,.channels=1,.chunk=4096};
    load_stream(&stream,22050,stream.frames);prefill_stream(&stream);
    TestPcmStream replacement={.frames=1000,.channels=2,.chunk=500};
    calls=stream.calls;load_stream(&replacement,48000,replacement.frames);poll_stream(&replacement);
    assert(stream.calls==calls&&replacement.calls==1&&pcm_context==&replacement);
    audio_stop();
}
static void test_stream_errors(void) {
    TestPcmStream stream={.frames=80000,.channels=1,.chunk=4096};
    load_stream(&stream,22050,stream.frames);prefill_stream(&stream);
    stream.error=MEDIA_UNSUPPORTED;advance_stream(&stream,HALF_BYTES/2);
    assert(audio_status()->state==AUDIO_ERROR&&audio_status()->error==MEDIA_UNSUPPORTED);
    unsigned calls=stream.calls;poll_stream(&stream);
    assert(stream.calls==calls&&!pcm_reader&&!pcm_context&&!hardware_running);
    /* Explicit length disagreement is an error, not implicit duration repair. */
    stream=(TestPcmStream){.frames=1000,.channels=1,.chunk=4096};
    load_stream(&stream,22050,1001);poll_stream(&stream);poll_stream(&stream);
    assert(audio_status()->state==AUDIO_ERROR&&audio_status()->error==MEDIA_BAD_FILE);
    assert(audio_status()->total_frames==1001&&!pcm_context);
    stream=(TestPcmStream){.frames=1001,.channels=1,.chunk=4096};
    load_stream(&stream,22050,1000);poll_stream(&stream);
    assert(audio_status()->state==AUDIO_ERROR&&audio_status()->error==MEDIA_BAD_FILE);
    assert(audio_status()->total_frames==1000&&!pcm_context);
    assert(!audio_play_pcm_stream(22050,1,80000,oversized_result,0));audio_poll();
    assert(audio_status()->state==AUDIO_ERROR&&audio_status()->error==MEDIA_BAD_FILE);
    assert(!pcm_reader&&!pcm_context);
    stream=(TestPcmStream){.frames=80000,.channels=1,.chunk=4096};
    load_stream(&stream,22050,stream.frames);prefill_stream(&stream);
    advance_stream(&stream,HALF_BYTES/2);advance_stream(&stream,HALF_BYTES/2);
    assert(audio_status()->state==AUDIO_ERROR&&audio_status()->error==MEDIA_UNDERRUN);
    assert(audio_status()->underruns==1&&!pcm_reader&&!pcm_context);
}
static void test_mp3_replacement(const char *path) {
    FILE *input=fopen(path,"rb");assert(input);
    unsigned bytes=(unsigned)fread(file,1,sizeof file,input);assert(bytes&&feof(input));fclose(input);
    TestPcmStream stream={.frames=80000,.channels=1,.chunk=4096};
    load_stream(&stream,22050,stream.frames);prefill_stream(&stream);
    unsigned calls=stream.calls;
    assert(!audio_play(file,bytes));assert(audio_status()->format==AUDIO_FORMAT_MP3);
    assert(!pcm_reader&&!pcm_context);audio_poll();assert(stream.calls==calls);
    if(audio_status()->sample_rate==48000) {
        static int16_t samples[100000];
        MediaMp3 decoder;assert(!media_mp3_open(&decoder,file,bytes));
        unsigned decoded=0;
        while(!media_mp3_finished(&decoder)) {
            assert((decoded+4096)*decoder.channels<=sizeof samples/sizeof samples[0]);
            int count=media_mp3_read(&decoder,samples+decoded*decoder.channels,4096);
            assert(count>=0);decoded+=(unsigned)count;
        }
        TestPcmStream reference={.frames=decoded,.channels=decoder.channels,.samples=samples};
        prefill_stream(&reference);check_resampled_playback(&reference);
        assert(stream.calls==calls);
    }
    audio_stop();
}
int main(int argc,char **argv) {
    present=0;assert(audio_init()==MEDIA_NO_DEVICE);assert(audio_play(file,100)==MEDIA_NO_DEVICE);
    assert(audio_play_pcm_stream(22050,1,1000,read_pcm,0)==MEDIA_NO_DEVICE);
    initialized=0;present=1;assert(!audio_init());
    audio_set_volume(120);assert(audio_status()->volume==100);
    start(1000);assert(audio_duration_ms()==45);
    for(unsigned i=1000;i<RING_BYTES/2;i++)assert(((int16_t *)audio_test_dma)[i]==0);
    position=1000;now++;audio_poll();assert(audio_status()->played_frames==500);
    audio_pause(1);assert(audio_status()->state==AUDIO_PAUSED);
    now+=1000;audio_poll();assert(audio_status()->state==AUDIO_PAUSED);
    audio_pause(0);assert(audio_status()->state==AUDIO_PLAYING);
    position=2000;now++;audio_poll();assert(audio_status()->played_frames==1000);
    position=4400;now++;audio_poll();assert(audio_status()->state==AUDIO_FINISHED);
    assert(audio_status()->played_frames==1000);
    start(80000);
    for(unsigned round=0;round<3;round++) {
        position=(position+HALF_BYTES)%RING_BYTES;now++;
        audio_poll();
        for(unsigned i=0;i<3;i++)audio_poll();
        assert(audio_status()->state==AUDIO_PLAYING&&ready_mask==3);
    }
    audio_stop();assert(audio_status()->state==AUDIO_STOPPED);
    start(80000);position=HALF_BYTES;now++;audio_poll();
    position=0;now++;audio_poll();assert(audio_status()->state==AUDIO_ERROR&&audio_status()->error==MEDIA_UNDERRUN);
    start(80000);now+=200;audio_poll();assert(audio_status()->error==MEDIA_UNDERRUN);
    start(80000);busy=1;audio_pause(1);assert(audio_status()->error==MEDIA_DEVICE_ERROR);busy=0;
    assert(audio_play(file,AUDIO_WORK_CAPACITY+1)==MEDIA_TOO_LARGE);
    test_short_stream();test_stream_refills(1);test_stream_refills(2);
    test_stream_ring_boundary();test_stream_replacement();test_stream_errors();
    test_resampled_stream(1,96000,4096);test_resampled_stream(2,96000,1152);
    test_resampled_stream(2,3001,997);test_resampled_stream(1,47,1);
    test_resampled_wave();
    if(argc>1)test_mp3_replacement(argv[1]);
    puts("SB16 state, owned input, bounded PCM callbacks, 48k resampling, refills, pause, completion, replacement, errors, underrun, and timeout tests passed.");
    return 0;
}
