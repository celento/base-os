#include "fs.h"
#include "term.h"
#include "program.h"
#include "layout.h"
#include "net.h"
#include "download.h"
#include "platform.h"
typedef struct {
    char lines[TERM_LINES][TERM_COLS+1], input[TERM_COLS+1];
    char history[16][TERM_COLS+1], draft[TERM_COLS+1];
    int head,count,len,cwd,hcount,hpos,canvas_on,scroll,rows,view_count,input_overflow;
    unsigned cwd_identity;
    unsigned task_dirty;
    char task_name[TERM_TASK_NAME_LEN];
    char task_document[TERM_TASK_NAME_LEN];
    unsigned task_started, binding_generation;
    ProcessHandle process;
    int canvas_width,canvas_height;
    int canvas_buffered,canvas_pending;
    int published_on,published_width,published_height;
    unsigned char canvas[PROGRAM_CANVAS_MAX_WIDTH*PROGRAM_CANVAS_MAX_HEIGHT];
} Terminal;
#ifndef TERM_MEMORY
#define TERM_MEMORY (APPS_BASE+0x300000)
#endif
static Terminal *terms=(Terminal *)TERM_MEMORY;
#ifndef NATIVE_CANVAS_MEMORY
#define NATIVE_CANVAS_MEMORY NATIVE_CANVAS_BASE
#endif
#define CANVAS_PIXELS (PROGRAM_CANVAS_MAX_WIDTH*PROGRAM_CANVAS_MAX_HEIGHT)
static unsigned char (*published_canvases)[CANVAS_PIXELS]=(void *)NATIVE_CANVAS_MEMORY;
static int selected,terms_ready;
_Static_assert(TERM_TASK_NAME_LEN >= FS_NAME_LEN,"task filename buffer too small");
_Static_assert(sizeof(Terminal)*8<=0xC0000,"terminal arena overflow");
_Static_assert(PROCESS_TASKS*CANVAS_PIXELS<=NATIVE_CANVAS_CAPACITY,"published canvas arena overflow");
#define T (terms[selected])
void term_select(int slot){if(slot>=0&&slot<8)selected=slot;}
static void push_at(Terminal *t,const char *s){t->task_dirty|=TERM_TASK_TEXT;int slot=(t->head+t->count)%TERM_LINES;if(t->count<TERM_LINES)t->count++;else t->head=(t->head+1)%TERM_LINES;int i=0;while(s[i]&&i<TERM_COLS){t->lines[slot][i]=s[i];i++;}t->lines[slot][i]=0;}
static void push(const char *s){push_at(&T,s);}
static void canvas_reset(void){
    T.task_dirty|=TERM_TASK_LAYOUT;
    kmemset(T.canvas,0,sizeof T.canvas);T.canvas_on=0;
    T.canvas_width=PROGRAM_CANVAS_DEFAULT_WIDTH;T.canvas_height=PROGRAM_CANVAS_DEFAULT_HEIGHT;
    /* Invalidate the old frame without exposing its bytes. The next publication
     * copies every active pixel from the cleared working canvas. Keep buffered
     * mode: a script may clear its terminal while a native task is still live. */
    T.canvas_pending=0;T.published_on=0;
    T.published_width=PROGRAM_CANVAS_DEFAULT_WIDTH;T.published_height=PROGRAM_CANVAS_DEFAULT_HEIGHT;
}
void term_reset(void){
    if(!terms_ready){kmemset(terms,0,sizeof(Terminal)*PROCESS_TASKS);terms_ready=1;}
    if(!term_task_close(selected))return;
    unsigned generation=T.binding_generation;
    kmemset(&T,0,sizeof T);T.binding_generation=generation;
    canvas_reset();T.cwd=fs_root();T.cwd_identity=fs_identity(T.cwd);
    push("Type help for commands; man NAME for examples.");push("Page Up / Page Down scroll through output.");
}
int term_count(void){return T.count;}
const char *term_get(int i){return i>=0&&i<T.count?T.lines[(T.head+i)%TERM_LINES]:"";}
const char *term_input(void){return T.input;}
int term_cwd(void){if(!fs_is_dir(T.cwd)||fs_identity(T.cwd)!=T.cwd_identity){T.cwd=fs_root();T.cwd_identity=fs_identity(T.cwd);}return T.cwd;}
void term_set_cwd(int id){if(fs_is_dir(id)){T.cwd=id;T.cwd_identity=fs_identity(id);}}
const unsigned char *term_canvas(void){return T.canvas_buffered?(T.published_on?published_canvases[selected]:0):(T.canvas_on?T.canvas:0);}
int term_canvas_width(void){return T.canvas_buffered?T.published_width:T.canvas_width;}
int term_canvas_height(void){return T.canvas_buffered?T.published_height:T.canvas_height;}
int term_canvas_size(int slot,int *width,int *height){
    if(width)*width=0;
    if(height)*height=0;
    if(slot<0||slot>=PROCESS_TASKS||!width||!height)return 0;
    const Terminal *t=&terms[slot];
    if(!(t->canvas_buffered?t->published_on:t->canvas_on))return 0;
    *width=t->canvas_buffered?t->published_width:t->canvas_width;
    *height=t->canvas_buffered?t->published_height:t->canvas_height;
    return 1;
}
void term_prompt(char *out,int max){char path[FS_PATH_LEN];fs_path(term_cwd(),path,sizeof path);int p=0;for(int i=0;path[i]&&p<max-3;i++)out[p++]=path[i];if(max>2){out[p++]='>';out[p++]=' ';out[p]=0;}}
void term_char(char c){T.scroll=0;if(c>=32&&c<=126){if(T.len<TERM_COLS){T.input[T.len++]=c;T.input[T.len]=0;}else T.input_overflow=1;}}
void term_backspace(void){T.scroll=0;if(T.len)T.input[--T.len]=0;if(!T.len)T.input_overflow=0;}
void term_history(int direction){T.scroll=0;if(!T.hcount)return;if(T.hpos==T.hcount){if(T.input_overflow)T.draft[0]=0;else kstrcpy(T.draft,T.input);}int p=T.hpos+direction;if(p<0)p=0;if(p>T.hcount)p=T.hcount;T.hpos=p;kstrcpy(T.input,p==T.hcount?T.draft:T.history[p]);T.len=kstrlen(T.input);T.input_overflow=0;}
typedef struct { const char *name,*usage,*description,*example,*note; } Manual;
static const Manual commands[]={
    {"help","help [COMMAND]","List commands, or show a command's manual.","help mkdir","Use Page Up / Page Down to read earlier output."},
    {"man","man COMMAND","Show usage, explanation, and an example.","man basic","man and help accept the same command names."},
    {"ls","ls [PATH]","List a folder; / after a name marks a directory.","ls /Programs","Without PATH, lists the terminal's current folder."},
    {"cd","cd PATH","Change this terminal's working folder.","cd /Programs","Paths accept /, . and ..; quote names with spaces."},
    {"pwd","pwd","Print the current folder's absolute path.","pwd","Each Terminal window has its own working folder."},
    {"cat","cat FILE","Print a file as text.","cat /readme.txt","Nonprinting bytes appear as dots; this does not change the file."},
    {"mkdir","mkdir PATH","Create one new folder.","mkdir /Projects","The parent must exist. An existing name is an error."},
    {"touch","touch PATH","Create a new empty file.","touch /Projects/notes.txt","Unlike Unix touch, an existing file is an error."},
    {"mv","mv SOURCE DESTINATION_FOLDER","Move one file/folder into an existing folder; keep its name.","mv /prefs/calendar.v1 /Documents","Exactly two paths; double-quote spaces. No overwrite or rename."},
    {"rm","rm PATH","Permanently delete a file or a folder and its contents.","rm /Projects/old.txt","This bypasses Trash. There is no undo."},
    {"echo","echo TEXT","Print the remaining text on a new line.","echo Hello world","There are no variables, pipes, or output redirection."},
    {"clear","clear","Clear this terminal's output and graphics canvas.","clear","Command history and the current folder are kept."},
    {"stat","stat PATH","Show type, byte size, and modification timestamp.","stat /readme.txt","Time is UTC seconds since 2000; zero means unknown."},
    {"df","df","Show used bytes, payload capacity, free slots, and disk status.","df","Folders and session files consume slots: 256 on IDE, 64 on floppy."},
    {"run","run SCRIPT","Run one terminal command per script line.","run /Programs/demo.sh","Stops on errors; four nested scripts, 256 command dispatches."},
    {"basic","basic FILE","Run numbered, uppercase Tiny BASIC statements.","basic /Programs/demo.bas","PRINT, LET, IF/THEN, GOTO, INKEY, WAIT, PLOT, RECT, REM, END."},
    {"net","net","Show RTL8139 link, QEMU IPv4 settings, and packet counters.","net","QEMU: -nic user,model=rtl8139; fixed guest IP 10.0.2.15."},
    {"ping","ping HOST","Send an ICMP echo and report round-trip time.","ping 10.0.2.2","One bounded request. Some hosts do not answer ICMP."},
    {"nslookup","nslookup HOST","Resolve an IPv4 A record through QEMU DNS.","nslookup example.com","Uses 10.0.2.3; upstream DNS must be reachable on the host."},
    {"fetch","fetch HTTP_URL","Fetch and print up to 4095 bytes of an HTTP response.","fetch http://10.0.2.2:8000/","HTTP only, no TLS. No files are saved. Network waits are bounded."},
    {"download","download HTTP_URL PATH","Download a complete HTTP response to a new file in the background.","download http://10.0.2.2:8000/song.wav /song.wav","Up to 2 MiB on the data disk. Never overwrites. HTTP only, no redirects."},
    {"downloads","downloads [status|cancel]","Show the current download, byte progress and disk save status.","downloads","The download survives closing Terminal. One network request at a time."},
    {"cancel","cancel [download]","Cancel the background download without creating a partial file.","cancel","Only the download is stopped; another app's network request is untouched."},
    {"start","start FILE [DOCUMENT]","Start a protected BEX1 app, optionally with a document path.","start /Programs/docstats.bex /Documents/stats-sample.txt","Quote paths with spaces. Ctrl+C stops; closing this window stops it."},
    {"stop","stop","Stop this terminal's native task.","stop","Ctrl+C also stops a task without waiting for the program."},
    {"tasks","tasks","List the running native task slots.","tasks","Sleeping and minimized tasks remain alive; closing a terminal stops it."},
    {"exec","exec FILE","Run a BEX1 native x86 program in protected memory.","exec /Programs/hello.bex","64 KB memory; two-second limit. Faults return to the terminal."}
};
static int manual(const char *name){
    if(!*name){
        push("COMMANDS - use man NAME for usage and examples");
        for(unsigned i=0;i<sizeof commands/sizeof *commands;i++)push(commands[i].usage);
        push("Paths with spaces: cd \"/test folder\"");
        push("Up/Down: history. Tab: completion. Page Up/Down: scroll.");
        return 0;
    }
    for(unsigned i=0;i<sizeof commands/sizeof *commands;i++)if(!kstrcmp(name,commands[i].name)){
        push(commands[i].usage);push(commands[i].description);push(commands[i].note);
        push("Example:");push(commands[i].example);
        if(!kstrcmp(name,"basic")){push("Canvas: 160x100; colors: 0..255; variables: A..Z.");push("Limit: 256 lines, 10000 statements, ten seconds.");}
        if(!kstrcmp(name,"mv")){
            push("Example with spaces: mv \"/old notes\" \"/Documents/archive folder\"");
            push("Absolute or relative paths; no options, wildcards, or quote escapes.");
            push("Root, apps, and folders containing apps cannot be moved.");
            push("A folder cannot move into itself or its children.");
            push("Changes stay in RAM until a successful disk save; busy means retry.");
            push("Commands are limited to 80 characters; cd closer for long paths.");
        }
        return 0;
    }
    push("No manual for that name. Type help to list commands.");return -1;
}
void term_set_view(int rows,int total){T.rows=rows;T.view_count=total;int max=total-rows;if(max<0)max=0;if(T.scroll>max)T.scroll=max;}
void term_set_rows(int rows){term_set_view(rows,T.count+1);}
void term_scroll(int delta){T.scroll+=delta;if(T.scroll<0)T.scroll=0;term_set_view(T.rows,T.view_count);}
int term_scroll_offset(void){return T.scroll;}
static int prefix(const char *a,const char *b){while(*b)if(*a++!=*b++)return 0;return 1;}
void term_complete(void){
    int start=T.len;while(start&&T.input[start-1]!=' ')start--;
    const char *match=0;int count=0,dir=0;
    if(!start){for(unsigned i=0;i<sizeof commands/sizeof *commands;i++)if(prefix(commands[i].name,T.input)){match=commands[i].name;count++;}}
    else {char parent[TERM_COLS+1];int slash=start;for(int i=start;i<T.len;i++)if(T.input[i]=='/')slash=i+1;
        int cwd=term_cwd();if(slash>start){kmemcpy(parent,T.input+start,slash-start);parent[slash-start]=0;cwd=fs_resolve(cwd,parent);}start=slash;
        int ids[FS_MAX_NODES],n=fs_list(cwd,ids,FS_MAX_NODES);for(int i=0;i<n;i++)if(prefix(fs_name(ids[i]),T.input+start)){match=fs_name(ids[i]);dir=fs_is_dir(ids[i]);count++;}}
    if(count==1&&start+kstrlen(match)+1<=TERM_COLS){kstrcpy(T.input+start,match);T.len=kstrlen(T.input);term_char(dir?'/':' ');}else if(count>1)push("Multiple matches; type more of the name.");
}
static void print_number(const char *label,unsigned n){char out[81],rev[12];int p=0,r=0;while(*label&&p<65)out[p++]=*label++;do{rev[r++]='0'+n%10;n/=10;}while(n);while(r)out[p++]=rev[--r];out[p]=0;push(out);}
static void print_network(void){
    const NetStatus *status=net_status();char ip[16],row[40];
    push(!status->available?"RTL8139: unavailable":status->link_up?"RTL8139: link up":"RTL8139: link down");
    net_format_ipv4(status->address,ip);kstrcpy(row,"IPv4: ");kstrcpy(row+6,ip);push(row);
    net_format_ipv4(status->gateway,ip);kstrcpy(row,"Gateway: ");kstrcpy(row+9,ip);push(row);
    net_format_ipv4(status->dns,ip);kstrcpy(row,"DNS: ");kstrcpy(row+5,ip);push(row);
    kstrcpy(row,"MAC: ");for(int i=0;i<6;i++){row[5+i*3]="0123456789abcdef"[status->mac[i]>>4];row[6+i*3]="0123456789abcdef"[status->mac[i]&15];row[7+i*3]=i==5?0:':';}push(row);
    print_number("Packets received: ",status->rx_packets);print_number("Packets sent: ",status->tx_packets);
    print_number("Packets dropped: ",status->dropped_packets);print_number("Receive errors: ",status->rx_errors);
    push(!status->available||!status->link_up?"Network offline":net_busy()?"Network request in progress":"Network ready; HTTP only (unencrypted)");
}
static void print_http_body(const char *text,unsigned size){
    char row[81];unsigned n=0;for(unsigned i=0;i<size;i++){
        unsigned char c=(unsigned char)text[i];if(c=='\r')continue;
        if(c=='\n'){row[n]=0;push(row);n=0;continue;}
        row[n++]=c>=32&&c<=126?(char)c:'.';if(n==80){row[n]=0;push(row);n=0;}
    }if(n){row[n]=0;push(row);}
}
static void print_download(void){
    const DownloadStatus *d=download_status();
    push(d->state==DOWNLOAD_IDLE?"No download yet.":d->state==DOWNLOAD_ACTIVE?"Download in progress":d->state==DOWNLOAD_DONE?"Download complete":d->state==DOWNLOAD_CANCELLED?"Download cancelled":"Download failed");
    if(d->state==DOWNLOAD_IDLE)return;
    push(d->url);push(d->path);print_number("Received bytes: ",d->received);print_number("File byte limit: ",d->limit);
    if(d->http_status)print_number("HTTP status: ",(unsigned)d->http_status);
    if(d->state==DOWNLOAD_ACTIVE)push(d->http_state==NET_HTTP_DONE?d->message:d->http_state==NET_HTTP_RESOLVING?"Looking up host...":d->http_state==NET_HTTP_CONNECTING?"Connecting...":"Receiving body...");
    else if(d->state==DOWNLOAD_DONE){
        /* The service tracks the committed file version, unlike global dirty
         * state. Wrap its full message so later-change diagnostics survive. */
        print_http_body(d->message,(unsigned)kstrlen(d->message));
    }else push(d->message);
}
static int download_command(int cwd,const char *url,const char *remaining,int quoted){
    if(quoted)remaining++;
    while(*remaining==' ')remaining++;
    char path[81];int count=0;char quote=*remaining=='"'?*remaining++:0;
    while(*remaining&&(quote?*remaining!=quote:*remaining!=' ')&&count<80)path[count++]=*remaining++;
    path[count]=0;if(quote){if(*remaining!='"')return -1;remaining++;}
    while(*remaining==' ')remaining++;
    if(!url[0]||!path[0]||*remaining){push("Usage: download HTTP_URL PATH (quote names containing spaces)");return -1;}
    if(download_start(cwd,url,path)){push(download_last_error());return -1;}
    push("Download started in background. Use downloads for status; cancel to stop.");return 0;
}
static void cat(int id){char row[81];int n=0;for(int i=0;i<fs_size(id);i++){char c=fs_data(id)[i];if(c=='\r')continue;if(c=='\n'){row[n]=0;push(row);n=0;continue;}row[n++]=c>=32&&c<=126?c:'.';if(n==80){row[n]=0;push(row);n=0;}}if(n){row[n]=0;push(row);}}
static void plot_at(Terminal *t,int x,int y,int color){
    if(x>=0&&x<t->canvas_width&&y>=0&&y<t->canvas_height){
        if(t->canvas_buffered)t->canvas_pending=1;
        else {
            t->task_dirty|=TERM_TASK_CANVAS;
            if(!t->canvas_on)t->task_dirty|=TERM_TASK_LAYOUT;
        }
        t->canvas_on=1;t->canvas[y*t->canvas_width+x]=(unsigned char)color;
    }
}
/* Clip before adding coordinates: callers may supply any signed origin.
 * Only working pixels change; publication still belongs to an explicit frame
 * boundary. Empty/offscreen rectangles must not activate or dirty the canvas. */
