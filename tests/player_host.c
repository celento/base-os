#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "audio.h"
#include "video.h"
#include "app.h"
#include "fs.h"

static char names[FS_MAX_NODES][FS_NAME_LEN];
static unsigned generations[FS_MAX_NODES];
static int parents[FS_MAX_NODES], valid[FS_MAX_NODES];
static unsigned now;
static AudioStatus fake={.available=1,.volume=75};
static VideoStatus fake_video;
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
uint32_t audio_capacity_bytes(void){return 393216;}
uint32_t audio_position_ms(void){return fake.played_frames/22;}
uint32_t audio_duration_ms(void){return fake.total_frames/22;}
int audio_play(const void *data,uint32_t bytes){(void)data;(void)bytes;fake.state=AUDIO_PLAYING;fake.error=0;fake.format=AUDIO_FORMAT_MP3;fake.channels=2;fake.sample_rate=22050;fake.total_frames=22050;return 0;}
void audio_pause(int paused){fake.state=paused?AUDIO_PAUSED:AUDIO_PLAYING;}
void audio_stop(void){fake.state=AUDIO_STOPPED;fake.error=0;fake.played_frames=0;}
void audio_set_volume(unsigned n){fake.volume=n>100?100:n;}
void audio_clear_error(void){fake.error=0;}
const VideoStatus *video_status(void){return &fake_video;}
int video_play(const void *data,uint32_t bytes){(void)data;(void)bytes;fake_video=(VideoStatus){.state=VIDEO_LOADING,.width=320,.height=240,.fps_num=25,.fps_den=1,.total_frames=75};return 0;}
void video_pause(int paused){fake_video.state=paused?VIDEO_PAUSED:VIDEO_PLAYING;}
void video_stop(void){fake_video.state=VIDEO_STOPPED;fake_video.displayed_frames=0;fake_video.error=0;}
void video_clear_error(void){fake_video.error=0;}
int video_poll(void){return 0;}
uint32_t video_position_ms(void){return fake_video.displayed_frames*40;}
uint32_t video_duration_ms(void){return fake_video.total_frames*40;}
const char *video_error_string(int error){(void)error;return "Video unavailable";}
const char *media_error_string(int error){(void)error;return "Audio unavailable";}
uint8_t app_accent=9,app_accent_dk=13,app_text=0,app_text_dim=2,app_chrome=3,app_chrome_dk=2;
static int origin_x,origin_y,bounds_w,bounds_h,draws;
static void bounds(int x,int y,int w,int h){assert(w>=0&&h>=0);assert(x>=origin_x&&y>=origin_y&&x+w<=origin_x+bounds_w&&y+h<=origin_y+bounds_h);draws++;}
void draw_rect(int x,int y,int w,int h,uint8_t c){(void)c;bounds(x,y,w,h);}
void video_draw(int x,int y,int w,int h){bounds(x,y,w,h);}
void draw_round_rect(int x,int y,int w,int h,int r,uint8_t c){(void)r;draw_rect(x,y,w,h,c);}
void draw_round_frame(int x,int y,int w,int h,int r,uint8_t c){(void)r;draw_rect(x,y,w,h,c);}
int ui_string_w(const char *s){return (int)strlen(s)*7;}
void draw_string(const char *s,int x,int y,uint8_t c){(void)c;bounds(x,y,ui_string_w(s),18);}
void draw_string_bold(const char *s,int x,int y,uint8_t c){draw_string(s,x,y,c);}
void draw_string_clip(const char *s,int x,int y,uint8_t c,int end){(void)c;int width=ui_string_w(s);if(width>end-x)width=end-x;bounds(x,y,width,18);}
void draw_string_bold_clip(const char *s,int x,int y,uint8_t c,int end){draw_string_clip(s,x,y,c,end);}
uint8_t gfx_lighter(uint8_t c,int p){(void)p;return c;}
uint8_t gfx_rgb(int r,int g,int b){(void)r;(void)g;(void)b;return 4;}
void fmt_uint(char *s,unsigned n){sprintf(s,"%u",n);}
void fmt_pad2(char *s,int n){sprintf(s,"%02d",n);}
int hit(int px,int py,int x,int y,int w,int h){return px>=x&&py>=y&&px<x+w&&py<y+h;}
#include "../src/player.c"
static void draw_at(int width,int height){origin_x=25;origin_y=30;bounds_w=width;bounds_h=height;draws=0;player_draw(origin_x,origin_y,width,height);assert(draws>0);}
int main(void){
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
    for(int i=2;i<34;i++)valid[i]=0;
    player_refresh();assert(!file_count);draw_at(420,390);
    puts("Player list, selection, transport, volume, refresh, and client bounds passed.");
}
