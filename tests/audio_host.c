#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define AUDIO_HOST_TEST
#include "../src/audio.c"

uint8_t audio_test_dma[RING_BYTES],audio_test_source[AUDIO_WORK_CAPACITY];
static uint8_t response[3],read_index,response_length;
static unsigned now,position,flip,present=1,busy;
uint32_t timer_ticks(void){return now;}
void audio_out(uint16_t port,uint8_t value) {
    if(port==SB_BASE+6&&value==0) {response[0]=0xaa;response_length=1;read_index=0;}
    if(port==DSP_WRITE&&value==0xe1) {response[0]=4;response[1]=5;response_length=2;read_index=0;}
    if(port==0xd8)flip=0;
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
int main(void) {
    present=0;assert(audio_init()==MEDIA_NO_DEVICE);assert(audio_play(file,100)==MEDIA_NO_DEVICE);
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
    puts("SB16 state, owned input, bounded refill, pause, completion, underrun, and timeout tests passed.");
}
