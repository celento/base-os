#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "audio.h"
#include "video.h"
#include "app.h"
#include "fs.h"
#include "layout.h"
static uint8_t test_mirror[FB_CAPACITY];
#undef PRESENT_BASE
#define PRESENT_BASE ((uintptr_t)test_mirror)
#define GFX_HOST_TEST
#include "../src/gfx.c"

static char names[FS_MAX_NODES][FS_NAME_LEN];
static unsigned generations[FS_MAX_NODES];
static int parents[FS_MAX_NODES], valid[FS_MAX_NODES];
static unsigned now;
static unsigned capacity=393216;
static AudioStatus fake={.available=1,.volume=75};
static VideoStatus fake_video;
static int fake_frame;
void kmemset(void *d,int v,int n){memset(d,v,(size_t)n);}
void kmemcpy(void *d,const void *s,int n){memmove(d,s,(size_t)n);}
int kstrlen(const char *s){return (int)strlen(s);}
int kstrcmp(const char *a,const char *b){return strcmp(a,b);}
int fs_root(void){return 0;}
int fs_valid(int id){return id>=0&&id<FS_MAX_NODES&&valid[id];}
int fs_is_dir(int id){return id==0||id==1;}
int fs_is_app(int id){(void)id;return 0;}
const char *fs_name(int id){return fs_valid(id)?names[id]:"";}
unsigned fs_identity(int id){return fs_valid(id)?generations[id]:0;}
int fs_parent(int id){return parents[id];}
int fs_find_child(int p,const char *s){return p==0&&!strcmp(s,"trash")?1:-1;}
int fs_size(int id){(void)id;return 4096;}
const char *fs_data(int id){(void)id;return "MP3";}
uint32_t timer_ticks(void){return now;}
const AudioStatus *audio_status(void){return &fake;}
uint32_t audio_capacity_bytes(void){return capacity;}
uint32_t audio_position_ms(void){return fake.played_frames/22;}
uint32_t audio_duration_ms(void){return fake.total_frames/22;}
int audio_play(const void *data,uint32_t bytes){(void)data;(void)bytes;fake.state=AUDIO_PLAYING;fake.error=0;fake.format=AUDIO_FORMAT_MP3;fake.channels=2;fake.sample_rate=22050;fake.total_frames=22050;fake.played_frames=0;return 0;}
void audio_pause(int paused){fake.state=paused?AUDIO_PAUSED:AUDIO_PLAYING;}
void audio_stop(void){fake.state=AUDIO_STOPPED;fake.error=0;fake.played_frames=0;}
void audio_set_volume(unsigned n){fake.volume=n>100?100:n;}
void audio_clear_error(void){fake.error=0;}
const VideoStatus *video_status(void){return &fake_video;}
int video_play(const void *data,uint32_t bytes){(void)data;(void)bytes;fake_video=(VideoStatus){.state=VIDEO_LOADING,.width=320,.height=240,.fps_num=25,.fps_den=1,.total_frames=75};return 0;}
void video_pause(int paused){fake_video.state=paused?VIDEO_PAUSED:VIDEO_PLAYING;}
void video_stop(void){fake_video.state=VIDEO_STOPPED;fake_video.displayed_frames=0;fake_video.error=0;}
void video_clear_error(void){fake_video.error=0;}
int video_poll(void){int frame=fake_frame;fake_frame=0;return frame;}
uint32_t video_position_ms(void){return fake_video.displayed_frames*40;}
uint32_t video_duration_ms(void){return fake_video.total_frames*40;}
const char *video_error_string(int error){(void)error;return "Video unavailable";}
const char *media_error_string(int error){(void)error;return "Audio unavailable";}
uint8_t app_accent=9,app_accent_dk=13,app_text=0,app_text_dim=2,app_chrome=3,app_chrome_dk=2;
void video_draw(int x,int y,int w,int h){draw_rect(x,y,w,h,COLOR_BLACK);}
void fmt_uint(char *s,unsigned n){sprintf(s,"%u",n);}
void fmt_pad2(char *s,int n){sprintf(s,"%02d",n);}
#include "../src/player.c"
enum { TEST_W=1024, TEST_H=768, OUTSIDE=253 };
static uint8_t back[TEST_W*TEST_H],linear[TEST_W*TEST_H*4];
static uint8_t before[sizeof back],reference[sizeof back];
static void assert_untouched_outside(const uint8_t *old,int x,int y,int w,int h){
    for(int row=0;row<TEST_H;row++)for(int col=0;col<TEST_W;col++)
        if(col<x||col>=x+w||row<y||row>=y+h)
            assert(back[row*TEST_W+col]==old[row*TEST_W+col]);
}
static void draw_at(int width,int height){
    memset(back,OUTSIDE,sizeof back);memcpy(before,back,sizeof back);
    player_draw(25,30,width,height);
    assert(memcmp(before,back,sizeof back));
    assert_untouched_outside(before,25,30,width,height);
}
static void expect_full_change(void){
    fake.played_frames+=2200;
    assert(player_tick()==PLAYER_CHANGED);
    assert(!player_tick());
}
static void test_tick_classification(void){
    assert(player_open_file(3)==MEDIA_OK);
    assert(player_tick()==PLAYER_CHANGED);assert(!player_tick());
    fake.played_frames=2199;assert(!player_tick());
    fake.played_frames=2200;assert(player_tick()==PLAYER_AUDIO_PROGRESS);assert(!player_tick());
    fake.played_frames=4378;assert(!player_tick());
    fake.played_frames=4400;assert(player_tick()==PLAYER_AUDIO_PROGRESS);
    fake.played_frames=0;assert(player_tick()==PLAYER_AUDIO_PROGRESS);

    /* State, error and volume always dominate a simultaneous position change. */
    static const int states[]={AUDIO_LOADING,AUDIO_PLAYING,AUDIO_PAUSED,AUDIO_PLAYING,
        AUDIO_STOPPED,AUDIO_PLAYING,AUDIO_FINISHED,AUDIO_ERROR,AUDIO_PLAYING};
    for(unsigned i=0;i<sizeof states/sizeof states[0];i++){
        fake.state=states[i];expect_full_change();
    }
    fake.error=MEDIA_BAD_FILE;expect_full_change();assert(message[0]);
    fake.error=MEDIA_NO_DEVICE;expect_full_change();
    assert(player_click(25,30,420,390,25+220,30+185));
    expect_full_change();assert(!message[0]);
    fake.volume--;expect_full_change();fake.volume++;expect_full_change();

    /* Decoder metadata and the footer's capacity cannot use a progress-only
     * redraw even if the transport enum and volume did not change. */
    fake.available=0;expect_full_change();fake.available=1;expect_full_change();
    fake.format=AUDIO_FORMAT_WAVE;expect_full_change();
    fake.sample_rate=44100;expect_full_change();
    fake.channels=1;expect_full_change();
    fake.bits_per_sample=16;expect_full_change();
    fake.output_rate=44100;expect_full_change();
    fake.total_frames+=22000;expect_full_change();
    fake.underruns++;expect_full_change();
    capacity+=1024;expect_full_change();

    /* Explicit library, selection, source/title and message changes remain
     * full redraws, including failed opens that leave audio state unchanged. */
    player_refresh();expect_full_change();
    assert(player_key(KEY_UP,0));expect_full_change();
    draw_at(640,584);
    assert(player_click(25,30,640,584,25+70,30+303));expect_full_change();
    assert(player_open_file(4)==MEDIA_OK);expect_full_change();
    assert(player_open_file(FS_MAX_NODES)==MEDIA_BAD_FILE);expect_full_change();
    assert(player_click(25,30,420,390,25+220,30+185));expect_full_change();
    assert(!player_key(0,'?'));assert(!player_tick());

    now=refresh_time+TIMER_HZ-1;
    fake.played_frames+=2200;assert(player_tick()==PLAYER_AUDIO_PROGRESS);
    now++;fake.played_frames+=2200;assert(player_tick()==PLAYER_CHANGED);
    assert(!player_tick());
    /* Refresh discovers changed playlist labels, even with stable identities. */
    strcpy(names[4],"Renamed.wav");now+=TIMER_HZ;
    fake.played_frames+=2200;assert(player_tick()==PLAYER_CHANGED);

    /* Video keeps its existing distinct frame path; video progress is never
     * mistaken for an audio-only update. */
    assert(player_open_file(2)==MEDIA_OK);assert(player_tick()==PLAYER_CHANGED);
    fake_video.state=VIDEO_PLAYING;assert(player_tick()==PLAYER_CHANGED);
    fake_video.displayed_frames++;fake_frame=1;
    assert(player_tick()==PLAYER_VIDEO_FRAME);assert(!player_tick());
    fake_video.displayed_frames++;fake_frame=1;fake.volume--;
    assert(player_tick()==PLAYER_CHANGED);
    fake_video.state=VIDEO_PAUSED;fake_frame=1;assert(player_tick()==PLAYER_CHANGED);
    fake_frame=1;assert(player_tick()==PLAYER_CHANGED);
    assert(player_open_file(3)==MEDIA_OK);assert(player_tick()==PLAYER_CHANGED);
    fake_frame=1;fake.played_frames+=2200;assert(player_tick()==PLAYER_CHANGED);
    player_close();assert(player_tick()==PLAYER_CHANGED);assert(!player_tick());
}
static void assert_progress_equivalent(int x,int y,int width,int height,unsigned position){
    memcpy(before,back,sizeof back);
    fake.played_frames=position*22;
    player_draw_audio_progress(x,y,width,height);
    assert_untouched_outside(before,x+30,y+114,width-60,16+UI_FONT_H);
    memcpy(reference,back,sizeof back);
    /* Repeating an identical update must not darken antialiased glyphs or
     * rounded edges by blending against their previous drawing. */
    for(int repeat=0;repeat<8;repeat++){
        player_draw_audio_progress(x,y,width,height);
        assert(!memcmp(back,reference,sizeof back));
    }
    /* Compare every backbuffer pixel against a fresh full client draw. The
     * surrounding desktop is patterned so accidental overdraw also fails. */
    memcpy(back,before,sizeof back);
    player_draw(x,y,width,height);
    assert(!memcmp(back,reference,sizeof back));
}
static void test_progress_pixels(void){
    assert(player_open_file(3)==MEDIA_OK);assert(player_tick()==PLAYER_CHANGED);
    static const unsigned durations[]={0,9500,10000,65535,65536,600000,7200000};
    static const unsigned positions[]={0,1,99,100,999,1000,5900,59900,60000,599999,600000,0};
    static const int geometry[][4]={{25,30,420,362},{7,9,640,584},{92,103,840,640}};
    for(int theme=0;theme<3;theme++){
        app_chrome=theme==0?COLOR_LTGRAY:theme==1?COLOR_WHITE:COLOR_NAVY;
        app_chrome_dk=theme==2?COLOR_BLUE:COLOR_GRAY;
        app_text_dim=theme==2?COLOR_WHITE:COLOR_GRAY;
        for(unsigned g=0;g<sizeof geometry/sizeof geometry[0];g++){
            int x=geometry[g][0],y=geometry[g][1],w=geometry[g][2],h=geometry[g][3];
            for(unsigned d=0;d<sizeof durations/sizeof durations[0];d++){
                fake.total_frames=durations[d]*22;fake.played_frames=0;
                for(unsigned p=0;p<sizeof back;p++)back[p]=(uint8_t)(p*37u+11u);
                player_draw(x,y,w,h);
                for(unsigned p=0;p<sizeof positions/sizeof positions[0];p++)
                    assert_progress_equivalent(x,y,w,h,positions[p]);
                assert_progress_equivalent(x,y,w,h,durations[d]);
                assert_progress_equivalent(x,y,w,h,0);
            }
        }
    }
    /* Invalid/small clients and the video client must be left untouched. */
    memcpy(before,back,sizeof back);
    player_draw_audio_progress(25,30,419,390);
    player_draw_audio_progress(25,30,420,361);
    assert(!memcmp(back,before,sizeof back));
    assert(player_open_file(2)==MEDIA_OK);
    player_draw_audio_progress(25,30,640,584);
    assert(!memcmp(back,before,sizeof back));
    app_chrome=COLOR_LTGRAY;app_chrome_dk=app_text_dim=COLOR_GRAY;
}
int main(void){
    gfx_init(back,linear,TEST_W,TEST_H,32,TEST_W*4);
    assert(!player_tick());
    valid[0]=valid[1]=1;strcpy(names[0],"/");strcpy(names[1],"trash");
    for(int i=2;i<34;i++){valid[i]=1;generations[i]=(unsigned)i;snprintf(names[i],FS_NAME_LEN,"Track %02d.%s",34-i,i&1?"MP3":"wav");}
    parents[33]=1;
    player_init();assert(file_count==31);draw_at(560,480);draw_at(420,390);draw_at(320,300);
    for(int i=0;i<25;i++)assert(player_key(KEY_DOWN,0));
    draw_at(420,390);assert(first_visible>0);
    assert(player_key(KEY_ENTER,0));assert(fake.state==AUDIO_PLAYING&&player_title()[0]);
    assert(player_key(KEY_SPACE,0));assert(fake.state==AUDIO_PAUSED);
    assert(player_key(KEY_SPACE,0));assert(fake.state==AUDIO_PLAYING);
    assert(player_key(0,'-'));assert(fake.volume==70);
    assert(player_key(0,'+'));assert(fake.volume==75);
    assert(player_click(25,30,420,390,25+96,30+224));assert(fake.volume==0);
    assert(player_click(25,30,420,390,25+420-80,30+224));assert(fake.volume==100);
    assert(player_click(25,30,420,390,25+140,30+185));assert(fake.state==AUDIO_STOPPED);
    assert(player_tick());assert(!player_tick());now+=TIMER_HZ;assert(player_tick());
    fake.error=MEDIA_NO_DEVICE;assert(player_tick()&&message[0]);
    assert(player_click(25,30,420,390,25+220,30+185));assert(!message[0]);
    strcpy(names[2],"Film.mpg");player_refresh();assert(player_open_file(2)==0);
    assert(video_mode&&fake_video.state==VIDEO_LOADING&&fake.state==AUDIO_STOPPED);
    draw_at(640,584);draw_at(420,390);draw_at(420,362);
    fake_video.audio_enabled=1;fake_video.audio_sample_rate=44100;fake_video.audio_channels=2;
    draw_at(640,584);draw_at(420,390);
    unsigned volume_before=fake.volume;
    assert(player_click(25,30,640,584,25+640-306,30+414));assert(fake.volume==volume_before-5);
    assert(player_click(25,30,640,584,25+640-202,30+414));assert(fake.volume==volume_before);
    assert(player_key(KEY_SPACE,0));assert(fake_video.state==VIDEO_PAUSED);
    assert(player_key(KEY_SPACE,0));assert(fake_video.state==VIDEO_PLAYING);
    fake_video.displayed_frames=18;assert(player_tick());draw_at(640,584);
    assert(player_click(25,30,640,584,25+130,30+414));assert(fake_video.state==VIDEO_STOPPED);
    assert(player_key(KEY_ENTER,0));assert(fake_video.state==VIDEO_LOADING);
    player_close();assert(fake_video.state==VIDEO_STOPPED&&fake.state==AUDIO_STOPPED);
    assert(player_open_file(3)==0);assert(!video_mode&&fake_video.state==VIDEO_STOPPED);
    test_tick_classification();test_progress_pixels();
    assert(player_open_file(3)==0);
    for(int i=2;i<34;i++)valid[i]=0;
    player_refresh();assert(!file_count);draw_at(420,390);
    player_close();assert(player_tick()==PLAYER_CHANGED);assert(!player_tick());
    assert(player_key(KEY_ENTER,0));assert(message[0]);
    assert(player_tick()==PLAYER_CHANGED);assert(!player_tick());
    assert(player_key(KEY_SPACE,0));assert(player_tick()==PLAYER_CHANGED);
    puts("Player list, transport, conservative tick classification, real-pixel progress equivalence, and client bounds passed.");
}
