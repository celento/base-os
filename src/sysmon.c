#include "app.h"
#include "sysmon.h"
#include "program.h"

static char cpu_vendor[13];
static char cpu_brand[49];
static int cpu_done;

static void cpuid(unsigned leaf, unsigned *a, unsigned *b, unsigned *c, unsigned *d) {
    asm volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

static void put4(char *dst, unsigned v) {
    dst[0] = (char)(v & 255);
    dst[1] = (char)((v >> 8) & 255);
    dst[2] = (char)((v >> 16) & 255);
    dst[3] = (char)((v >> 24) & 255);
}

static void cpu_probe(void) {
    unsigned a, b, c, d;
    cpuid(0, &a, &b, &c, &d);
    put4(cpu_vendor, b);
    put4(cpu_vendor + 4, d);
    put4(cpu_vendor + 8, c);
    cpu_vendor[12] = 0;
    cpuid(0x80000000u, &a, &b, &c, &d);
    if (a >= 0x80000004u) {
        for (unsigned i = 0; i < 3; i++) {
            cpuid(0x80000002u + i, &a, &b, &c, &d);
            put4(cpu_brand + i * 16, a);
            put4(cpu_brand + i * 16 + 4, b);
            put4(cpu_brand + i * 16 + 8, c);
            put4(cpu_brand + i * 16 + 12, d);
        }
        cpu_brand[48] = 0;
    } else {
        cpu_brand[0] = 0;
    }
    cpu_done = 1;
}

static const char *brand_trim(void) {
    const char *p = cpu_brand;
    while (*p == ' ')
        p++;
    return p;
}

/* Every control and text call is bounded by the supplied client rectangle. */
typedef struct { int x,y,w,h; } MonitorRect;
typedef struct {
    MonitorRect client, tabs[3];
    int x, w, heading_y, rows_y, footer_y;
} MonitorLayout;
#define TASK_ROW_H 42
#define WINDOW_ROW_H 38
static int current_tab;
static int drawn_tab=-1, drawn_task_n, drawn_win_n;
static MonitorRect drawn_client;
static TermTaskInfo drawn_tasks[SYSMON_MAX_TASKS];
static int drawn_windows[SYSMON_MAX_WIN];

static int smaller(int a,int b){return a<b?a:b;}
static MonitorLayout layout(int bx,int by,int bw,int bh){
    MonitorLayout l={0};
    l.client=(MonitorRect){bx,by,bw,bh};
    int margin=bw>=64?16:0;
    l.x=bx+margin;l.w=bw-2*margin;
    int gap=l.w>=24?6:0,tabw=(l.w-2*gap)/3;
    for(int i=0;i<3;i++)l.tabs[i]=(MonitorRect){l.x+i*(tabw+gap),by+12,tabw,28};
    l.heading_y=by+48;l.rows_y=by+74;l.footer_y=by+bh-24;
    return l;
}
static int inside(MonitorRect outer,MonitorRect r){
    return r.w>0&&r.h>0&&r.x>=outer.x&&r.y>=outer.y&&
        r.x-outer.x<=outer.w-r.w&&r.y-outer.y<=outer.h-r.h;
}
static int contains(MonitorRect r,int mx,int my){
    return r.w>0&&r.h>0&&mx>=r.x&&my>=r.y&&mx-r.x<r.w&&my-r.y<r.h;
}
static void text(const MonitorLayout *l,const char *s,int x,int y,int w,int bold,uint8_t color){
    if(!s||w<=0||y<l->client.y||y-l->client.y>l->client.h-UI_FONT_H)return;
    if(x<l->client.x)return;
    int right=l->client.x+l->client.w;
    if(w>right-x)w=right-x;
    /* Glyph rasters are wider than advances. Reserve their final overhang. */
    if(w<UI_FONT_W)return;
    if(bold)draw_string_bold_clip(s,x,y,color,x+w-UI_FONT_W);
    else draw_string_clip(s,x,y,color,x+w-UI_FONT_W);
}
static void button(const MonitorLayout *l,MonitorRect r,const char *label,int active){
    if(!inside(l->client,r))return;
    draw_round_rect(r.x,r.y,r.w,r.h,5,active?app_accent:gfx_gray(0xD8));
    if(!active&&r.w>2&&r.h>2)draw_round_rect(r.x+1,r.y+1,r.w-2,r.h-2,4,COLOR_WHITE);
    int tw=ui_string_w(label),tx=r.x+(r.w-tw)/2;
    if(tx<r.x+3)tx=r.x+3;
    /* Centered labels have ample horizontal padding at supported sizes. */
    text(l,label,tx,r.y+(r.h-UI_FONT_H)/2,r.x+r.w-tx,0,active?COLOR_WHITE:app_text);
}
static void row(const MonitorLayout *l,int y,const char *label,const char *value){
    int labelw=smaller(144,l->w/3),gap=12;
    text(l,label,l->x,y,labelw,0,app_text_dim);
    int valuew=l->w-labelw-gap;
    int tw=ui_string_w(value)+UI_FONT_W;
    int x=l->x+labelw+gap;
    if(tw<valuew)x+=valuew-tw;
    text(l,value,x,y,l->x+l->w-x,0,app_text);
}
/* Avoid width * bytes: long is only 32 bits in the actual i386 kernel.
 * Accumulate a fraction once per visible pixel; neither addend can exceed cap. */
static int bar_width(int width,int used,int cap){
    if(width<=0||used<=0||cap<=0)return 0;
    if(used>=cap)return width;
    unsigned remainder=0,amount=(unsigned)used,limit=(unsigned)cap;
    int filled=0;
    for(int i=0;i<width;i++){
        if(remainder>=limit-amount){remainder-=limit-amount;filled++;}
        else remainder+=amount;
    }
    return filled;
}
static void bar(const MonitorLayout *l,int y,int used,int cap){
    MonitorRect r={l->x,y,l->w,8};
    if(!inside(l->client,r))return;
    draw_round_rect(r.x,r.y,r.w,r.h,4,gfx_gray(0xDC));
    int fw=bar_width(r.w,used,cap);
    if(fw>=8)draw_round_rect(r.x,r.y,fw,r.h,4,app_accent);
    else if(fw>0)draw_rect(r.x,r.y,fw,r.h,app_accent);
}
static void append(char *dst,const char *s){
    while(*dst)dst++;
    while(*s)*dst++=*s++;
    *dst=0;
}
static void fmt_time(char *out,unsigned sec){
    char t[12];
    fmt_uint(out,sec/3600);append(out,"h ");
    fmt_pad2(t,(int)(sec/60)%60);append(out,t);append(out,"m ");
    fmt_pad2(t,(int)(sec%60));append(out,t);append(out,"s");
}
static void system_draw(const MonitorLayout *l,const SysInfo *si){
    char buf[64],t[12];
    text(l,"System overview",l->x,l->heading_y,l->w,1,app_text);
    int y=l->rows_y+4;
    row(l,y,"Processor",cpu_brand[0]?brand_trim():cpu_vendor);y+=28;
    fmt_time(buf,si->uptime_sec);row(l,y,"Uptime",buf);y+=28;
    fmt_uint(buf,(unsigned)si->mem_mb);append(buf," MB");row(l,y,"Memory",buf);y+=28;
    fmt_uint(buf,(unsigned)si->fb_w);append(buf," x ");fmt_uint(t,(unsigned)si->fb_h);
    append(buf,t);append(buf," @ ");fmt_uint(t,(unsigned)si->fb_bpp);append(buf,t);append(buf," bpp");
    row(l,y,"Display",buf);y+=28;
    fmt_uint(buf,si->frames);row(l,y,"Frames rendered",buf);y+=40;
    text(l,"Storage",l->x,y,l->w,1,app_text);y+=28;
    fmt_uint(buf,(unsigned)si->fs_nodes);append(buf," of ");fmt_uint(t,(unsigned)si->fs_max);append(buf,t);
    row(l,y,"Files and folders",buf);y+=24;bar(l,y,si->fs_nodes,si->fs_max);y+=22;
    fmt_uint(buf,(unsigned)si->fs_bytes/1024);append(buf," KB of ");
    fmt_uint(t,(unsigned)si->fs_cap/1024);append(buf,t);append(buf," KB");
    row(l,y,"Data used",buf);y+=24;bar(l,y,si->fs_bytes,si->fs_cap);
    text(l,"CPU utilization is not measured.",l->x,l->footer_y,l->w,0,app_text_dim);
}
static MonitorRect task_button(const MonitorLayout *l,int i,int stop){
    int sw=ui_string_w("Stop task")+32,showw=ui_string_w("Show terminal")+32,gap=6;
    int right=l->x+l->w;
    return (MonitorRect){stop?right-sw:right-sw-gap-showw,l->rows_y+i*TASK_ROW_H,
        stop?sw:showw,22};
}
static MonitorRect window_button(const MonitorLayout *l,int i){
    int width=ui_string_w("Close")+32;
    return (MonitorRect){l->x+l->w-width,l->rows_y+i*WINDOW_ROW_H+4,width,24};
}
static int row_visible(const MonitorLayout *l,int i,int height){
    int y=l->rows_y+i*height;
    return l->w>=240&&y>=l->client.y&&y+height<=l->footer_y-4;
}
static void windows_draw(const MonitorLayout *l,const SysInfo *si){
    char heading[32];fmt_uint(heading,(unsigned)si->win_n);append(heading,si->win_n==1?" open window":" open windows");
    text(l,heading,l->x,l->heading_y,l->w,1,app_text);
    int count=smaller(si->win_n,SYSMON_MAX_WIN);
    for(int i=0;i<count&&row_visible(l,i,WINDOW_ROW_H);i++){
        int y=l->rows_y+i*WINDOW_ROW_H;
        MonitorRect close=window_button(l,i);
        int statusx=close.x-110;
        text(l,si->win_name[i],l->x+6,y+7,statusx-l->x-12,0,app_text);
        text(l,si->win_min[i]?"Minimized":"Open",statusx,y+7,104,0,app_text_dim);
        button(l,close,"Close",0);
        draw_hline(l->x,y+WINDOW_ROW_H-1,l->w,gfx_gray(0xEC));
        drawn_windows[drawn_win_n++]=si->win_id[i];
    }
    if(count==0)text(l,"No windows are open.",l->x,l->rows_y,l->w,0,app_text_dim);
    const char *footer=drawn_win_n<count?"Enlarge the window to see all windows.":
        "Closing a Terminal also ends its native task.";
    text(l,footer,l->x,l->footer_y,l->w,0,app_text_dim);
}
static void tasks_draw(const MonitorLayout *l,const SysInfo *si){
    char heading[40];fmt_uint(heading,(unsigned)si->task_n);append(heading,si->task_n==1?" active native task":" active native tasks");
    text(l,heading,l->x,l->heading_y,l->w,1,app_text);
    int count=smaller(si->task_n,SYSMON_MAX_TASKS);
    for(int i=0;i<count&&row_visible(l,i,TASK_ROW_H);i++){
        const TermTaskInfo *task=&si->tasks[i];
        int y=l->rows_y+i*TASK_ROW_H;
        MonitorRect show=task_button(l,i,0),stop=task_button(l,i,1);
        char title[TERM_TASK_TITLE_LEN]="";
        append(title,task->name);
        if(task->document[0]){append(title," - ");append(title,task->document);}
        text(l,title,l->x+6,y+2,show.x-l->x-12,1,app_text);
        button(l,show,"Show terminal",0);button(l,stop,"Stop task",0);
        char detail[96]="Terminal ",t[24];
        fmt_uint(t,(unsigned)task->owner+1);append(detail,t);
        append(detail,task->state==PROCESS_TASK_SLEEPING?"   Sleeping":"   Running");
        append(detail,"   Elapsed ");fmt_time(t,task->elapsed_sec);append(detail,t);
        text(l,detail,l->x+6,y+22,l->w-12,0,app_text_dim);
        draw_hline(l->x,y+TASK_ROW_H-1,l->w,gfx_gray(0xEC));
        drawn_tasks[drawn_task_n++]=*task;
    }
    if(count==0){
        text(l,"No native tasks are running.",l->x,l->rows_y,l->w,0,app_text);
        text(l,"In Terminal: start /Programs/counter.bex",l->x,l->rows_y+28,l->w,0,app_text_dim);
    }
    const char *footer=drawn_task_n<count?"Enlarge the window to see all tasks.":
        "Elapsed is lifetime, including time spent sleeping.";
    text(l,footer,l->x,l->footer_y,l->w,0,app_text_dim);
}
void sysmon_reset(void){current_tab=SYSMON_TAB_SYSTEM;drawn_tab=-1;drawn_task_n=drawn_win_n=0;}
int sysmon_tab(void){return current_tab;}
void sysmon_draw(int bx,int by,int bw,int bh,const SysInfo *si){
    drawn_tab=-1;drawn_task_n=drawn_win_n=0;
    if(bw<=0||bh<=0||!si)return;
    if(!cpu_done)cpu_probe();
    MonitorLayout l=layout(bx,by,bw,bh);
    draw_rect(bx,by,bw,bh,COLOR_WHITE);
    static const char *const tabs[]={"System","Windows","Tasks"};
    for(int i=0;i<3;i++)button(&l,l.tabs[i],tabs[i],current_tab==i);
    if(current_tab==SYSMON_TAB_WINDOWS)windows_draw(&l,si);
    else if(current_tab==SYSMON_TAB_TASKS)tasks_draw(&l,si);
    else system_draw(&l,si);
    drawn_client=l.client;drawn_tab=current_tab;
}
SysmonAction sysmon_click(int bx,int by,int bw,int bh,int mx,int my,const SysInfo *si){
    SysmonAction none={SYSMON_ACTION_NONE,-1,0};
    if(bw<=0||bh<=0||!si)return none;
    MonitorLayout l=layout(bx,by,bw,bh);
    if(!contains(l.client,mx,my))return none;
    for(int i=0;i<3;i++)if(inside(l.client,l.tabs[i])&&contains(l.tabs[i],mx,my)){
        if(current_tab==i)return none;
        current_tab=i;drawn_tab=-1;
        return (SysmonAction){SYSMON_ACTION_REDRAW,-1,0};
    }
    if(drawn_tab!=current_tab||drawn_client.x!=bx||drawn_client.y!=by||
       drawn_client.w!=bw||drawn_client.h!=bh)return none;
    if(current_tab==SYSMON_TAB_WINDOWS){
        for(int i=0;i<drawn_win_n;i++)if(contains(window_button(&l,i),mx,my)){
            for(int j=0;j<si->win_n&&j<SYSMON_MAX_WIN;j++)if(si->win_id[j]==drawn_windows[i])
                return (SysmonAction){SYSMON_ACTION_CLOSE_WINDOW,drawn_windows[i],0};
        }
    }else if(current_tab==SYSMON_TAB_TASKS){
        for(int i=0;i<drawn_task_n;i++){
            int kind=contains(task_button(&l,i,1),mx,my)?SYSMON_ACTION_STOP_TASK:
                contains(task_button(&l,i,0),mx,my)?SYSMON_ACTION_SHOW_TERMINAL:SYSMON_ACTION_NONE;
            if(!kind)continue;
            const TermTaskInfo *shown=&drawn_tasks[i];
            for(int j=0;j<si->task_n&&j<SYSMON_MAX_TASKS;j++){
                const TermTaskInfo *live=&si->tasks[j];
                if(live->owner==shown->owner&&live->instance==shown->instance&&
                   (live->state==PROCESS_TASK_READY||live->state==PROCESS_TASK_SLEEPING))
                    return (SysmonAction){kind,shown->owner,shown->instance};
            }
        }
    }
    return none;
}
