/* Actual SB16 MPEG-1/Layer-II playback and desktop synchronization.
 * Linked into disposable test disks only; metadata comes from independent
 * FFmpeg probing of an original generated fixture. */
#define FEATURE_TEST
#include "../src/kernel.c"
#include "../src/video.h"
#include "mpeg_av_metadata.h"
#ifndef MPEG_NATIVE_TASKS
#define MPEG_NATIVE_TASKS 0
#endif
#if MPEG_NATIVE_TASKS
#include "mpeg_native_examples.h"
static int native_first,native_second,native_saved;
static void native_quiet(const char *text){(void)text;}
static void native_pixel(int x,int y,int c){(void)x;(void)y;(void)c;}
#endif
#ifndef MPEG_AV_CONTROLS
#define MPEG_AV_CONTROLS 0
#endif
static unsigned max_poll_ticks,max_sync_lag;
static void require_av(int ok,const char *message){if(!ok)panic(message);}
#if MPEG_NATIVE_TASKS
static void native_command(const char *text){while(*text)term_char(*text++);term_enter();}
static unsigned native_saved_value(int owner){
    char path[]="/Documents/counter-1.txt";path[19]=(char)('1'+owner);
    int id=fs_resolve(fs_root(),path);require_av(id>=0,"MPEG native counter save missing");
    const char *p=fs_data(id);unsigned value=0;while(*p>='0'&&*p<='9')value=value*10+(unsigned)(*p++-'0');return value;
}
static void native_begin(int first){
    if(fs_find_child(fs_root(),"Documents")<0)require_av(fs_mkdir(fs_root(),"Documents")>=0,"MPEG native documents");
    native_first=first;native_command("start /Programs/counter.bex");
    wins[first].x=28;wins[first].y=74;wins[first].w=450;wins[first].h=278;
    open_term();native_second=win_front();native_command("start /Programs/counter.bex");
    wins[native_second].x=28;wins[native_second].y=374;wins[native_second].w=450;wins[native_second].h=278;
    require_av(term_task_running(native_first)&&term_task_running(native_second),"MPEG native counters did not start");
    term_task_key(native_first,'+');term_task_key(native_first,'+');term_task_key(native_second,'+');
    ProgramIO io={native_quiet,native_pixel,0,0,0,0};
    require_av(!process_task_start(6,mpeg_fpu_a,sizeof mpeg_fpu_a,&io),"MPEG x87 task A");
    require_av(!process_task_start(7,mpeg_fpu_b,sizeof mpeg_fpu_b,&io),"MPEG x87 task B");
}
static int native_poll(void){
    int changed=term_task_poll();
    require_av(term_task_running(native_first)&&term_task_running(native_second),"MPEG native counter ended");
    require_av(process_task_status(6)==PROCESS_TASK_READY&&process_task_status(7)==PROCESS_TASK_READY,"MPEG changed native x87 state");
    if(!native_saved&&audio_position_ms()>=1500){
        term_task_key(native_first,' ');term_task_key(native_second,' ');
        term_task_key(native_first,'s');term_task_key(native_second,'s');native_saved=1;
    }
    return changed;
}
static void native_finish(void){
    unsigned one=native_saved_value(native_first),two=native_saved_value(native_second);
    require_av(native_saved&&one>=23&&two>=13&&one>two&&one-two>=8&&one-two<=12,"MPEG native counters did not progress independently");
    term_task_close(native_first);require_av(!term_task_running(native_first)&&term_task_running(native_second),"MPEG native close isolation");
    term_task_stop(native_second);require_av(!term_task_running(native_second),"MPEG native stop");
    process_task_key(6,'q');process_task_key(7,'q');
    for(int i=0;i<5;++i){process_task_step(6);process_task_step(7);}
    require_av(process_task_status(6)==PROCESS_TASK_DONE&&!process_task_result(6)&&process_task_status(7)==PROCESS_TASK_DONE&&!process_task_result(7),"MPEG x87 tasks did not exit cleanly");
    process_task_clear(6);process_task_clear(7);
    platform_log("MPEG-NATIVE-COUNTERS ");kprint_uint(one);serial_write(' ');kprint_uint(two);serial_write('\n');
    platform_log("MPEG-NATIVE-X87-PASS\n");
}
#endif
static int av_poll(void){
    unsigned before=timer_ticks();audio_poll();int changed=player_tick();
#if MPEG_NATIVE_TASKS
    if(native_poll()) changed=PLAYER_CHANGED;
#endif
    unsigned ticks=timer_ticks()-before;if(ticks>max_poll_ticks)max_poll_ticks=ticks;
    drain_8042();
    while(kqn){uint8_t sc=kq[0];for(int i=1;i<kqn;++i)kq[i-1]=kq[i];--kqn;
        key_pressed=key_char=key_sc=0;keyboard_handle_byte(sc);if(key_pressed)handle_key();}
    uint16_t cw;__asm__ volatile("fnstcw %0":"=m"(cw));require_av(cw==0x0b7f,"MPEG changed x87 control");
    require_av(video_status()->state!=VIDEO_ERROR,video_error_string(video_status()->error));
    require_av(audio_status()->state!=AUDIO_ERROR,media_error_string(audio_status()->error));
    return changed;
}
static void av_show(void){render_scene();gfx_present();}
static unsigned hash_frame(void){
    const VideoFrame *f=video_frame();unsigned hash=2166136261u;
    const uint8_t *p[3]={f->y,f->cb,f->cr};
    for(unsigned c=0;c<3;++c){unsigned w=f->width>>(c!=0),h=f->height>>(c!=0),stride=c?f->chroma_stride:f->y_stride;
        for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x)hash=(hash^p[c][y*stride+x])*16777619u;}
    return hash;
}
static void await_av_state(int state){unsigned began=timer_ticks();while(video_status()->state!=state){av_poll();__asm__ volatile("hlt");require_av(timer_ticks()-began<TIMER_HZ*5,"MPEG keyboard timeout");}}
void feature_test(void){
    int id=fs_resolve(fs_root(),"/Video/frame-study.mpg");require_av(id>=0,"MPEG fixture missing");
    require_av(audio_status()->available,"MPEG requires SB16");audio_set_volume(100);
    open_term();int terminal=win_front();wins[terminal].x=28;wins[terminal].y=138;wins[terminal].w=450;wins[terminal].h=380;
#if MPEG_NATIVE_TASKS
    native_begin(terminal);
#endif
    int player=win_open(WK_PLAYER);require_av(player>=0,"MPEG player window");
    wins[player].x=510;wins[player].y=46;wins[player].w=PLAYER_W;wins[player].h=PLAYER_H;
    uint16_t control=0x0b7f;const uint32_t value[2]={0,0x3ff80000};
    __asm__ volatile("fninit\n\tfldcw %0\n\tfldl %1"::"m"(control),"m"(value):"memory");
    require_av(!player_open_file(id),"MPEG open");
    unsigned start=timer_ticks(),last=0,typed=MPEG_NATIVE_TASKS,paused=0,stopped=0;
    platform_log("MPEG-AV-START\n");
    while(video_status()->state!=VIDEO_FINISHED){
        int changed=av_poll();const VideoStatus *v=video_status();
        if(changed && v->state==VIDEO_LOADING) av_show();
        if(v->state!=VIDEO_LOADING){
            require_av(v->audio_enabled&&v->audio_error==0,"MPEG audio disabled");
            require_av(v->audio_sample_rate==AV_RATE&&v->audio_channels==AV_CHANNELS,"MPEG audio metadata");
            require_av(v->audio_lead_frames==AV_LEAD&&v->video_start_ms==AV_VIDEO_START,"MPEG PTS alignment");
            require_av(audio_status()->sample_rate==AV_RATE&&audio_status()->output_rate==AV_OUTPUT_RATE,"MPEG hardware output rate");
        }
        if(v->displayed_frames!=last){
            last=v->displayed_frames;
            if(changed==PLAYER_VIDEO_FRAME){
                Win *w=&wins[player];player_draw_playback(w->x+1,w->y+TITLE_H+1,w->w-2,w->h-TITLE_H-2);gfx_present();
            }else if(changed)av_show();
            audio_poll();
            if(!MPEG_AV_CONTROLS){platform_log("MPEG-FRAME ");kprint_uint(last);serial_write(' ');kprint_hex32(hash_frame());serial_write('\n');}
            if(changed&&last<v->total_frames&&audio_status()->state==AUDIO_PLAYING){
                unsigned due=AV_VIDEO_START+(last-1)*1000/25,clock=audio_position_ms();
                require_av(clock+1>=due,"MPEG video led audio clock");
                unsigned lag=clock>due?clock-due:0;if(lag>max_sync_lag)max_sync_lag=lag;
                if(lag>=125){platform_log("MPEG-LAG ");kprint_uint(last);serial_write(' ');kprint_uint(clock);serial_write(' ');kprint_uint(due);serial_write('\n');}
                require_av(lag<125,"MPEG A/V drift beyond125ms");
            }
            if(last>=10&&!typed){
                win_focus(terminal);const char *text="echo MPEG video + MP2 audio are playing";
                while(*text){key_sc=0;key_char=*text++;handle_key();}key_sc=KEY_ENTER;key_char=0;handle_key();
                require_av(!kstrcmp(term_get(term_count()-1),"MPEG video + MP2 audio are playing"),"MPEG desktop response");
                win_focus(player);av_show();typed=1;
            }
        }
        if(MPEG_AV_CONTROLS&&!paused&&last>=20){
            platform_log("MPEG-WAIT-PAUSE\n");await_av_state(VIDEO_PAUSED);
            unsigned frame=v->displayed_frames,pos=audio_status()->played_frames,hash=hash_frame();
            av_show();platform_log("MPEG-PAUSED-SCREEN\n");unsigned began=timer_ticks();
            while(timer_ticks()-began<TIMER_HZ){av_poll();require_av(v->state==VIDEO_PAUSED&&v->displayed_frames==frame&&audio_status()->played_frames==pos&&hash_frame()==hash,"MPEG pause advanced");__asm__ volatile("hlt");}
            platform_log("MPEG-WAIT-RESUME\n");await_av_state(VIDEO_PLAYING);paused=1;
        }
        if(MPEG_AV_CONTROLS&&!stopped&&last>=40){
            platform_log("MPEG-WAIT-STOP\n");await_av_state(VIDEO_STOPPED);
            require_av(audio_status()->state==AUDIO_STOPPED&&!video_frame(),"MPEG stop left transport active");
            av_show();platform_log("MPEG-STOPPED-SCREEN\nMPEG-WAIT-REPLAY\n");await_av_state(VIDEO_LOADING);
            last=0;stopped=1;
        }
        require_av(timer_ticks()-start<TIMER_HZ*30,"MPEG playback timeout");__asm__ volatile("hlt");
    }
    require_av(last==AV_FRAMES&&video_status()->displayed_frames==AV_FRAMES,"MPEG final video frame");
    require_av(audio_status()->state==AUDIO_FINISHED&&audio_status()->played_frames==AV_AUDIO_FRAMES+AV_LEAD,"MPEG audio tail lost");
    require_av(!audio_status()->underruns,"MPEG audio underrun");
    require_av(video_position_ms()==video_duration_ms(),"MPEG completion position");
#if MPEG_NATIVE_TASKS
    native_finish();
#endif
    uint32_t after[2];__asm__ volatile("fstpl %0\n\tfninit":"=m"(after)::"memory");
    require_av(after[0]==value[0]&&after[1]==value[1],"MPEG changed x87 register");
    av_show();platform_log("MPEG-FINISHED-SCREEN\n");
    platform_log("MPEG-MAX-SYNC-LAG-MS ");kprint_uint(max_sync_lag);serial_write('\n');
    platform_log("MPEG-MAX-POLL-TICKS ");kprint_uint(max_poll_ticks);serial_write('\n');
    platform_log("MPEG-PRESENTATION-SKIPS ");kprint_uint(video_status()->presentation_skips);serial_write('\n');
    platform_log("MPEG-SOURCE-FRAMES ");kprint_uint(audio_status()->played_frames);serial_write('\n');
    platform_log("MPEG-AV-PASS\n");
}
