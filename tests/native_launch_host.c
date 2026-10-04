/* Exact desktop dispatch with real Terminal/FS; process execution is guest-tested. */
#define TERM_TASK_FIXTURE_ONLY
#include "term_task_info_host.c"
#include "file_view.h"
#define MAX_WIN 8
#define EDIT_BUF_SIZE 65536
#define ICON_TRASH 2
static const struct {const char *label;} icons[ICON_TRASH]={{"Terminal"},{"Files"}};
enum { WK_NONE=-1, WK_TERM, WK_EDIT, WK_CLOCK, WK_CAL, WK_MINES, WK_2048,
       WK_BREAKOUT, WK_SYSMON, WK_HELLO, WK_PROPERTIES, WK_WRITER, WK_SPREADSHEET,
       WK_VIEW, WK_BROWSER, WK_PLAYER };
static struct {int open,kind,seq;} wins[MAX_WIN];
static int context_slot,open_dlg,dirty,properties_id,launch_n,launch_len,launch_sel,launcher_on;
static int fm_cwd,fm_count,fm_total,fm_selected,fm_ids[FS_MAX_NODES];
static FileViewOptions fm_view;
static unsigned fm_ids_identity[FS_MAX_NODES];
static int pick_count,pick_selected,pick_cwd,pick_writer,pick_sheet,pick_pics_only;
static int pick_owner=-1,pick_owner_seq,pick_ids[FS_MAX_NODES],trash_id=-1,prefs_id=-1;
static unsigned pick_identity[FS_MAX_NODES];
static char launch_buf[24];
static const char *native_launch_status="",*properties_reason;
static int edits,other_opens,icon_opens,document_requests;
static int edit_close_owner=-1,name_dlg,dragging_win=-1,drag_active;
static int win_front(void){for(int i=MAX_WIN-1;i>=0;i--)if(wins[i].open)return i;return -1;}
static void context_set(int slot){if(slot>=0){context_slot=slot;term_select(slot);}}
static int win_open(int kind){
    for(int i=0;i<MAX_WIN;i++)if(!wins[i].open){
        wins[i].open=1;wins[i].kind=kind;wins[i].seq++;
        context_slot=i;term_select(i);if(kind==WK_TERM)term_reset();return i;
    }return -1;
}
static void fm_set_cwd(int id){fm_cwd=id;}
static int fm_cwd_valid(void){return fs_is_dir(fm_cwd);}
static void fm_checked_cwd(void){if(!fs_is_dir(fm_cwd))fm_cwd=fs_root();}
static int fm_has_parent(void){return fs_parent(fm_cwd)>=0;}
static void fm_go_up(void){fm_cwd=fs_parent(fm_cwd);}
static void document_request(int owner,int action,int file){(void)owner;(void)action;(void)file;document_requests++;}
#define DOCUMENT_OPEN 0
static void edit_load(int id){(void)id;edits++;}
static int is_image_file(int id){(void)id;return 0;}
#define STUB_OPEN(name) static void name(void){other_opens++;}
#define STUB_FILE(name) static void name(int id){(void)id;other_opens++;}
STUB_FILE(open_browser) STUB_FILE(open_player) STUB_FILE(open_writer)
STUB_FILE(open_spreadsheet) STUB_FILE(open_view) STUB_FILE(start_open_dialog)
STUB_OPEN(open_calc) STUB_OPEN(open_paint) STUB_OPEN(open_snake)
STUB_OPEN(open_wordle) STUB_OPEN(open_term) STUB_OPEN(open_todo) STUB_OPEN(cal_reset)
STUB_OPEN(edit_close_cancel) STUB_OPEN(image_viewer_close) STUB_OPEN(browser_close)
STUB_OPEN(player_close) STUB_OPEN(writer_close) STUB_OPEN(spreadsheet_close)
static void icon_open(int icon){(void)icon;icon_opens++;}
#include "native_launch_types.inc"
static LaunchItem launch_items[24];
#include "native_launch_ops.inc"
static int windows(void){int count=0;for(int i=0;i<MAX_WIN;i++)count+=wins[i].open;return count;}
static void close_all(void){
    for(int i=0;i<MAX_WIN;i++){term_task_close(i);wins[i].open=0;}
    context_slot=0;term_select(0);term_reset();
}
static void query(const char *name){kstrcpy(launch_buf,name);launch_len=kstrlen(name);launch_sel=0;launcher_on=1;launcher_refresh();}
static void select_file(int id){
    fm_cwd=fs_parent(id);fm_refresh();
    for(int i=0;i<fm_count;i++)if(fm_ids[i]==id){fm_selected=i+fm_has_parent();return;}
    assert(!"file not listed");
}
int main(void){
    reset();int first=executable("native one.bex"),upper=executable("COUNTER.BEX");
    unsigned identity=fs_identity(first);TermTaskInfo before,after;
    /* Explicit owner API is independent of whichever Terminal is selected. */
    term_select(2);term_reset();term_char('x');
    assert(!term_task_start_file(0,first,identity)&&selected==2&&!strcmp(term_input(),"x"));
    assert(term_task_info(0,&before)&&!strcmp(before.name,"native one.bex"));
    term_select(0);canvas_resize(320,200);plot(1,1,6);callbacks[0].present();term_char('z');
    assert(term_task_start_file(0,upper,fs_identity(upper))<0);
    assert(term_canvas_width()==320&&term_canvas()[321]==6&&!strcmp(term_input(),"z"));
    assert(term_task_info(0,&after)&&before.instance==after.instance);
    assert(term_task_start_file(-1,first,identity)<0&&term_task_start_file(8,first,identity)<0);
    term_task_close(0);assert(term_task_start_file(0,first,identity+1)<0&&!term_task_running(0));
    assert(term_task_start_file(0,fs_root(),fs_identity(fs_root()))<0);
    int shortcut=fs_create_app(fs_root(),"shortcut.bex");
    assert(term_task_start_file(0,shortcut,fs_identity(shortcut))<0);
    close_all();
    /* Files and launcher share the native route, including names with spaces. */
    select_file(first);fm_open_selected();
    assert(windows()==1&&wins[0].kind==WK_TERM&&term_task_info(0,&before));
    assert(term_cwd()==fs_parent(first)&&!edits);
    query("COUNTER.BEX");assert(launch_n==1);launcher_run(0);
    assert(!launcher_on&&windows()==2&&term_task_info(1,&after));
    assert(before.instance!=after.instance&&!strcmp(after.name,"COUNTER.BEX")&&!edits);
    open_fs_file(first);assert(windows()==3&&term_task_running(2));
    for(int i=3;i<8;i++)open_fs_file(upper);
    assert(windows()==8);assert(term_task_info(0,&before));open_fs_file(first);
    assert(windows()==8&&strstr(native_launch_status,"Close a window"));
    assert(term_task_info(0,&after)&&before.instance==after.instance);
    win_close(-1);assert(native_launch_status[0]);
    win_close(3);assert(!native_launch_status[0]&&!term_task_running(3));open_fs_file(first);
    assert(windows()==8&&term_task_running(3)&&!native_launch_status[0]);
    close_all();
    /* Cached results own their labels and reject a deleted/reused node. */
    query("native one.bex");assert(launch_n==1);
    assert(!fs_rename(first,"native new.bex"));
    assert(!strcmp(launch_items[0].name,"native one.bex"));
    launcher_run(0);assert(!windows()&&launcher_on&&!launch_n);
    assert(!fs_rename(first,"native one.bex"));query("native one.bex");
    assert(!fs_delete(first));int replacement=executable("native one.bex");assert(replacement==first);
    launcher_run(0);assert(!windows()&&launcher_on&&launch_n==1);
    launcher_run(0);assert(windows()==1&&term_task_running(0));close_all();
    /* A cached Files row and Open dialog likewise cannot activate a reused id. */
    select_file(replacement);assert(!fs_delete(replacement));first=executable("native one.bex");
    assert(first==replacement);fm_open_selected();assert(!windows()&&fm_selected==-1);
    fm_open_selected();assert(!windows());select_file(first);fm_open_selected();assert(windows()==1);
    close_all();pick_cwd=fs_root();od_refresh();
    for(int i=0;i<pick_count;i++)if(pick_ids[i]==first)pick_selected=i;
    assert(!fs_delete(first));replacement=executable("native one.bex");assert(replacement==first);
    open_dlg=1;od_open_selected();assert(open_dlg&&!windows());od_open_selected();
    assert(!open_dlg&&windows()==1&&term_task_running(0));close_all();
    /* App shortcuts and ordinary documents retain their existing routes. */
    int plain=fs_create(fs_root(),"note.txt");assert(plain>=0);open_fs_file(plain);
    assert(edits==1&&!term_task_running(context_slot));close_all();
    query("Terminal");assert(launch_n==1);launcher_run(0);assert(icon_opens==1);
    open_fs_file(shortcut);assert(!windows());
    puts("native launch: Files/Open/launcher dispatch, fresh owners, spaces/case, copied names, reused nodes, eight-window capacity and shared start lifecycle passed");
}
