#include "ui/app.h"
#include "apps/sysmon.h"
#include "kernel/program.h"
#include "ui/icon_draw.h"
#include "ui/kit.h"
#include "ui/ui_icons.h"

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

/* Layout: a toolbar with the System / Windows / Tasks switch, the page, and
 * a status bar. In Modern the toolbar continues the window header. Every
 * control and text call stays inside the supplied client rectangle, and the
 * same rectangles serve drawing and hit testing. */
#define MODERN (ui_theme.style == UI_STYLE_MODERN)
typedef struct { int x,y,w,h; } MonitorRect;
typedef struct {
    MonitorRect client, tool, tabs[3], body, status;
    int ok, x, w, rows_y, rows_bottom;
} MonitorLayout;
static int current_tab;
static int drawn_tab=-1, drawn_task_n, drawn_win_n;
static MonitorRect drawn_client;
static TermTaskInfo drawn_tasks[SYSMON_MAX_TASKS];
static int drawn_program_n;
static unsigned drawn_programs[SYSMON_MAX_PROGRAMS];
static int drawn_windows[SYSMON_MAX_WIN];
static unsigned drawn_window_instances[SYSMON_MAX_WIN];
static const char *const tab_labels[3]={"System","Windows","Tasks"};

/* The desktop passes its hover tracker while the window is active, so
 * pointer motion over a control repaints it. Without one nothing hovers. */
static int (*hover_fn)(int x,int y,int w,int h);
void sysmon_set_hover(int (*fn)(int x,int y,int w,int h)){hover_fn=fn;}
static int hover(MonitorRect r){return hover_fn&&r.w>0&&r.h>0?hover_fn(r.x,r.y,r.w,r.h):0;}

static int tool_h(void){return MODERN?44:34;}
static int status_h(void){return MODERN?28:24;}
static int side_pad(void){return MODERN?16:12;}
static int head_h(void){return MODERN?32:24;}
static int window_row_h(void){return ui_theme.control_h+8;}
static int task_row_h(void){return MODERN?46:42;}
static int smaller(int a,int b){return a<b?a:b;}

