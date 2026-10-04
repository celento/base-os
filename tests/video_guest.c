/* Real Media Player, decoded video, desktop dispatch, and x87 preservation.
 * Linked only into the disposable QEMU image built by tools/video_test.py. */
#define FEATURE_TEST
#include "../src/kernel.c"
#include "../src/video.h"
static void require_video(int ok,const char *message){if(!ok)panic(message);}
static unsigned video_max_poll_ticks;
static unsigned video_frame_hash(void) {
    const VideoFrame *frame=video_frame();
    require_video(frame!=0,"video output missing");
    unsigned hash=2166136261u;
    const uint8_t *planes[]={frame->y,frame->cb,frame->cr};
    for(unsigned c=0;c<3;++c){
        unsigned w=frame->width>>(c!=0),h=frame->height>>(c!=0);
        unsigned stride=c?frame->chroma_stride:frame->y_stride;
        for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x)hash=(hash^planes[c][y*stride+x])*16777619u;
    }
    return hash;
}
static void video_pump(void) {
    unsigned start=timer_ticks();
    player_tick();
    unsigned elapsed=timer_ticks()-start;
    if(elapsed>video_max_poll_ticks)video_max_poll_ticks=elapsed;
    audio_poll();drain_8042();
    while(kqn>0){
        uint8_t sc=kq[0];for(int j=1;j<kqn;++j)kq[j-1]=kq[j];--kqn;
        key_pressed=key_char=key_sc=0;keyboard_handle_byte(sc);if(key_pressed)handle_key();
    }
    uint16_t control;
    __asm__ volatile("fnstcw %0":"=m"(control));
    require_video(control==0x0b7f,"video altered x87 control word");
    require_video(video_status()->state!=VIDEO_ERROR,"video decoder error");
}
static void video_show(void){render_scene();gfx_present();}
static void await_state(int state,unsigned limit){
    unsigned start=timer_ticks();
    while(video_status()->state!=state){video_pump();__asm__ volatile("hlt");require_video(timer_ticks()-start<limit,"video input wait timeout");}
}
void feature_test(void) {
    int file=fs_resolve(fs_root(),"/Video/frame-study.mpg");
    require_video(file>=0&&fs_size(file)>16384,"video data volume fixture missing");
    open_term();int terminal=win_front();
    wins[terminal].x=28;wins[terminal].y=138;wins[terminal].w=450;wins[terminal].h=380;
    const char *intro="echo Video plays inside the BaseOS kernel";
    while(*intro)term_char(*intro++);term_enter();
    int player=win_open(WK_PLAYER);require_video(player>=0,"video window");
    wins[player].x=510;wins[player].y=46;wins[player].w=PLAYER_W;wins[player].h=PLAYER_H;
    require_video(player_open_file(file)==MEDIA_OK,"video file open");
    uint16_t control=0x0b7f;
    const uint32_t expected_value[2]={0,0x3ff80000}; /* exactly 1.5 */
    __asm__ volatile("fninit\n\tfldcw %0\n\tfldl %1"::"m"(control),"m"(expected_value):"memory");
    unsigned start=timer_ticks(),last=0,typed=0;
    while(video_status()->displayed_frames<20){
        video_pump();
        if(video_status()->displayed_frames!=last){
            last=video_status()->displayed_frames;video_show();
            if(last>=10&&!typed){
                win_focus(terminal);const char *text="echo Desktop remained responsive";
                while(*text){key_sc=0;key_char=*text++;handle_key();}key_sc=KEY_ENTER;key_char=0;handle_key();
                require_video(!kstrcmp(term_get(term_count()-1),"Desktop remained responsive"),"video desktop keyboard response");
                win_focus(player);typed=1;
            }
        }
        require_video(timer_ticks()-start<TIMER_HZ*15,"video initial playback timeout");
        __asm__ volatile("hlt");
    }
    platform_log("VIDEO-WAIT-PAUSE\n");await_state(VIDEO_PAUSED,TIMER_HZ*4);
    unsigned paused_frame=video_status()->displayed_frames,paused_position=video_position_ms(),hash=video_frame_hash();
    video_show();platform_log("VIDEO-PAUSED-SCREEN\n");
    start=timer_ticks();
    while(timer_ticks()-start<TIMER_HZ){
        video_pump();require_video(video_status()->state==VIDEO_PAUSED,"video pause state unstable");
        require_video(video_status()->displayed_frames==paused_frame&&video_position_ms()==paused_position&&video_frame_hash()==hash,"video pause advanced");
        __asm__ volatile("hlt");
    }
    platform_log("VIDEO-WAIT-RESUME\n");await_state(VIDEO_PLAYING,TIMER_HZ*4);
    while(video_status()->displayed_frames<40){video_pump();video_show();__asm__ volatile("hlt");}
    platform_log("VIDEO-WAIT-STOP\n");await_state(VIDEO_STOPPED,TIMER_HZ*4);
    require_video(!video_frame()&&video_position_ms()==0,"video stop did not reset");
    video_show();platform_log("VIDEO-STOPPED-SCREEN\n");
    platform_log("VIDEO-WAIT-REPLAY\n");await_state(VIDEO_LOADING,TIMER_HZ*4);
    last=0;start=timer_ticks();
    while(video_status()->state!=VIDEO_FINISHED){
        video_pump();
        if(video_status()->displayed_frames!=last){
            last=video_status()->displayed_frames;
            platform_log("VIDEO-FRAME ");kprint_uint(last);serial_write(' ');kprint_hex32(video_frame_hash());serial_write('\n');
            video_show();
        }
        require_video(timer_ticks()-start<TIMER_HZ*20,"video completion timeout");__asm__ volatile("hlt");
    }
    require_video(last==75&&video_position_ms()==3000&&video_duration_ms()==3000,"video final frame or duration");
    require_video(audio_status()->state==AUDIO_STOPPED,"silent video started audio device");
    uint32_t restored_value[2];__asm__ volatile("fstpl %0\n\tfninit":"=m"(restored_value)::"memory");
    require_video(restored_value[0]==expected_value[0]&&restored_value[1]==expected_value[1],"video altered x87 register");
    video_show();platform_log("VIDEO-FINISHED-SCREEN\n");
    platform_log("VIDEO-MAX-POLL-TICKS ");kprint_uint(video_max_poll_ticks);serial_write('\n');
    platform_log("VIDEO-LATE-RESYNCS ");kprint_uint(video_status()->late_resyncs);serial_write('\n');
    platform_log("VIDEO-QEMU-PASS\n");
}
