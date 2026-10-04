/* Real network/audio work between independent native desktop task slices. */
#define FEATURE_TEST
#include "../src/kernel.c"
#include "../src/browser.h"
#include "media_fixture.h"
#include "task_media_examples.h"
#ifndef HTTP_PORT
#define HTTP_PORT "18080"
#endif
#define TASK_ORIGIN "http://10.0.2.2:" HTTP_PORT
static void check(int ok,const char *why){if(!ok)panic(why);}
static void command(const char *text){while(*text)term_char(*text++);term_enter();}
static void quiet(const char *text){(void)text;}
static void pixel(int x,int y,int c){(void)x;(void)y;(void)c;}
static void draw_test(void){
    cursor_restore();draw_desktop();draw_ui();
    browser_draw(625,88,620,555);flip_vga();cursor_on=0;
}
static unsigned saved_counter(int owner){
    char path[]="/Documents/counter-1.txt";path[19]=(char)('0'+owner);
    int id=fs_resolve(fs_root(),path);check(id>=0,"counter save missing");
    const char *text=fs_data(id);unsigned n=0;
    while(*text>='0'&&*text<='9')n=n*10+(unsigned)(*text++-'0');return n;
}
void feature_test(void){
    check(net_status()->available,"task media needs RTL8139");
    check(audio_status()->available,"task media needs SB16");
    int programs=fs_find_child(fs_root(),"Programs"),id=fs_find_child(programs,"counter.bex");
    if(id<0){id=fs_create(programs,"counter.bex");check(id>=0,"counter install");check(fs_write(id,(const char *)sdk_counter,sizeof sdk_counter)==sizeof sdk_counter,"counter write");}
    open_term();int first=win_front();command("start /Programs/counter.bex");
    wins[first].x=22;wins[first].y=80;wins[first].w=580;wins[first].h=285;
    open_term();int second=win_front();command("start /Programs/counter.bex");
    wins[second].x=22;wins[second].y=385;wins[second].w=580;wins[second].h=285;
    check(first==0&&second==1&&term_task_running(first)&&term_task_running(second),"counter owners");
    term_task_key(first,'+');term_task_key(first,'+');term_task_key(second,'+');
    ProgramIO io={quiet,pixel,0,0};
    check(!process_task_start(6,task_fpu_a,sizeof task_fpu_a,&io),"media FPU task A");
    check(!process_task_start(7,task_fpu_b,sizeof task_fpu_b,&io),"media FPU task B");
    browser_init();browser_open(TASK_ORIGIN "/slow");
    audio_set_volume(100);check(!audio_play(media_fixture,sizeof media_fixture),"MP3 playback start");
    unsigned began=timer_ticks(),last_draw=began,draws=0;int stopped=0,saved=0,heard=0;
    while(timer_ticks()-began<6*TIMER_HZ){
        unsigned now=timer_ticks();poll_time();platform_poll();browser_tick();term_task_poll();
        check(process_task_status(6)!=PROCESS_TASK_DONE&&process_task_status(7)!=PROCESS_TASK_DONE,"MP3 changed task x87 state");
        if(!stopped&&net_http_result()->state==NET_HTTP_RECEIVING){
            unsigned input_start=timer_ticks();browser_key(0x26,0,BROWSER_MOD_CTRL);browser_key(0,'x',0);
            draw_test();check(timer_ticks()-input_start<TIMER_HZ,"UI blocked by native tasks or network");
            browser_key(KEY_ESC,0,0);check(!browser_loading()&&!net_busy(),"browser cancel blocked");
            browser_open(TASK_ORIGIN "/index");stopped=1;
        }
        if(now-began>=2*TIMER_HZ&&audio_status()->played_frames>22050)heard=1;
        if(!saved&&now-began>=3*TIMER_HZ){
            term_task_key(first,' ');term_task_key(second,' ');
            term_task_key(first,'s');term_task_key(second,'s');saved=1;
        }
        if(now-last_draw>=7){draw_test();draws++;last_draw=timer_ticks();}
        if(now-began>5*TIMER_HZ&&audio_status()->state==AUDIO_FINISHED)break;
        __asm__ volatile("hlt");
    }
    check(stopped&&!browser_loading()&&!kstrcmp(browser_title(),"Native task HTTP"),"HTTP work did not finish alongside native tasks");
    check(heard&&audio_status()->state==AUDIO_FINISHED&&audio_status()->underruns==0,"MP3 stalled or underrun alongside native tasks");
    check(audio_status()->played_frames==audio_status()->total_frames,"MP3 tail lost");
    check(draws>=10&&term_task_running(first)&&term_task_running(second),"desktop task progress or redraw failed");
    unsigned a=saved_counter(first+1),b=saved_counter(second+1);
    check(a>=22&&b>=12&&a>b&&a-b>=8&&a-b<=12,"independent counter saves");
    term_task_close(first);check(!term_task_running(first)&&term_task_running(second),"counter close isolation");
    term_task_stop(second);check(!term_task_running(second),"counter keyboard stop");
    process_task_key(6,'q');process_task_key(7,'q');
    for(int i=0;i<5;i++){process_task_step(6);process_task_step(7);}
    check(process_task_status(6)==PROCESS_TASK_DONE&&!process_task_result(6)&&process_task_status(7)==PROCESS_TASK_DONE&&!process_task_result(7),"FPU tasks failed during MP3");
    process_task_clear(6);process_task_clear(7);
    check(!fs_sync(),"counter persistence flush");
    draw_test();platform_log("TASK-MEDIA-HTTP-PASS\n");
    for(;;)__asm__ volatile("hlt");
}
