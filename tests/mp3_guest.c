#define FEATURE_TEST
#include "../src/kernel.c"
#include "../src/audio.h"
#include "media_fixture.h"
static void mp3_check(int ok,const char *message){if(!ok)panic(message);}
void feature_test(void) {
    mp3_check(!audio_init(),"MP3 needs SB16");audio_set_volume(100);
    int id=fs_create(fs_root(),"sample.mp3");mp3_check(id>=0,"MP3 file create");
    mp3_check(fs_write(id,(const char *)media_fixture,sizeof media_fixture)==sizeof media_fixture,"MP3 fixture too large");
    /* Establish a non-default x87 environment that each decoder call preserves. */
    unsigned cr0;__asm__ volatile("mov %%cr0,%0":"=r"(cr0));
    unsigned enabled=(cr0&~12u)|2u;__asm__ volatile("mov %0,%%cr0"::"r"(enabled):"memory");
    uint16_t control=0x0f7f,after=0;
    __asm__ volatile("fninit\n\tfldcw %0\n\tfld1"::"m"(control):"memory");
    mp3_check(!audio_play(fs_data(id),fs_size(id)),"MP3 file open");
    mp3_check(audio_status()->format==AUDIO_FORMAT_MP3,"MP3 detection");
    unsigned began=timer_ticks(),refresh=0;
    while(audio_status()->state==AUDIO_LOADING||audio_status()->state==AUDIO_PLAYING) {
        audio_poll();drain_8042();
        if(++refresh%12==0){draw_ui();flip_vga();}
        mp3_check(timer_ticks()-began<8*TIMER_HZ,"MP3 timeout");
        __asm__ volatile("hlt");
    }
    __asm__ volatile("fnstcw %0":"=m"(after));
    uint32_t number[2];__asm__ volatile("fstpl %0":"=m"(number));
    mp3_check(after==control&&number[0]==0&&number[1]==0x3ff00000,"MP3 x87 state changed");
    __asm__ volatile("fninit\n\tmov %0,%%cr0"::"r"(cr0):"memory");
    mp3_check(audio_status()->state==AUDIO_FINISHED,media_error_string(audio_status()->error));
    mp3_check(audio_status()->underruns==0,"MP3 underrun");
    platform_log("AUDIO-MP3-PASS\n");
}