static MonitorLayout layout(int bx,int by,int bw,int bh){
    MonitorLayout l={0};
    l.client=(MonitorRect){bx,by,bw,bh};
    l.ok=bw>=300&&bh>=tool_h()+status_h()+48;
    l.tool=(MonitorRect){bx,by,bw,tool_h()};
    l.status=(MonitorRect){bx,by+bh-status_h(),bw,status_h()};
    l.body=(MonitorRect){bx,by+tool_h(),bw,bh-tool_h()-status_h()};
    /* Segment widths match kit_segmented: label plus 12px each side, with
     * a 2px (Modern) or 1px (Classic) seam. */
    int sh=kit_segmented_h(),sy=by+(tool_h()-1-sh)/2,cx=bx+(MODERN?12:8)+(MODERN?2:1);
    for(int i=0;i<3;i++){
        int w=uib_string_w(tab_labels[i])+24;
        l.tabs[i]=(MonitorRect){cx,MODERN?sy+2:sy+1,w,MODERN?sh-4:sh-2};
        cx+=w+(MODERN?2:1);
    }
    l.x=bx+side_pad();l.w=bw-2*side_pad();
    l.rows_y=l.body.y+head_h();l.rows_bottom=l.status.y;
    return l;
}
static int contains(MonitorRect r,int mx,int my){
    return r.w>0&&r.h>0&&mx>=r.x&&my>=r.y&&mx-r.x<r.w&&my-r.y<r.h;
}
static void append(char *dst,const char *s){
    while(*dst)dst++;
    while(*s)*dst++=*s++;
    *dst=0;
}
static void fmt_time(char *out,unsigned sec){
    char t[12];
    out[0]=0;
    fmt_uint(t,sec/3600);append(out,t);append(out,"h ");
    fmt_pad2(t,(int)(sec/60)%60);append(out,t);append(out,"m ");
    fmt_pad2(t,(int)(sec%60));append(out,t);append(out,"s");
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
/* A usage bar; the fill is scaled to pixels first, so the kit never
 * multiplies a byte count. */
static void bar(int x,int y,int w,int used,int cap){
    if(w<16)return;
    kit_progress(x,y,w,bar_width(w,used,cap),w);
}

/* An empty list: a large icon, a title and a hint, centred in the body. */
static void empty_state(const MonitorLayout *l,int icon,const char *title,const char *hint){
    int top=l->rows_y,h=l->rows_bottom-top,need=32+12+UI_FONT_H*2+4,y=top+(h-need)/2;
    if(y<top+8)y=top+8;
    if(y+need>l->rows_bottom)return;
    ui_icon_draw(icon,l->client.x+(l->client.w-32)/2,y,32,TK_ICON,ui_theme.content);
    kit_text_center(title,l->x,y+44,l->w,TK_TEXT,1);
    kit_text_center(hint,l->x,y+44+UI_FONT_H+4,l->w,TK_TEXT2,0);
}
static void status_bar(const MonitorLayout *l,const char *left,const char *right){
    MonitorRect s=l->status;
    if(MODERN){
        draw_hline(s.x,s.y,s.w,TK_LINE_SUBTLE);
        kit_fill_bottom(s.x,s.y+1,s.w,s.h-1,ui_theme.r_window>1?ui_theme.r_window-1:0,ui_theme.statusbar);
    }else kit_statusbar(s.x,s.y,s.w,s.h);
    int ty=kit_ty(s.y+1,s.h-1),pad=MODERN?12:8,lw=0;
    if(left)lw=kit_text(left,s.x+pad,ty,TK_TEXT2,s.w-2*pad,0);
    if(right){
        int room=s.w-3*pad-lw,tw=kit_text_w(right,0);
        if(tw>room)tw=room;
        if(tw>24)kit_text(right,s.x+s.w-pad-tw,ty,TK_TEXT2,tw,0);
    }
}

/* System page: label and value rows grouped in cards (Modern) or listed
 * under section headings with rules (Classic). */
typedef struct { const MonitorLayout *l; int y, top, rows, full; } MonitorCard;
static int label_w(const MonitorLayout *l){return smaller(MODERN?150:144,l->w/3);}
static int card_bottom(const MonitorCard *c){return c->l->body.y+c->l->body.h-4;}
static void section(MonitorCard *c,const char *title,int first){
    const MonitorLayout *l=c->l;
    if(!first){
        if(!MODERN&&c->y+8<=card_bottom(c))kit_separator(l->x,c->y+6,l->w);
        c->y+=16;
    }
    c->top=c->y;c->rows=0;
    if(c->full||c->y+UI_FONT_H+8>card_bottom(c)){c->full=1;return;}
    kit_text(title,l->x,c->y,TK_TEXT,l->w,1);
    c->y+=UI_FONT_H+(MODERN?9:7);
    c->top=c->y;
}
/* One row; with_bar adds a usage bar under the value. */
static void card_row(MonitorCard *c,const char *label,const char *value,int with_bar,int used,int cap){
    const MonitorLayout *l=c->l;
    int rh=MODERN?(with_bar?54:36):(with_bar?42:26);
    if(c->full||c->y+rh>card_bottom(c)){c->full=1;return;}
    int lw=label_w(l);
    if(MODERN){
        int ix=l->x+16,iw=l->w-32,ty=with_bar?c->y+8:kit_ty(c->y,rh);
        if(c->rows)draw_hline(l->x+1,c->y,l->w-2,TK_45);
        kit_text(label,ix,ty,TK_TEXT,lw,0);
        int room=iw-lw-12,tw=kit_text_w(value,0);
        if(tw>room)tw=room;
        kit_text(value,ix+iw-tw,ty,TK_TEXT2,tw,0);
        if(with_bar)bar(ix,c->y+rh-18,iw,used,cap);
    }else{
        int vx=l->x+lw+12,vw=l->x+l->w-vx;
        kit_text(label,l->x,c->y+4,TK_TEXT2,lw,0);
        kit_text(value,vx,c->y+4,TK_TEXT,vw,0);
        if(with_bar)bar(vx,c->y+26,vw,used,cap);
    }
    c->y+=rh;c->rows++;
}
static void card_end(MonitorCard *c){
    if(MODERN&&c->rows)kit_frame(c->l->x,c->top,c->l->w,c->y-c->top,10,TK_LINE_SUBTLE);
}
static void system_draw(const MonitorLayout *l,const SysInfo *si){
    char buf[64],t[12];
    MonitorCard c={l,l->body.y+(MODERN?16:14),0,0,0};
    section(&c,"System",1);
    card_row(&c,"Processor",cpu_brand[0]?brand_trim():cpu_vendor,0,0,0);
    fmt_time(buf,si->uptime_sec);card_row(&c,"Uptime",buf,0,0,0);
    fmt_uint(buf,(unsigned)si->mem_mb);append(buf," MB");card_row(&c,"Memory",buf,0,0,0);
    fmt_uint(buf,(unsigned)si->fb_w);append(buf," x ");fmt_uint(t,(unsigned)si->fb_h);
    append(buf,t);append(buf,", ");fmt_uint(t,(unsigned)si->fb_bpp);append(buf,t);append(buf,"-bit, 256 colors");
    card_row(&c,"Display",buf,0,0,0);
    fmt_uint(buf,si->frames);card_row(&c,"Frames rendered",buf,0,0,0);
    card_end(&c);
    section(&c,"Storage",0);
    /* fs_max 0: the volume has no fixed node table, so there is no bar. */
    fmt_uint(buf,(unsigned)si->fs_nodes);
    if(si->fs_max>0){append(buf," of ");fmt_uint(t,(unsigned)si->fs_max);append(buf,t);}
    card_row(&c,"Files and folders",buf,si->fs_max>0,si->fs_nodes,si->fs_max);
    fmt_uint(buf,(unsigned)si->fs_bytes/1024);append(buf," KB of ");
    fmt_uint(t,(unsigned)si->fs_cap/1024);append(buf,t);append(buf," KB");
    card_row(&c,"Data used",buf,1,si->fs_bytes,si->fs_cap);
    card_end(&c);
}

/* Row buttons sit at the right of their row, vertically centred. */
static MonitorRect row_button(int right,int y,int rh,const char *label){
    int w=kit_button_w(label,-1),h=ui_theme.control_h;
    return (MonitorRect){right-w,y+(rh-h)/2,w,h};
}
static MonitorRect task_button(const MonitorLayout *l,int i,int stop){
    int y=l->rows_y+i*task_row_h(),right=l->x+l->w;
    MonitorRect s=row_button(right,y,task_row_h(),"Stop task");
    if(stop)return s;
    return row_button(s.x-(MODERN?8:6),y,task_row_h(),"Show window");
}
static MonitorRect program_button(const MonitorLayout *l,int row){
    return row_button(l->x+l->w,l->rows_y+row*task_row_h(),task_row_h(),"Stop program");
}
static MonitorRect window_button(const MonitorLayout *l,int i){
    return row_button(l->x+l->w,l->rows_y+i*window_row_h(),window_row_h(),"Close");
}
static int row_visible(const MonitorLayout *l,int i,int height){
    int y=l->rows_y+i*height;
    return l->ok&&y>=l->client.y&&y+height<=l->rows_bottom;
}
static void button(MonitorRect r,const char *label){kit_button(r.x,r.y,r.w,r.h,label,-1,KB_DEFAULT,hover(r));}
static void row_rule(const MonitorLayout *l,int y){
    if(MODERN)draw_hline(l->x,y,l->w,TK_LINE_SUBTLE);
    else draw_hline(l->client.x,y,l->client.w,TK_LINE_SUBTLE);
}
/* A coloured state dot and its label; returns the width used. */
static int state_dot(int x,int y,uint8_t dot,const char *label,int maxw){
    if(maxw<24)return 0;
    kit_fill(x,y+(UI_FONT_H-8)/2,8,8,4,dot,1);
    return 14+kit_text(label,x+14,y,TK_TEXT2,maxw-14,0);
}
static void list_header(const MonitorLayout *l,const char *first,const char *second,int second_x){
    MonitorRect b=l->body;
    kit_list_header(b.x,b.y,b.w,head_h());
    int ty=kit_ty(b.y,head_h());
    kit_text(first,l->x,ty,TK_TEXT,(second?second_x:l->x+l->w)-l->x-8,1);
    if(second){
        if(!MODERN)draw_vline(second_x-8,b.y+4,head_h()-8,TK_LINE_SUBTLE);
        kit_text(second,second_x,ty,TK_TEXT,l->x+l->w-second_x,1);
    }
}
static int window_state_x(const MonitorLayout *l){
    return window_button(l,0).x-(MODERN?124:116);
}
static void windows_draw(const MonitorLayout *l,const SysInfo *si,char *summary,const char **hint){
    fmt_uint(summary,(unsigned)si->win_n);append(summary,si->win_n==1?" open window":" open windows");
    int count=smaller(si->win_n,SYSMON_MAX_WIN),sx=window_state_x(l),rh=window_row_h();
    int columns=sx>l->x+120;
    list_header(l,"Window",columns?"State":0,sx);
    for(int i=0;i<count&&row_visible(l,i,rh);i++){
        int y=l->rows_y+i*rh,ty=kit_ty(y,rh);
        MonitorRect close=window_button(l,i);
        ui_icon_draw(UI_ICON_A_WINDOW,l->x,y+(rh-16)/2,16,TK_ICON,ui_theme.content);
        int name_right=columns?sx-12:close.x-12;
        kit_text(si->win_name[i],l->x+24,ty,TK_TEXT,name_right-l->x-24,0);
        if(columns)state_dot(sx,ty,si->win_min[i]?TK_LINE_STRONG:TK_GREEN,si->win_min[i]?"Minimized":"Open",close.x-sx-8);
        button(close,"Close");
        row_rule(l,y+rh-1);
        drawn_window_instances[drawn_win_n]=si->win_instance[i];
        drawn_windows[drawn_win_n++]=si->win_id[i];
    }
    if(count==0)empty_state(l,UI_ICON_A_WINDOW,"No windows are open","Apps you open appear here.");
    *hint=drawn_win_n<count?"Enlarge the window to see all windows":"Closing a native window ends its task";
}
static void tasks_draw(const MonitorLayout *l,const SysInfo *si,char *summary,const char **hint){
    fmt_uint(summary,(unsigned)si->task_n);append(summary,si->task_n==1?" active native task":" active native tasks");
    if(si->program_n>0){
        char t[12];append(summary,", ");fmt_uint(t,(unsigned)si->program_n);append(summary,t);
        append(summary,si->program_n==1?" program":" programs");
    }
    int count=smaller(si->task_n,SYSMON_MAX_TASKS),programs=smaller(si->program_n,SYSMON_MAX_PROGRAMS);
    int rh=task_row_h(),line1=(rh-2*UI_FONT_H-2)/2,line2=line1+UI_FONT_H+2;
    list_header(l,"Task",0,0);
    for(int i=0;i<count&&row_visible(l,i,rh);i++){
        const TermTaskInfo *task=&si->tasks[i];
        int y=l->rows_y+i*rh;
        MonitorRect show=task_button(l,i,0),stop=task_button(l,i,1);
        char title[TERM_TASK_TITLE_LEN]="";
        append(title,task->name);
        if(task->document[0]){append(title," - ");append(title,task->document);}
        int tw=show.x-l->x-12;
        kit_text(title,l->x,y+line1,TK_TEXT,tw,1);
        int sleeping=task->state==PROCESS_TASK_SLEEPING;
        int dx=l->x+state_dot(l->x,y+line2,sleeping?TK_AMBER:TK_GREEN,sleeping?"Sleeping":"Running",tw);
        char detail[96]="   Window ",t[24];
        fmt_uint(t,(unsigned)task->owner+1);append(detail,t);
        append(detail,"   Elapsed ");fmt_time(t,task->elapsed_sec);append(detail,t);
        kit_text(detail,dx,y+line2,TK_TEXT2,l->x+tw-dx,0);
        button(show,"Show window");button(stop,"Stop task");
        row_rule(l,y+rh-1);
        drawn_tasks[drawn_task_n++]=*task;
    }
    /* ELF programs follow the native tasks, one row each. */
    for(int k=0;k<programs&&drawn_task_n==count&&row_visible(l,count+k,rh);k++){
        const SysmonProgram *program=&si->programs[k];
        int y=l->rows_y+(count+k)*rh;
        MonitorRect stop=program_button(l,count+k);
        int tw=stop.x-l->x-12;
        kit_text(program->name,l->x,y+line1,TK_TEXT,tw,1);
        int dx=l->x+state_dot(l->x,y+line2,TK_ACCENT,"Program",tw);
        char detail[96]=" ",t[24];
        fmt_uint(t,program->pid);append(detail,t);
        append(detail,"   Threads ");fmt_uint(t,program->threads);append(detail,t);
        append(detail,"   Memory ");fmt_uint(t,program->kib);append(detail,t);append(detail," KiB");
        append(detail,"   CPU ");fmt_time(t,program->cpu_ms/1000);append(detail,t);
        kit_text(detail,dx,y+line2,TK_TEXT2,l->x+tw-dx,0);
        button(stop,"Stop program");
        row_rule(l,y+rh-1);
        drawn_programs[drawn_program_n++]=program->pid;
    }
    if(count==0&&programs==0)
        empty_state(l,UI_ICON_A_TASKS,"No native tasks are running","In Terminal: start /Programs/counter.bex");
    *hint=drawn_task_n<count||drawn_program_n<programs?"Enlarge the window to see all tasks":
        "Elapsed includes time spent sleeping";
}
void sysmon_reset(void){current_tab=SYSMON_TAB_SYSTEM;drawn_tab=-1;drawn_task_n=drawn_win_n=drawn_program_n=0;}
int sysmon_tab(void){return current_tab;}
void sysmon_draw(int bx,int by,int bw,int bh,const SysInfo *si){
    drawn_tab=-1;drawn_task_n=drawn_win_n=drawn_program_n=0;
    if(bw<=0||bh<=0||!si)return;
    if(!cpu_done)cpu_probe();
    MonitorLayout l=layout(bx,by,bw,bh);
    draw_rect(bx,by,bw,bh,ui_theme.content);
    if(!l.ok){
        if(bw>=200&&bh>=UI_FONT_H+16)kit_text("Enlarge System Monitor.",bx+8,by+8,TK_TEXT2,bw-16,0);
        return;
    }
    kit_toolbar(l.tool.x,l.tool.y,l.tool.w,l.tool.h);
    int hot=-1;
    for(int i=0;i<3;i++)if(i!=current_tab&&(hover(l.tabs[i])&KIT_HOVER))hot=i;
    kit_segmented(l.tabs[0].x-(MODERN?2:1),l.tabs[0].y-(MODERN?2:1),tab_labels,3,current_tab,hot,0,0);
    char summary[64]="";
    const char *hint=0;
    if(current_tab==SYSMON_TAB_WINDOWS)windows_draw(&l,si,summary,&hint);
    else if(current_tab==SYSMON_TAB_TASKS)tasks_draw(&l,si,summary,&hint);
    else{
        char up[24];
        system_draw(&l,si);
        fmt_time(up,si->uptime_sec);append(summary,"Up ");append(summary,up);
        hint="CPU utilization is not measured";
    }
    status_bar(&l,summary,hint);
    drawn_client=l.client;drawn_tab=current_tab;
}
SysmonAction sysmon_click(int bx,int by,int bw,int bh,int mx,int my,const SysInfo *si){
    SysmonAction none={SYSMON_ACTION_NONE,-1,0};
    if(bw<=0||bh<=0||!si)return none;
    MonitorLayout l=layout(bx,by,bw,bh);
    if(!l.ok||!contains(l.client,mx,my))return none;
    for(int i=0;i<3;i++)if(contains(l.tabs[i],mx,my)){
        if(current_tab==i)return none;
        current_tab=i;drawn_tab=-1;
        return (SysmonAction){SYSMON_ACTION_REDRAW,-1,0};
    }
    if(drawn_tab!=current_tab||drawn_client.x!=bx||drawn_client.y!=by||
       drawn_client.w!=bw||drawn_client.h!=bh)return none;
    if(current_tab==SYSMON_TAB_WINDOWS){
        for(int i=0;i<drawn_win_n;i++)if(contains(window_button(&l,i),mx,my)){
            for(int j=0;j<si->win_n&&j<SYSMON_MAX_WIN;j++)
                if(si->win_id[j]==drawn_windows[i]&&si->win_instance[j]==drawn_window_instances[i])
                    return (SysmonAction){SYSMON_ACTION_CLOSE_WINDOW,drawn_windows[i],drawn_window_instances[i]};
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
        for(int k=0;k<drawn_program_n;k++){
            if(!contains(program_button(&l,drawn_task_n+k),mx,my))continue;
            for(int j=0;j<si->program_n&&j<SYSMON_MAX_PROGRAMS;j++)
                if(si->programs[j].pid==drawn_programs[k])
                    return (SysmonAction){SYSMON_ACTION_STOP_PROGRAM,-1,drawn_programs[k]};
        }
    }
    return none;
}
