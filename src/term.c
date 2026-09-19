#include "fs.h"
#include "term.h"
#include "program.h"
#include "layout.h"
typedef struct {
    char lines[TERM_LINES][TERM_COLS+1], input[TERM_COLS+1];
    char history[16][TERM_COLS+1], draft[TERM_COLS+1];
    int head,count,len,cwd,hcount,hpos,canvas_on,scroll,rows,view_count;
    unsigned char canvas[160*100];
} Terminal;
#ifndef TERM_MEMORY
#define TERM_MEMORY (APPS_BASE+0x300000)
#endif
static Terminal *terms=(Terminal *)TERM_MEMORY;
static int selected;
_Static_assert(sizeof(Terminal)*8<=0xC0000,"terminal arena overflow");
#define T (terms[selected])
void term_select(int slot){if(slot>=0&&slot<8)selected=slot;}
static void push(const char *s){int slot=(T.head+T.count)%TERM_LINES;if(T.count<TERM_LINES)T.count++;else T.head=(T.head+1)%TERM_LINES;int i=0;while(s[i]&&i<TERM_COLS){T.lines[slot][i]=s[i];i++;}T.lines[slot][i]=0;}
void term_reset(void){kmemset(&T,0,sizeof T);T.cwd=fs_root();push("Type help for commands; man NAME for examples.");push("Page Up / Page Down scroll through output.");}
int term_count(void){return T.count;}
const char *term_get(int i){return i>=0&&i<T.count?T.lines[(T.head+i)%TERM_LINES]:"";}
const char *term_input(void){return T.input;}
int term_cwd(void){if(!fs_is_dir(T.cwd))T.cwd=fs_root();return T.cwd;}
void term_set_cwd(int id){if(fs_is_dir(id))T.cwd=id;}
const unsigned char *term_canvas(void){return T.canvas_on?T.canvas:0;}
void term_prompt(char *out,int max){char path[FS_PATH_LEN];fs_path(term_cwd(),path,sizeof path);int p=0;for(int i=0;path[i]&&p<max-3;i++)out[p++]=path[i];if(max>2){out[p++]='>';out[p++]=' ';out[p]=0;}}
void term_char(char c){T.scroll=0;if(c>=32&&c<=126&&T.len<TERM_COLS){T.input[T.len++]=c;T.input[T.len]=0;}}
void term_backspace(void){T.scroll=0;if(T.len)T.input[--T.len]=0;}
void term_history(int direction){T.scroll=0;if(!T.hcount)return;if(T.hpos==T.hcount)kstrcpy(T.draft,T.input);int p=T.hpos+direction;if(p<0)p=0;if(p>T.hcount)p=T.hcount;T.hpos=p;kstrcpy(T.input,p==T.hcount?T.draft:T.history[p]);T.len=kstrlen(T.input);}
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
    {"rm","rm PATH","Permanently delete a file or a folder and its contents.","rm /Projects/old.txt","This bypasses Trash. There is no undo."},
    {"echo","echo TEXT","Print the remaining text on a new line.","echo Hello world","There are no variables, pipes, or output redirection."},
    {"clear","clear","Clear this terminal's output and graphics canvas.","clear","Command history and the current folder are kept."},
    {"stat","stat PATH","Show type, byte size, and modification timestamp.","stat /readme.txt","Time is UTC seconds since 2000; zero means unknown."},
    {"df","df","Show used bytes, payload capacity, free slots, and disk status.","df","Folders and session files also consume the 64 volume slots."},
    {"run","run SCRIPT","Run one terminal command per script line.","run /Programs/demo.sh","Stops on errors; four nested scripts, 256 command dispatches."},
    {"basic","basic FILE","Run numbered, uppercase Tiny BASIC statements.","basic /Programs/demo.bas","PRINT, LET, IF/THEN, GOTO, INKEY, WAIT, PLOT, RECT, REM, END."},
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
static void cat(int id){char row[81];int n=0;for(int i=0;i<fs_size(id);i++){char c=fs_data(id)[i];if(c=='\r')continue;if(c=='\n'){row[n]=0;push(row);n=0;continue;}row[n++]=c>=32&&c<=126?c:'.';if(n==80){row[n]=0;push(row);n=0;}}if(n){row[n]=0;push(row);}}
static void plot(int x,int y,int color){if(x>=0&&x<160&&y>=0&&y<100){T.canvas_on=1;T.canvas[y*160+x]=(unsigned char)color;}}
static int execute(const char *,int,int *);
static int script(int id,int depth,int *budget){
    if(depth>=4||!fs_valid(id)||fs_is_dir(id))return -1;
    /* Copy each script so deleting its source cannot change the running program. */
    char *source=(char *)(TERM_MEMORY+0xC0000+depth*FS_MAX_SIZE);int size=fs_size(id);kmemcpy(source,fs_data(id),size);int pos=0;
    while(pos<size){char line[81];int n=0;while(pos<size&&source[pos]!='\n'){char c=source[pos++];if(c=='\r')continue;if(n==80)return -1;line[n++]=c;}pos++;line[n]=0;if(execute(line,depth+1,budget))return -1;}return 0;
}
static int execute(const char *s,int depth,int *budget){
    if(--*budget<0)return -1;
    while(*s==' ')s++;
    if(!*s||*s=='#')return 0;
    char cmd[81];int n=0;while(*s&&*s!=' '&&n<80)cmd[n++]=*s++;cmd[n]=0;while(*s==' ')s++;
    char arg[81];int a=0;const char *p=s;char quote=*p=='"'?*p++:0;
    while(*p&&(quote?*p!=quote:*p!=' ')&&a<80)arg[a++]=*p++;
    arg[a]=0;
    if(quote&&*p!='"')return -1;
    int cwd=term_cwd(),id=arg[0]?fs_resolve(cwd,arg):cwd;
    if(!kstrcmp(cmd,"help")||!kstrcmp(cmd,"man"))return manual(arg);
    else if(!kstrcmp(cmd,"echo"))push(s);
    else if(!kstrcmp(cmd,"clear")){T.head=T.count=T.canvas_on=0;}
    else if(!kstrcmp(cmd,"pwd")){char path[64];fs_path(cwd,path,sizeof path);push(path);}
    else if(!kstrcmp(cmd,"cd")){if(!fs_is_dir(id))return -1;T.cwd=id;}
    else if(!kstrcmp(cmd,"ls")){if(!fs_is_dir(id))return -1;int ids[64],count=fs_list(id,ids,64);for(int i=0;i<count;i++){char row[26];kstrcpy(row,fs_name(ids[i]));if(fs_is_dir(ids[i]))kstrcpy(row+kstrlen(row),"/");push(row);}}
    else if(!kstrcmp(cmd,"cat")){if(!arg[0]||!fs_valid(id)||fs_is_dir(id))return -1;cat(id);}
    else if(!kstrcmp(cmd,"mkdir")||!kstrcmp(cmd,"touch")){char name[FS_NAME_LEN];int parent=fs_destination(cwd,arg,name);if(parent<0)return -1;if((!kstrcmp(cmd,"mkdir")?fs_mkdir(parent,name):fs_create(parent,name))<0)return -1;}
    else if(!kstrcmp(cmd,"rm")){if(!arg[0]||fs_delete(id)<0)return -1;}
    else if(!kstrcmp(cmd,"stat")){if(!fs_valid(id))return -1;push(fs_name(id));push(fs_is_dir(id)?"Directory":"File");print_number("Bytes: ",fs_size(id));print_number("Modified (UTC seconds since 2000; 0=unknown): ",fs_modified(id));}
    else if(!kstrcmp(cmd,"df")){print_number("Bytes used: ",fs_used_bytes());print_number("Payload capacity: ",fs_capacity());print_number("Free node slots: ",FS_MAX_NODES-fs_node_count());push("Each file: at most 16383 bytes. Folders also use slots.");push(fs_storage_status()?fs_storage_status():"Disk is synchronized.");}
    else if(!kstrcmp(cmd,"run"))return script(id,depth,budget);
    else if(!kstrcmp(cmd,"basic")||!kstrcmp(cmd,"exec")){
        if(!arg[0]||!fs_valid(id)||fs_is_dir(id))return -1;
        ProgramIO io={push,plot,program_key,program_present};kmemset(T.canvas,0,sizeof T.canvas);T.canvas_on=0;
        int rc=!kstrcmp(cmd,"basic")?basic_run(fs_data(id),fs_size(id),&io):process_run(fs_data(id),fs_size(id),&io);
        if(rc){push("Program stopped (error, fault, or execution limit).");return -1;}push("Program finished.");
    }else return -1;return 0;
}
void term_enter(void){
    T.scroll=0;
    char line[81];kstrcpy(line,T.input);push(line);
    if(T.len){if(T.hcount==16){for(int i=1;i<16;i++)kstrcpy(T.history[i-1],T.history[i]);T.hcount--;}kstrcpy(T.history[T.hcount++],line);}
    T.hpos=T.hcount;T.len=0;T.input[0]=0;T.draft[0]=0;int budget=256;
    if(execute(line,0,&budget))push("Error: check command, path, syntax, or available space.");
}