static void canvas_rect_at(Terminal *t,int x,int y,int width,int height,int color){
    if(width<=0||height<=0||x>=t->canvas_width||y>=t->canvas_height)return;
    if(x<0){if(x<=-width)return;width+=x;x=0;}
    if(y<0){if(y<=-height)return;height+=y;y=0;}
    if(width>t->canvas_width-x)width=t->canvas_width-x;
    if(height>t->canvas_height-y)height=t->canvas_height-y;
    if(t->canvas_buffered)t->canvas_pending=1;
    else {
        t->task_dirty|=TERM_TASK_CANVAS;
        if(!t->canvas_on)t->task_dirty|=TERM_TASK_LAYOUT;
    }
    t->canvas_on=1;
    unsigned char *row=t->canvas+y*t->canvas_width+x;
    for(int i=0;i<height;i++)kmemset(row+i*t->canvas_width,color,width);
}
static int canvas_resize_at(Terminal *t,int width,int height){
    if(!((width==(int)PROGRAM_CANVAS_DEFAULT_WIDTH&&height==(int)PROGRAM_CANVAS_DEFAULT_HEIGHT)||
         (width==(int)PROGRAM_CANVAS_MAX_WIDTH&&height==(int)PROGRAM_CANVAS_MAX_HEIGHT)))return -1;
    kmemset(t->canvas,0,sizeof t->canvas);
    t->canvas_width=width;t->canvas_height=height;t->canvas_on=1;
    if(t->canvas_buffered)t->canvas_pending=1;
    else t->task_dirty|=TERM_TASK_LAYOUT;
    return 0;
}
/* Called only at a native task's explicit frame boundary for its explicit
 * Terminal attachment. The bounded copy never polls or dispatches apps. PIT
 * interrupts cannot switch ring-0 work, so pixels and metadata publish together. */
