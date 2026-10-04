#define FEATURE_TEST
#include "../src/kernel.c"
#include "../src/audio.h"
static uint8_t *const test_wave=(uint8_t *)0x600000;
static void au16(unsigned p,unsigned v){test_wave[p]=v;test_wave[p+1]=v>>8;}
static void au32(unsigned p,unsigned v){au16(p,v);au16(p+2,v>>16);}
static void audio_check(int condition,const char *message){if(!condition)panic(message);}
static unsigned build_wave(unsigned frames,unsigned rate,unsigned bits,unsigned channels) {
    unsigned size=frames*channels*(bits/8);kmemset(test_wave,0,size+44);
    kmemcpy(test_wave,"RIFF",4);au32(4,size+36);kmemcpy(test_wave+8,"WAVEfmt ",8);
    au32(16,16);au16(20,1);au16(22,channels);au32(24,rate);au32(28,rate*channels*(bits/8));
    au16(32,channels*(bits/8));au16(34,bits);kmemcpy(test_wave+36,"data",4);au32(40,size);
    for(unsigned frame=0;frame<frames;frame++)for(unsigned ch=0;ch<channels;ch++) {
        /* 441/882 Hz stereo, or 315 Hz mono. Analytically checked by host. */
        unsigned period=channels==1?70:(ch?50:100);
        int phase=frame%period;
        int sample=phase<(int)period/2?-12000+phase*48000/(int)period:36000-phase*48000/(int)period;
        if(bits==8)test_wave[44+frame]=(uint8_t)(128+sample/256);
        else au16(44+(frame*channels+ch)*2,(unsigned)sample);
    }
    return size+44;
}
static void play_wait(unsigned max_ticks) {
    uint32_t start=timer_ticks();unsigned refresh=0;
    while(audio_status()->state==AUDIO_LOADING||audio_status()->state==AUDIO_PLAYING) {
        audio_poll();drain_8042();
        if(++refresh%12==0){draw_ui();flip_vga();}
        audio_check(timer_ticks()-start<max_ticks,"audio playback timeout");
        __asm__ volatile("hlt");
    }
    audio_check(audio_status()->state==AUDIO_FINISHED,media_error_string(audio_status()->error));
    audio_check(audio_status()->underruns==0,"audio underrun");
}
void feature_test(void) {
    audio_check(audio_init()==0,"SB16 absent");audio_set_volume(100);
    platform_log("AUDIO: 8-bit mono file\n");
    unsigned size=build_wave(12000,22050,8,1);
    int id=fs_create(fs_root(),"audio-test.wav");audio_check(id>=0,"audio file create");
    audio_check(fs_write(id,(const char *)test_wave,size)==(int)size,"audio file write");
    audio_check(audio_play_wav(fs_data(id),fs_size(id))==0,"audio file open");
    audio_check(fs_write(id,"changed",7)==7,"audio source replacement");
    play_wait(5*TIMER_HZ);
    timer_delay(TIMER_HZ/4);
    platform_log("AUDIO: 16-bit stereo streaming\n");
    size=build_wave(88200,44100,16,2);
    audio_check(audio_play_wav(test_wave,size)==0,"stereo open");
    play_wait(6*TIMER_HZ);
    audio_check(audio_status()->played_frames==88200,"exact stereo duration");
    platform_log("AUDIO-WAV-PASS\n");
}
