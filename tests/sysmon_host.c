/* Normal monitor layouts/actions, using the real software compositor. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "layout.h"
static uint8_t test_mirror[FB_CAPACITY];
#undef PRESENT_BASE
#define PRESENT_BASE ((uintptr_t)test_mirror)
#define GFX_HOST_TEST
#include "../src/gfx.c"
#include "app.h"
#include "sysmon.h"
#include "program.h"
void kmemset(void *d,int v,int n){memset(d,v,(size_t)n);}
void kmemcpy(void *d,const void *s,int n){memmove(d,s,(size_t)n);}
void fmt_uint(char *s,unsigned n){sprintf(s,"%u",n);}
void fmt_pad2(char *s,int n){s[0]='0'+n/10%10;s[1]='0'+n%10;s[2]=0;}
uint8_t app_accent=COLOR_BLUE,app_accent_dk=COLOR_NAVY,app_text=COLOR_DKGRAY;
uint8_t app_text_dim=COLOR_GRAY,app_chrome=COLOR_WHITE,app_chrome_dk=COLOR_LTGRAY;
static uint8_t back[1280*800],linear[sizeof back*4];
static int bx,by,bw,bh,show_labels,stop_labels,close_labels;
static void bounds(int x,int y,int w,int h){
    assert(w>0&&h>0&&x>=bx&&y>=by&&x+w<=bx+bw&&y+h<=by+bh);
}
static void checked_rect(int x,int y,int w,int h,uint8_t c){bounds(x,y,w,h);draw_rect(x,y,w,h,c);}
static void checked_round(int x,int y,int w,int h,int r,uint8_t c){bounds(x,y,w,h);draw_round_rect(x,y,w,h,r,c);}
static void checked_line(int x,int y,int w,uint8_t c){bounds(x,y,w,1);draw_hline(x,y,w,c);}
static void checked_text(const char *s,int x,int y,uint8_t c,int xmax){
    bounds(x,y,xmax-x+UI_FONT_W,UI_FONT_H);
    if((!strcmp(s,"Show terminal")||!strcmp(s,"Stop task")||!strcmp(s,"Close"))&&ui_string_w(s)>xmax-x)fprintf(stderr,"clipped label %s: %d > %d\n",s,ui_string_w(s),xmax-x);
    if(!strcmp(s,"Show terminal")){show_labels++;assert(ui_string_w(s)<=xmax-x);}
    if(!strcmp(s,"Stop task")){stop_labels++;assert(ui_string_w(s)<=xmax-x);}
    if(!strcmp(s,"Close")){close_labels++;assert(ui_string_w(s)<=xmax-x);}
    draw_string_clip(s,x,y,c,xmax);
}
static void checked_bold(const char *s,int x,int y,uint8_t c,int xmax){
    bounds(x,y,xmax-x+UI_FONT_W,UI_FONT_H);draw_string_bold_clip(s,x,y,c,xmax);
}
#define draw_rect checked_rect
#define draw_round_rect checked_round
#define draw_hline checked_line
#define draw_string_clip checked_text
#define draw_string_bold_clip checked_bold
#include "../src/sysmon.c"
#undef draw_rect
#undef draw_round_rect
#undef draw_hline
#undef draw_string_clip
#undef draw_string_bold_clip
static void render(SysInfo *si){
    memset(back,253,sizeof back);show_labels=stop_labels=close_labels=0;
    sysmon_draw(bx,by,bw,bh,si);
    for(int y=0;y<800;y++)for(int x=0;x<1280;x++)
        if(x<bx||x>=bx+bw||y<by||y>=by+bh)assert(back[y*1280+x]==253);
}
static SysmonAction click(SysInfo *si,MonitorRect r){
    return sysmon_click(bx,by,bw,bh,r.x+r.w/2,r.y+r.h/2,si);
}
static void tab(SysInfo *si,int index){
    MonitorLayout l=layout(bx,by,bw,bh);
    int previous=sysmon_tab();SysmonAction a=click(si,l.tabs[index]);
    assert(a.kind==(previous==index?SYSMON_ACTION_NONE:SYSMON_ACTION_REDRAW));
    assert(sysmon_tab()==index);render(si);
}
static void snapshot(const char *directory,const char *name){
    if(!directory)return;
    char path[512];snprintf(path,sizeof path,"%s/%s.ppm",directory,name);
    FILE *f=fopen(path,"wb");assert(f);fprintf(f,"P6\n%d %d\n255\n",bw,bh);
    for(int y=by;y<by+bh;y++)for(int x=bx;x<bx+bw;x++){
        uint32_t color=pal32[back[y*1280+x]];
        fputc((color>>16)&255,f);fputc((color>>8)&255,f);fputc(color&255,f);
    }
    assert(!fclose(f));
}
static void check_actions(SysInfo *si){
    MonitorLayout l=layout(bx,by,bw,bh);
    tab(si,SYSMON_TAB_WINDOWS);assert(drawn_win_n==8&&close_labels==8);
    for(int i=0;i<8;i++){
        SysmonAction a=click(si,window_button(&l,i));
        assert(a.kind==SYSMON_ACTION_CLOSE_WINDOW&&a.owner==si->win_id[i]);
    }
    tab(si,SYSMON_TAB_TASKS);assert(drawn_task_n==8&&show_labels==8&&stop_labels==8);
    for(int i=0;i<8;i++)for(int stop=0;stop<2;stop++){
        SysmonAction a=click(si,task_button(&l,i,stop));
        assert(a.kind==(stop?SYSMON_ACTION_STOP_TASK:SYSMON_ACTION_SHOW_TERMINAL));
        assert(a.owner==si->tasks[i].owner&&a.task_instance==si->tasks[i].instance);
    }
    /* Background, metadata text and just outside the client are not controls. */
    assert(!sysmon_click(bx,by,bw,bh,l.x+5,l.rows_y+25,si).kind);
    assert(!sysmon_click(bx,by,bw,bh,bx+bw,by+20,si).kind);
    assert(!sysmon_click(bx,by,bw,bh,bx+1,by+bh,si).kind);
    /* Actions are tied to the last rendered task identity, not a shifting row. */
    SysInfo changed=*si;
    for(int i=0;i<7;i++)changed.tasks[i]=changed.tasks[i+1];
    changed.task_n=7;
    assert(!click(&changed,task_button(&l,0,1)).kind);
    SysmonAction a=click(&changed,task_button(&l,1,1));
    assert(a.kind==SYSMON_ACTION_STOP_TASK&&a.owner==si->tasks[1].owner);
    changed=*si;changed.tasks[0].instance++;
    assert(!click(&changed,task_button(&l,0,1)).kind);
    MonitorRect stop=task_button(&l,0,1);
    assert(!sysmon_click(bx,by,bw+1,bh,stop.x+10,stop.y+10,si).kind);
}
int main(int argc,char **argv){
    gfx_init(back,linear,1280,800,32,1280*4);
    SysInfo si={.uptime_sec=3723,.frames=1250,.fs_nodes=35,.fs_max=64,
        .fs_bytes=7340032,.fs_cap=8385024,.fb_w=1280,.fb_h=800,.fb_bpp=32,.mem_mb=64,
        .win_n=8,.task_n=8};
    for(int i=0;i<8;i++){
        si.win_name[i]=i%2?"Terminal":"System Monitor";si.win_id[i]=7-i;si.win_min[i]=i%2;
        snprintf(si.tasks[i].name,sizeof si.tasks[i].name,"counter-%d.bex",i+1);
        si.tasks[i].owner=i;si.tasks[i].instance=(unsigned)i+101;
        si.tasks[i].state=i%2?PROCESS_TASK_SLEEPING:PROCESS_TASK_READY;
        si.tasks[i].elapsed_sec=(unsigned)(i+1)*3711;
    }
    /* Valid 8 MiB capacity cases that overflowed signed i386 width * bytes. */
    int widths[]={1,7,488,1000,1248};
    int capacities[]={64,1032129,8385024};
    for(unsigned w=0;w<sizeof widths/sizeof *widths;w++)for(unsigned c=0;c<sizeof capacities/sizeof *capacities;c++){
        int cap=capacities[c],used[]={0,1,cap/4,cap/2,cap-1,cap};
        for(unsigned i=0;i<sizeof used/sizeof *used;i++)
            assert(bar_width(widths[w],used[i],cap)==(int)((uint64_t)widths[w]*(unsigned)used[i]/(unsigned)cap));
    }
    bx=23;by=31;bw=SYSMON_W;bh=SYSMON_H-1;
    sysmon_reset();render(&si);snapshot(argc>1?argv[1]:0,"monitor-system");
    check_actions(&si);snapshot(argc>1?argv[1]:0,"monitor-tasks");
    tab(&si,SYSMON_TAB_WINDOWS);snapshot(argc>1?argv[1]:0,"monitor-windows");
    bx=350;by=80;bw=900;bh=650;check_actions(&si);
    tab(&si,SYSMON_TAB_SYSTEM);render(&si);
    /* Smaller supplied dimensions stay bounded, although unsupported for all rows. */
    int sizes[][2]={{360,200},{280,120},{32,32},{1,1}};
    for(unsigned i=0;i<sizeof sizes/sizeof *sizes;i++){
        bx=100;by=90;bw=sizes[i][0];bh=sizes[i][1];
        for(int page=0;page<3;page++){current_tab=page;render(&si);}
    }
    bx=11;by=13;bw=520;bh=439;si.win_n=si.task_n=0;
    tab(&si,SYSMON_TAB_TASKS);assert(!drawn_task_n&&!stop_labels&&!show_labels);
    tab(&si,SYSMON_TAB_WINDOWS);assert(!drawn_win_n&&!close_labels);
    puts("monitor: three tabs, eight bounded rows, resized hit targets, task identity, empty states and 8 MiB bars passed");
}