static void canvas_publish_at(Terminal *t){
    if(!t->canvas_buffered||!t->canvas_pending)return;
    int layout=t->published_on!=t->canvas_on||t->published_width!=t->canvas_width||
               t->published_height!=t->canvas_height;
    if(t->canvas_on)kmemcpy(published_canvases[t-terms],t->canvas,t->canvas_width*t->canvas_height);
    t->published_on=t->canvas_on;t->published_width=t->canvas_width;t->published_height=t->canvas_height;
    t->canvas_pending=0;
    t->task_dirty|=layout?TERM_TASK_LAYOUT:TERM_TASK_CANVAS;
}
static void plot(int x,int y,int color){plot_at(&T,x,y,color);}
static void canvas_rect(int x,int y,int width,int height,int color){canvas_rect_at(&T,x,y,width,height,color);}
static int canvas_resize(int width,int height){return canvas_resize_at(&T,width,height);}
/* Resolve all three fields; callbacks for an old process/binding are discarded.
 * No callback changes selected or dispatches another process. */
static Terminal *binding_terminal(const ProcessBinding *binding){
    if(!binding||binding->slot>=PROCESS_TASKS||!binding->process||!binding->generation)return 0;
    Terminal *t=terms+binding->slot;
    return t->process==binding->process&&t->binding_generation==binding->generation?t:0;
}
static void native_print(const ProcessBinding *binding,const char *text){
    Terminal *t=binding_terminal(binding);if(t)push_at(t,text);
}
static void native_plot(const ProcessBinding *binding,int x,int y,int color){
    Terminal *t=binding_terminal(binding);if(t)plot_at(t,x,y,color);
}
static void native_present(const ProcessBinding *binding){
    Terminal *t=binding_terminal(binding);if(t)canvas_publish_at(t);
}
static int native_resize(const ProcessBinding *binding,int width,int height){
    Terminal *t=binding_terminal(binding);return t?canvas_resize_at(t,width,height):-1;
}
static void native_rect(const ProcessBinding *binding,int x,int y,int width,int height,int color){
    Terminal *t=binding_terminal(binding);if(t)canvas_rect_at(t,x,y,width,height,color);
}
int term_task_running(int slot){
    if(slot<0||slot>=PROCESS_TASKS)return 0;
    int state=process_status(terms[slot].process);
    return state==PROCESS_TASK_READY||state==PROCESS_TASK_SLEEPING;
}
int term_task_start_file(int slot,int file,unsigned identity){
    return term_task_start_file_with_arg(slot,file,identity,0,0);
}
int term_task_start_file_with_arg(int slot,int file,unsigned identity,
                                   const char *argument,unsigned argument_length){
    if(slot<0||slot>=PROCESS_TASKS)return -1;
    int previous=selected;term_select(slot);
    int result=-1;
    if(T.process&&process_status(T.process)==PROCESS_TASK_DONE)term_task_close(slot);
    if(!identity||!fs_valid(file)||fs_is_dir(file)||fs_is_app(file)||fs_identity(file)!=identity)
        push("Cannot start: the selected program changed or is no longer a file.");
    else if(T.process)
        push("Cannot start: this terminal already has a native task.");
    else if(argument_length>PROCESS_ARGUMENT_MAX||
            (argument_length&&(!argument||argument[0]!='/')))
        push("Cannot start: use an absolute document path of at most 128 bytes.");
    else if(T.binding_generation==UINT32_MAX)
        push("Cannot start: this terminal's binding identities are exhausted.");
    else {
        ProcessHandle process=0;
        /* No task runs between this identity check and the loader's owned copy. */
        int created=process_create(fs_data(file),fs_size(file),argument,argument_length,&process);
        if(created)
            push(created==PROCESS_CREATE_MEMORY?"Cannot start: native backing memory is unavailable.":
                 created==-1?"Cannot start: all native process records are in use.":
                 "Cannot start: not a supported BEX1 program (maximum 49152 bytes).");
        else {
            ProcessIO io={{process,(unsigned)slot,T.binding_generation+1},
                          native_print,native_plot,native_present,native_resize,native_rect};
            if(!process_bind(process,&io)){
                process_request_stop(process);process_reap(process);
                push("Cannot start: native output attachment is unavailable.");
                selected=previous;return -1;
            }
            /* Commit both sides before it can run; START never invokes output. */
            T.process=process;T.binding_generation=io.binding.generation;
            if(!process_start(process)){
                T.process=0;process_request_stop(process);process_reap(process);
                push("Cannot start: native process is unavailable.");
                selected=previous;return -1;
            }
            kstrcpy(T.task_name,fs_name(file));T.task_started=timer_ticks();
            unsigned first=0;
            for(unsigned i=0;i<argument_length;i++)if(argument[i]=='/')first=i+1;
            unsigned n=argument_length-first;
            if(n>=sizeof T.task_document)n=sizeof T.task_document-1;
            if(n)kmemcpy(T.task_document,argument+first,n);
            T.task_document[n]=0;
            T.task_dirty|=TERM_TASK_LIFECYCLE;
            canvas_reset();T.canvas_buffered=1;T.scroll=0;
            push(T.task_name);
            push("Native task started. Ctrl+C stops; close ends it.");
            result=0;
        }
    }
    selected=previous;return result;
}
int term_task_info(int slot,TermTaskInfo *out){
    if(!out)return 0;
    kmemset(out,0,sizeof *out);
    if(slot<0||slot>=PROCESS_TASKS)return 0;
    int state=process_status(terms[slot].process);
    if(state!=PROCESS_TASK_READY&&state!=PROCESS_TASK_SLEEPING)return 0;
    const Terminal *t=&terms[slot];
    kstrcpy(out->name,t->task_name[0]?t->task_name:"Native task");
    kstrcpy(out->document,t->task_document);
    out->owner=slot;out->state=state;
    out->started_ticks=t->task_started;out->instance=t->process;
    out->elapsed_sec=t->process?(unsigned)(timer_ticks()-t->task_started)/TIMER_HZ:0;
    return 1;
}
int term_task_title(int slot,char *out,int capacity){
    if(!out||capacity<=0)return 0;
    out[0]=0;TermTaskInfo info;
    if(!term_task_info(slot,&info))return 0;
    const char *parts[3]={info.name,info.document[0]?" - ":"",info.document};
    int used=0;
    for(int p=0;p<3;p++)for(int i=0;parts[p][i]&&used<capacity-1;i++)out[used++]=parts[p][i];
    out[used]=0;return 1;
}
static void task_metadata_clear(int slot){
    if(slot<0||slot>=PROCESS_TASKS)return;
    if(terms[slot].process)terms[slot].task_dirty|=TERM_TASK_LIFECYCLE;
    kmemset(terms[slot].task_name,0,sizeof terms[slot].task_name);
    kmemset(terms[slot].task_document,0,sizeof terms[slot].task_document);
    terms[slot].task_started=terms[slot].process=0;
}
int term_task_key(int slot,int key){
    return slot>=0&&slot<PROCESS_TASKS?process_key(terms[slot].process,key):0;
}
int term_task_close(int slot){
    if(slot<0||slot>=PROCESS_TASKS)return 1;
    ProcessHandle process=terms[slot].process;
    if(process){
        if(!process_request_stop(process))return 0;
        ProcessResult result;
        /* Completion is consumed before record reuse, even for explicit close. */
        process_get_result(process,&result);
        if(!process_reap(process))return 0;
    }
    task_metadata_clear(slot);
    /* Stop/error/close discards unfinished work, retaining the published view. */
    terms[slot].canvas_pending=0;return 1;
}
void term_task_stop(int slot){
    if(!term_task_running(slot))return;
    if(!process_request_stop(terms[slot].process))return;
    push_at(terms+slot,"Native task stopped.");term_task_close(slot);
}
TermTaskUpdate term_task_poll_update(void){
    TermTaskUpdate update={-1,0};
    int slot=-1;
    /* Consume retained completions before another slice, so a continuously
     * runnable peer cannot starve a deferred stop's display/reap. */
    for(unsigned i=0;i<PROCESS_TASKS;i++)
        if(terms[i].process&&process_status(terms[i].process)==PROCESS_TASK_DONE){slot=(int)i;break;}
    if(slot<0){
        ProcessHandle ran=process_schedule_one();
        for(unsigned i=0;i<PROCESS_TASKS;i++)
            if(ran&&terms[i].process==ran){slot=(int)i;break;}
        /* A queued stop may complete without running a slice. */
        if(slot<0)for(unsigned i=0;i<PROCESS_TASKS;i++)
            if(terms[i].process&&process_status(terms[i].process)==PROCESS_TASK_DONE){slot=(int)i;break;}
    }
    if(slot>=0){
        Terminal *t=terms+slot;ProcessResult result;
        if(process_get_result(t->process,&result)){
            if(result.reason==PROCESS_EXIT_STOP)push_at(t,"Native task stopped.");
            else if(result.reason==PROCESS_EXIT_APP&&!result.value)push_at(t,"Native task finished.");
            else push_at(t,"Native task ended with an error or fault.");
            term_task_close(slot);
        }
        update.slot=slot;update.flags=t->task_dirty;t->task_dirty=0;
    }
    return update;
}
int term_task_poll(void){return term_task_poll_update().flags!=0;}
static int execute(const char *,int,int *);
/* Unlike the legacy one-argument commands, mv must consume two complete
 * operands before resolving either path. Do not accept quoted-prefix suffixes,
 * embedded quotes, or a truncated operand as an accidental different path. */
