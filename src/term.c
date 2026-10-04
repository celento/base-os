#include "fs.h"
#include "term.h"
#include "program.h"
#include "layout.h"
#include "net.h"
#include "download.h"
#include "platform.h"
#include "app_storage.h"
typedef TerminalText Terminal;
static Terminal *terms=((AppStorage *)TERM_MEMORY)->terminals;
static int selected;
static int (*native_launch_hook)(int,int,unsigned,const char *,unsigned);
void term_set_native_launch(int (*hook)(int,int,unsigned,const char *,unsigned)){
    native_launch_hook=hook;
}
#define T (terms[selected])
void term_select(int slot){if(slot>=0&&slot<PROCESS_TASKS)selected=slot;}
static void push_at(Terminal *t,const char *s){app_view_mark((int)(t-terms),TERM_TASK_TEXT);int slot=(t->head+t->count)%TERM_LINES;if(t->count<TERM_LINES)t->count++;else t->head=(t->head+1)%TERM_LINES;int i=0;while(s[i]&&i<TERM_COLS){t->lines[slot][i]=s[i];i++;}t->lines[slot][i]=0;}
void term_write_at(int slot,const char *text){if(slot>=0&&slot<PROCESS_TASKS)push_at(terms+slot,text);}
static void push(const char *s){push_at(&T,s);}
static void canvas_reset(void){app_view_canvas_reset(selected);}
void term_reset(void){
    if(!app_view_reset(selected))return;
    kmemset(&T,0,sizeof T);T.cwd=fs_root();T.cwd_identity=fs_identity(T.cwd);
    push("Type help for commands; man NAME for examples.");push("Page Up / Page Down scroll through output.");
}
int term_count(void){return T.count;}
const char *term_get(int i){return i>=0&&i<T.count?T.lines[(T.head+i)%TERM_LINES]:"";}
const char *term_input(void){return T.input;}
int term_cwd(void){if(!fs_is_dir(T.cwd)||fs_identity(T.cwd)!=T.cwd_identity){T.cwd=fs_root();T.cwd_identity=fs_identity(T.cwd);}return T.cwd;}
void term_set_cwd(int id){if(fs_is_dir(id)){T.cwd=id;T.cwd_identity=fs_identity(id);}}
const unsigned char *term_canvas(void){return app_view_frame(selected).pixels;}
int term_canvas_width(void){return app_view_frame(selected).width;}
int term_canvas_height(void){return app_view_frame(selected).height;}
int term_canvas_size(int slot,int *width,int *height){return app_view_canvas_size(slot,width,height);}
void term_prompt(char *out,int max){char path[FS_PATH_LEN];fs_path(term_cwd(),path,sizeof path);int p=0;for(int i=0;path[i]&&p<max-3;i++)out[p++]=path[i];if(max>2){out[p++]='>';out[p++]=' ';out[p]=0;}}
void term_char(char c){T.scroll=0;if(c>=32&&c<=126){if(T.len<TERM_COLS){T.input[T.len++]=c;T.input[T.len]=0;}else T.input_overflow=1;}}
void term_backspace(void){T.scroll=0;if(T.len)T.input[--T.len]=0;if(!T.len)T.input_overflow=T.input_invalid=0;}
void term_input_lost(int slot){
    if(slot<0||slot>=PROCESS_TASKS)return;
    Terminal *t=&terms[slot];t->input_invalid=1;
    push_at(t,"Input was lost. Clear the line with Backspace before running it.");
}
void term_history(int direction){T.scroll=0;if(!T.hcount)return;if(T.hpos==T.hcount){if(T.input_overflow)T.draft[0]=0;else kstrcpy(T.draft,T.input);T.draft_invalid=T.input_invalid;}int p=T.hpos+direction;if(p<0)p=0;if(p>T.hcount)p=T.hcount;T.hpos=p;kstrcpy(T.input,p==T.hcount?T.draft:T.history[p]);T.len=kstrlen(T.input);T.input_overflow=0;T.input_invalid=p==T.hcount?T.draft_invalid:0;}
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
    {"start","start FILE [DOCUMENT]","Start a native app, optionally with a document path.","start /Programs/docstats.bex /Documents/stats-sample.txt","GUI apps open their own window. Hosted apps use this Terminal."},
    {"stop","stop","Stop this terminal's native task.","stop","Ctrl+C also stops a task without waiting for the program."},
    {"tasks","tasks","List the running native task slots.","tasks","Sleeping and minimized tasks remain alive; closing their window stops them."},
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
/* Synchronous BASIC/exec retain their selected-Terminal, unbuffered adapter. */
static void plot(int x,int y,int color){app_view_plot(selected,x,y,color);}
static void canvas_rect(int x,int y,int width,int height,int color){app_view_rect(selected,x,y,width,height,color);}
static int canvas_resize(int width,int height){return app_view_resize(selected,width,height);}
int term_binding_matches(const ProcessBinding *binding){return app_view_binding_matches(binding);}
int term_task_running(int slot){return app_view_running(slot);}
int term_task_start_file(int slot,int file,unsigned identity){return term_task_start_file_with_arg(slot,file,identity,0,0);}
int term_task_start_file_with_arg(int slot,int file,unsigned identity,const char *argument,unsigned argument_length){
    int result=app_view_start_file(slot,file,identity,argument,argument_length,APP_VIEW_OUTPUT_TERMINAL);
    if(!result)terms[slot].scroll=0;
    return result;
}
int term_task_info(int slot,TermTaskInfo *out){return app_view_info(slot,out);}
int term_task_title(int slot,char *out,int capacity){return app_view_title(slot,out,capacity);}
int term_task_key(int slot,int key){return app_view_key(slot,key);}
int term_task_close(int slot){return app_view_close(slot);}
void term_task_stop(int slot){app_view_stop(slot);}
TermTaskUpdate term_task_poll_update(void){return app_view_poll_update();}
int term_task_poll(void){return app_view_poll();}
static void (*program_input_hook)(int active);
void term_set_program_input(void (*hook)(int active)) { program_input_hook=hook; }
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
static int start_dispatch(int file,const char *argument,unsigned length){
    int owner=selected;
    int result=native_launch_hook?native_launch_hook(owner,file,fs_identity(file),argument,length):
        term_task_start_file_with_arg(owner,file,fs_identity(file),argument,length);
    /* Focus may now belong to a GUI, but the remainder of this shell command
     * (or bounded script) still belongs to its original Terminal. */
    term_select(owner);return result;
}
static int start_command(int file,const char *remaining,int quoted){
    if(quoted)remaining++;
    if(*remaining&&*remaining!=' ')return -1;
    while(*remaining==' ')remaining++;
    if(!*remaining)return start_dispatch(file,0,0);
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
    return start_dispatch(file,path,(unsigned)kstrlen(path));
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
        int found=0;TermTaskInfo info;
        for(int slot=0;slot<PROCESS_TASKS;slot++)if(term_task_running(slot)){
            print_number((term_task_info(slot,&info)&&info.state==PROCESS_TASK_SLEEPING)?"Sleeping, window slot ":"Running, window slot ",(unsigned)slot+1);found=1;
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
        const unsigned char *image=(const unsigned char *)fs_data(id);
        if(!kstrcmp(cmd,"exec")&&fs_size(id)>=4&&image[0]=='B'&&image[1]=='E'&&image[2]=='X'&&image[3]=='2'){
            push("Legacy exec supports BEX1 only; use start for this program.");return -1;
        }
        ProgramIO io={push,plot,program_key,program_present,canvas_resize,canvas_rect};app_view_canvas_unbuffered(selected);canvas_reset();
        if(program_input_hook)program_input_hook(1);
        int rc=!kstrcmp(cmd,"basic")?basic_run(fs_data(id),fs_size(id),&io):process_run(fs_data(id),fs_size(id),&io);
        if(program_input_hook)program_input_hook(0);
        if(rc){push("Program stopped (error, fault, or execution limit).");return -1;}push("Program finished.");
    }else return -1;return 0;
}
void term_enter(void){
    T.scroll=0;
    if(T.input_invalid){push("Input was lost; command not run. Clear the line with Backspace.");return;}
    char line[81];kstrcpy(line,T.input);push(line);
    int overflow=T.input_overflow;T.input_overflow=0;
    if(T.len&&!overflow){if(T.hcount==16){for(int i=1;i<16;i++)kstrcpy(T.history[i-1],T.history[i]);T.hcount--;}kstrcpy(T.history[T.hcount++],line);}
    T.hpos=T.hcount;T.len=0;T.input[0]=0;T.draft[0]=0;int budget=256;
    if(overflow){push("Command exceeds 80 characters; nothing was run. Use cd for shorter paths.");return;}
    int result=execute(line,0,&budget);
    if(result==FS_ERR_BUSY)push("Disk is saving; retry shortly.");
    else if(result&&result!=TERM_COMMAND_REPORTED)push("Error: check command, path, syntax, or available space.");
}
