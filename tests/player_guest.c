#define FEATURE_TEST
#include "../src/kernel.c"
#include "../src/audio.h"
#include "../src/player.h"
#include "media_fixture.h"
static void player_check(int ok,const char *message){if(!ok)panic(message);}
static void show_player(void) {
    render_scene();
    int x=(fb_w-PLAYER_W)/2,y=90;
    gui_draw_window(x,y,PLAYER_W,PLAYER_H,"Media Player",0,0);
    player_draw(x+1,y+TITLE_H+1,PLAYER_W-2,PLAYER_H-TITLE_H-2);
    flip_vga();
}
void feature_test(void) {
    player_check(!audio_init(),"player requires SB16");
    int music=fs_mkdir(fs_root(),"Music");player_check(music>=0,"music folder");
    const char *names[]={"First light.mp3","Evening study.mp3","Stereo sketch.mp3","Wave study.mp3","Quiet room.mp3","Small hours.mp3"};
    int first=-1;
    for(unsigned i=0;i<sizeof(names)/sizeof(*names);i++) {
        int id=fs_create(music,names[i]);player_check(id>=0,"player file");
        player_check(fs_write(id,(const char *)media_fixture,sizeof media_fixture)==sizeof media_fixture,"player fixture size");
        if(!i)first=id;
    }
    player_init();player_check(!player_open_file(first),"player open MP3");
    unsigned start=timer_ticks();
    while(audio_status()->state==AUDIO_LOADING) {
        audio_poll();__asm__ volatile("hlt");
        player_check(timer_ticks()-start<TIMER_HZ*3,"player buffer timeout");
    }
    while(audio_position_ms()<350) {
        audio_poll();player_tick();__asm__ volatile("hlt");
        player_check(timer_ticks()-start<TIMER_HZ*4,"player progress timeout");
    }
    player_check(player_key(KEY_SPACE,0),"player pause key");
    player_check(audio_status()->state==AUDIO_PAUSED,"player paused state");
    player_key(0,'-');player_key(0,'-');
    show_player();platform_log("PLAYER-SCREEN\n");timer_delay(2*TIMER_HZ);
    player_check(audio_status()->state==AUDIO_PAUSED,"pause is stable");
    player_check(player_key(KEY_SPACE,0),"player resume key");
    start=timer_ticks();
    while(audio_status()->state==AUDIO_PLAYING) {
        audio_poll();player_tick();__asm__ volatile("hlt");
        player_check(timer_ticks()-start<TIMER_HZ*4,"player finish timeout");
    }
    player_check(audio_status()->state==AUDIO_FINISHED,"player finished state");
    player_key(KEY_DOWN,0);player_check(player_key(KEY_ENTER,0),"playlist keyboard open");
    player_check(audio_status()->state==AUDIO_LOADING,"playlist starts new track");
    player_key(0,'s');player_check(audio_status()->state==AUDIO_STOPPED,"player stop");
    player_key(0,'r');
    player_check(audio_status()->volume==65,"player volume controls");
    platform_log("PLAYER-UI-PASS\n");
}