static int move_path(const char **remaining,char out[FS_PATH_LEN]){
    const char *p=*remaining;
    while(*p==' '||*p=='\t')p++;
    int quoted=*p=='"',n=0;
    if(quoted)p++;
    while(*p&&(quoted?*p!='"':(*p!=' '&&*p!='\t'))){
        if(*p=='"'||n>=FS_PATH_LEN-1)return -1;
        out[n++]=*p++;
    }
    out[n]=0;
    if(!n)return -1;
    if(quoted){if(*p!='"')return -1;p++;}
    if(*p&&*p!=' '&&*p!='\t')return -1;
    *remaining=p;return 0;
}
static int move_within(int node,int ancestor){
    for(int depth=0;node>=0&&depth<=FS_MAX_DEPTH;depth++){
        if(node==ancestor)return 1;
        node=fs_parent(node);
    }
    return 0;
}
static int move_command(int cwd,const char *remaining){
    char source[FS_PATH_LEN],destination[FS_PATH_LEN];
    if(move_path(&remaining,source)||move_path(&remaining,destination))goto usage;
    while(*remaining==' '||*remaining=='\t')remaining++;
    if(*remaining)goto usage;
    int id=fs_resolve(cwd,source),parent=fs_resolve(cwd,destination);
    if(!fs_valid(id)||id==fs_root()||fs_is_app(id)){
        push("Cannot move: source must be an ordinary file or folder, not root or an app.");return -1;
    }
    if(!fs_is_dir(parent)){
        push("Cannot move: destination must be an existing folder.");return -1;
    }
    for(int app=1;app<fs_node_limit();app++)if(fs_is_app(app)&&move_within(app,id)){
        push("Cannot move: source folder contains an app.");return -1;
    }
    if(move_within(parent,id)){
        push("Cannot move: a folder cannot move into itself or its children.");return -1;
    }
    /* fs_move's legacy collision policy generates a new name. Reject clashes
     * first so this command always preserves the basename. No app dispatch or
     * filesystem mutation occurs between this preflight and fs_move. */
    int clash=fs_find_child(parent,fs_name(id));
    if(clash>=0&&clash!=id){
        push("Cannot move: that name already exists in the destination folder.");return -1;
    }
    if(fs_sync_busy())return FS_ERR_BUSY;
    if(fs_parent(id)==parent){push("Already in that folder; nothing moved.");return 0;}
    int result=fs_move(id,parent);
    if(result==FS_ERR_BUSY)return result;
    if(result<0){push("Cannot move: destination path is too deep or invalid.");return result;}
    push("Moved in RAM; not yet saved to disk.");return 0;
usage:
    push("Usage: mv SOURCE DESTINATION_FOLDER (exactly two paths; double-quote spaces)");
    return -1;
}
static int start_command(int file,const char *remaining,int quoted){
    if(quoted)remaining++;
    if(*remaining&&*remaining!=' ')return -1;
    while(*remaining==' ')remaining++;
    if(!*remaining)return term_task_start_file(selected,file,fs_identity(file));
    char input[TERM_COLS+1];unsigned n=0;char quote=*remaining=='"'?*remaining++:0;
    while(*remaining&&(quote?*remaining!=quote:*remaining!=' ')&&n<TERM_COLS)
        input[n++]=*remaining++;
    input[n]=0;
    if(quote){if(*remaining!='"')return -1;remaining++;}
    while(*remaining==' ')remaining++;
    if(!n||*remaining){push("Usage: start FILE [DOCUMENT] (quote paths with spaces)");return -1;}
    int document=fs_resolve(term_cwd(),input);
    if(!fs_valid(document)||fs_is_dir(document)||fs_is_app(document)){
        push("Cannot start: the document is not an ordinary file.");return -1;
    }
    char path[FS_PATH_LEN];fs_path(document,path,sizeof path);
    return term_task_start_file_with_arg(selected,file,fs_identity(file),path,(unsigned)kstrlen(path));
}
static int script(int id,int depth,int *budget){
    if(depth>=4||!fs_valid(id)||fs_is_dir(id))return -1;
    /* Copy each script so deleting its source cannot change the running program. */
    char *source=(char *)(TERM_MEMORY+0xC0000+depth*FS_MAX_SIZE);int size=fs_size(id);if(size>=FS_MAX_SIZE)return -1;kmemcpy(source,fs_data(id),size);int pos=0;
    while(pos<size){char line[81];int n=0;while(pos<size&&source[pos]!='\n'){char c=source[pos++];if(c=='\r')continue;if(n==80)return -1;line[n++]=c;}pos++;line[n]=0;int result=execute(line,depth+1,budget);if(result)return result;}return 0;
}
static int execute(const char *s,int depth,int *budget){
    if(--*budget<0)return -1;
    while(*s==' ')s++;
    if(!*s||*s=='#')return 0;
    char cmd[81];int n=0;while(*s&&*s!=' '&&n<80)cmd[n++]=*s++;cmd[n]=0;while(*s==' ')s++;
    if(!kstrcmp(cmd,"mv"))return move_command(term_cwd(),s);
    char arg[81];int a=0;const char *p=s;char quote=*p=='"'?*p++:0;
    while(*p&&(quote?*p!=quote:*p!=' ')&&a<80)arg[a++]=*p++;
    arg[a]=0;
    if(quote&&*p!='"')return -1;
    int cwd=term_cwd(),id=arg[0]?fs_resolve(cwd,arg):cwd;
    if(!kstrcmp(cmd,"help")||!kstrcmp(cmd,"man"))return manual(arg);
    else if(!kstrcmp(cmd,"echo"))push(s);
    else if(!kstrcmp(cmd,"clear")){T.head=T.count=0;canvas_reset();}
    else if(!kstrcmp(cmd,"net"))print_network();
    else if(!kstrcmp(cmd,"download"))return download_command(cwd,arg,p,quote!=0);
    else if(!kstrcmp(cmd,"downloads")){
        if(!arg[0]||!kstrcmp(arg,"status"))print_download();
        else if(!kstrcmp(arg,"cancel")){if(!download_cancel())push("No background download is running.");else push(download_status()->message);}
        else return -1;
    }
    else if(!kstrcmp(cmd,"cancel")){
        if(arg[0]&&kstrcmp(arg,"download"))return -1;
        if(!download_cancel())push("No background download is running.");else push(download_status()->message);
    }
    else if(!kstrcmp(cmd,"ping")||!kstrcmp(cmd,"nslookup")||!kstrcmp(cmd,"fetch")){
        if(!arg[0])return -1;
        if(net_busy()){push("Network is busy; wait for the current request or stop it.");return -1;}
        if(!kstrcmp(cmd,"ping")){unsigned ms;if(net_ping(arg,&ms)){push(net_last_error());return -1;}print_number("ICMP reply, round-trip milliseconds: ",ms);}
        else if(!kstrcmp(cmd,"nslookup")){uint32_t address;char ip[16];if(net_resolve(arg,&address)){push(net_last_error());return -1;}net_format_ipv4(address,ip);push(ip);}
        else {char body[4096];if(net_http_get(arg,body,sizeof body)){push(net_last_error());return -1;}const NetHttpResult *result=net_http_result();print_number("HTTP status: ",(unsigned)result->status);if(result->content_type[0])push(result->content_type);print_http_body(body,result->length);if(result->truncated)push("[Response truncated to 4095 bytes]");if(result->location[0]){push("Redirect Location:");push(result->location);}}
    }
    else if(!kstrcmp(cmd,"pwd")){char path[FS_PATH_LEN];fs_path(cwd,path,sizeof path);print_http_body(path,(unsigned)kstrlen(path));}
    else if(!kstrcmp(cmd,"cd")){if(!fs_is_dir(id))return -1;term_set_cwd(id);}
    else if(!kstrcmp(cmd,"ls")){if(!fs_is_dir(id))return -1;int ids[FS_MAX_NODES],count=fs_list(id,ids,FS_MAX_NODES);for(int i=0;i<count;i++){char row[26];kstrcpy(row,fs_name(ids[i]));if(fs_is_dir(ids[i]))kstrcpy(row+kstrlen(row),"/");push(row);}}
    else if(!kstrcmp(cmd,"cat")){if(!arg[0]||!fs_valid(id)||fs_is_dir(id))return -1;cat(id);}
    else if(!kstrcmp(cmd,"mkdir")||!kstrcmp(cmd,"touch")){
        if(fs_sync_busy())return FS_ERR_BUSY;
        char name[FS_NAME_LEN];int parent=fs_destination(cwd,arg,name);if(parent<0)return -1;
        int result=!kstrcmp(cmd,"mkdir")?fs_mkdir(parent,name):fs_create(parent,name);if(result<0)return result;
    }
    else if(!kstrcmp(cmd,"rm")){
        if(fs_sync_busy())return FS_ERR_BUSY;
        if(!arg[0])return -1;
        int result=fs_delete(id);if(result<0)return result;
    }
    else if(!kstrcmp(cmd,"stat")){if(!fs_valid(id))return -1;push(fs_name(id));push(fs_is_dir(id)?"Directory":"File");print_number("Bytes: ",fs_size(id));print_number("Modified (UTC seconds since 2000; 0=unknown): ",fs_modified(id));}
    else if(!kstrcmp(cmd,"df")){print_number("Bytes used: ",fs_used_bytes());print_number("Payload capacity: ",fs_capacity());print_number("Free node slots: ",fs_node_limit()-fs_node_count());print_number("Maximum file bytes: ",fs_file_limit());push(fs_storage_name());push("Folders also consume file slots.");push(fs_storage_status()?fs_storage_status():"Disk is synchronized.");}
    else if(!kstrcmp(cmd,"run"))return script(id,depth,budget);
    else if(!kstrcmp(cmd,"tasks")){
        int found=0;
        for(int slot=0;slot<PROCESS_TASKS;slot++)if(term_task_running(slot)){
            print_number(process_status(terms[slot].process)==PROCESS_TASK_SLEEPING?"Sleeping, terminal slot ":"Running, terminal slot ",(unsigned)slot+1);found=1;
            char title[TERM_TASK_TITLE_LEN];term_task_title(slot,title,sizeof title);push(title);
        }
        if(!found)push("No native tasks are running.");
    }
    else if(!kstrcmp(cmd,"stop")){if(!term_task_running(selected))push("No native task in this terminal.");else term_task_stop(selected);}
    else if(!kstrcmp(cmd,"start")){
        if(!arg[0]||!fs_valid(id)||fs_is_dir(id))return -1;
        return start_command(id,p,quote!=0);
    }
    else if(!kstrcmp(cmd,"basic")||!kstrcmp(cmd,"exec")){
        if(!arg[0]||!fs_valid(id)||fs_is_dir(id))return -1;
        if(term_task_running(selected)){push("Stop this terminal's native task first.");return -1;}
        ProgramIO io={push,plot,program_key,program_present,canvas_resize,canvas_rect};T.canvas_buffered=0;canvas_reset();
        int rc=!kstrcmp(cmd,"basic")?basic_run(fs_data(id),fs_size(id),&io):process_run(fs_data(id),fs_size(id),&io);
        if(rc){push("Program stopped (error, fault, or execution limit).");return -1;}push("Program finished.");
    }else return -1;return 0;
}
void term_enter(void){
    T.scroll=0;
    char line[81];kstrcpy(line,T.input);push(line);
    int overflow=T.input_overflow;T.input_overflow=0;
    if(T.len&&!overflow){if(T.hcount==16){for(int i=1;i<16;i++)kstrcpy(T.history[i-1],T.history[i]);T.hcount--;}kstrcpy(T.history[T.hcount++],line);}
    T.hpos=T.hcount;T.len=0;T.input[0]=0;T.draft[0]=0;int budget=256;
    if(overflow){push("Command exceeds 80 characters; nothing was run. Use cd for shorter paths.");return;}
    int result=execute(line,0,&budget);
    if(result==FS_ERR_BUSY)push("Disk is saving; retry shortly.");
    else if(result)push("Error: check command, path, syntax, or available space.");
}
