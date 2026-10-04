/* Test real Files functions with ordinary valid-volume workflows. */
#define main file_view_model_main
#include "file_view_host.c"
#undef main
#include "history.h"
#include "app.h"
#define EDIT_BUF_SIZE 65536
#define MAX_WIN 8
#define TITLE_H 32
#define INFO_H 28
#define ROW_H 28
#define SB 16
#define ICON_TRASH 1
#include "files_kernel_types.inc"
static WindowState states[MAX_WIN],*window_state=states;
static int context_slot,dirty,trash_id=-1,prefs_id=-1;
static int fm_renaming,fm_rename_id=-1,fm_rename_len;
static unsigned fm_rename_identity;
static char fm_rename_buf[FS_NAME_LEN];
static int fm_dragging,fm_drag_active,fm_drag_id=-1,fm_drag_parent;
static int fm_drag_sx,fm_drag_sy;
static unsigned fm_drag_identity;
static int key_sc,ctrl_down,alt_down;
static char key_char;
static uint32_t frame_count,files_message_until;
static int files_action_busy;
static int open_dlg,name_dlg,clip_len,mouse_x,mouse_y;
static unsigned clip_generation;
static char clip_buf[1024];
static int opened=-1;
enum { WK_FILES=4 };
static struct { int open,kind; } wins[MAX_WIN];
static void context_set(int slot) {context_slot=slot;}
static int front_kind(void) {return wins[context_slot].kind;}
static int find_open_kind(int kind) {
    for(int i=0;i<MAX_WIN;i++)if(wins[i].open&&wins[i].kind==kind)return i;
    return -1;
}
static void open_fs_file(int id) {opened=id;}
static void od_refresh(void) {}
static int icon_hit(int icon,int x,int y) {(void)icon;(void)x;(void)y;return 1;}
static void kprint_debug(const char *text) {(void)text;}
int hit(int x,int y,int bx,int by,int w,int h) {return x>=bx&&x<bx+w&&y>=by&&y<by+h;}
static void fm_go_up(void);
#include "files_kernel_ops.inc"

static void desktop_reset(void) {
    reset_volume(1);memset(states,0,sizeof(states));memset(wins,0,sizeof(wins));
    context_slot=0;wins[0].open=1;wins[0].kind=WK_FILES;trash_id=fs_mkdir(0,"trash");
    prefs_id=fs_mkdir(0,"prefs");fm_set_cwd(0);fm_refresh();
    fm_renaming=0;fm_rename_id=-1;fm_dragging=fm_drag_active=0;fm_drag_id=-1;
    key_sc=ctrl_down=alt_down=key_char=0;frame_count=100;files_message_until=0;
    clip_len=clip_generation=0;open_dlg=name_dlg=0;opened=-1;
}
static void folder_open(int id) {fm_set_cwd(id);fm_refresh();}
static void selected(int id) {fm_select_id(id);assert(fm_row_id(fm_selected)==id);}
static void filter_text(const char *text) {
    fm_filter_show();
    for(int i=0;text[i];i++){key_sc=0;key_char=text[i];assert(fm_filter_key());}
    key_char=0;
}
static void input(int sc,int control) {
    key_sc=sc;key_char=0;ctrl_down=control;fm_filter_key();ctrl_down=0;
}
static void selection_and_filter(void) {
    desktop_reset();int folder=fs_mkdir(0,"Folder");
    int z=make_file(folder,"zulu.txt",32),a=make_file(folder,"Alpha.txt",8);
    int b=make_file(folder,"beta.bin",1);folder_open(folder);selected(z);
    for(int i=0;i<FILE_VIEW_SORT_COUNT;i++){
        fm_sort_by(i);assert(fm_row_id(fm_selected)==z);
        fm_sort_by(i);assert(fm_row_id(fm_selected)==z);
    }
    filter_text("TXT");assert(fm_count==2&&fm_total==3&&fm_row_id(fm_selected)==z);
    filter_text("alpha");assert(fm_count==1&&fm_selected==-1&&fm_row_id(fm_selected)==-2);
    /* No implicit replacement selection can be copied, renamed or opened. */
    files_clipboard_action(1);assert(file_clipboard_mode()==FILE_CLIPBOARD_NONE);
    fm_open_selected();assert(opened==-1);
    fm_rename_begin(a);assert(!fm_renaming);
    fm_filter_clear(0);assert(fm_count==3&&fm_selected==-1&&fm_filter_open);
    selected(a);filter_text("no match");assert(fm_count==0&&fm_vis_count()==1&&fm_selected==-1);
    input(KEY_ESC,0);assert(!fm_filter_open&&fm_count==3&&fm_selected==-1);
    selected(b);filter_text("beta");input(KEY_ENTER,0);
    assert(fm_filter_open&&!fm_filter_focus&&fm_row_id(fm_selected)==b);
    fm_open_selected();assert(opened==b);
    input(KEY_ESC,0);assert(!fm_filter_open&&fm_count==3);
    filter_text("123456789012345678901234567890");assert(strlen(fm_view.filter)==23);
    input(0x1e,1);input(KEY_BACKSPACE,0);assert(!fm_view.filter[0]&&fm_count==3);
    filter_text("a");input(KEY_BACKSPACE,0);assert(!fm_view.filter[0]);
    input(KEY_ESC,0);assert(fm_first==0);
    filter_text("no match");fm_selected=0;fm_open_selected();
    assert(fm_cwd==0&&!fm_filter_open&&!fm_view.filter[0]);
    assert(fm_count==1&&fm_ids[0]==folder); /* System folders remain hidden. */
    assert(fs_rename(trash_id,"Renamed trash")==0);fm_refresh();
    assert(fm_count==1&&fm_total==1);
}
static void stale_selections_and_rename(void) {
    desktop_reset();int folder=fs_mkdir(0,"Folder");
    int a=make_file(folder,"a.txt",8),b=make_file(folder,"b.txt",16);
    folder_open(folder);selected(a);unsigned identity=fs_identity(a);
    assert(!fs_delete(a));int replacement=make_file(folder,"reused.txt",32);assert(replacement==a);
    assert(fs_identity(replacement)!=identity&&fm_row_id(fm_selected)==-2);
    files_clipboard_action(1);assert(file_clipboard_mode()==FILE_CLIPBOARD_NONE);
    do_duplicate();assert(fs_child_count(folder)==2);
    fm_refresh();assert(fm_selected==-1);
    selected(b);fm_rename_begin(b);assert(fm_renaming);
    assert(!fs_delete(b));int reused=make_file(folder,"other.txt",2);assert(reused==b);
    strcpy(fm_rename_buf,"do-not-rename.txt");fm_rename_len=strlen(fm_rename_buf);
    fm_rename_commit();assert(!fm_renaming&&!strcmp(fs_name(reused),"other.txt"));
    fm_refresh();selected(reused);filter_text("other");input(KEY_ENTER,0);fm_rename_begin(reused);
    strcpy(fm_rename_buf,"renamed.txt");fm_rename_len=strlen(fm_rename_buf);fm_rename_commit();
    assert(!strcmp(fs_name(reused),"renamed.txt")&&!fm_view.filter[0]&&fm_row_id(fm_selected)==reused);
    /* Deleting/reusing a drag source cannot redirect a Trash drop. */
    fm_drag_id=reused;fm_drag_identity=fs_identity(reused);fm_drag_parent=folder;fm_drag_active=1;
    assert(!fs_delete(reused));int next=make_file(folder,"leave-me.txt",1);assert(next==reused);
    files_drop();assert(fs_parent(next)==folder&&fs_child_count(trash_id)==0);
    /* A dead folder cannot turn a pending Paste into a write to its reused ID. */
    selected(replacement);files_clipboard_action(1);assert(file_clipboard_mode()==FILE_CLIPBOARD_COPY);
    int oldfolder=fs_mkdir(0,"Old");folder_open(oldfolder);assert(!fs_delete(oldfolder));
    int newfolder=fs_mkdir(0,"New");assert(newfolder==oldfolder&&!fm_cwd_valid());
    files_clipboard_action(2);assert(fs_child_count(newfolder)==0&&fm_cwd==0);
}
static void paste_sort_and_scroll(void) {
    desktop_reset();int source=fs_mkdir(0,"Source"),target=fs_mkdir(0,"Target");
    int original=make_file(source,"note.txt",17);folder_open(source);selected(original);
    files_clipboard_action(1);folder_open(target);filter_text("different");input(KEY_ENTER,0);
    files_clipboard_action(2);int result=fm_row_id(fm_selected);
    assert(result>=0&&result!=original&&fs_parent(result)==target&&!fm_view.filter[0]);
    assert(fs_size(result)==17&&fs_child_count(target)==1);
    /* Another window refresh follows its existing exact selection. */
    wins[1].open=1;wins[1].kind=WK_FILES;context_set(1);folder_open(target);selected(result);
    context_set(0);files_clipboard_action(2);assert(fs_child_count(target)==2);
    int second=fm_row_id(fm_selected);assert(second!=result);
    context_set(1);assert(fm_row_id(fm_selected)==result);context_set(0);
    selected(second);files_clipboard_action(0);folder_open(source);files_clipboard_action(2);
    assert(fm_row_id(fm_selected)==second&&fs_parent(second)==source);
    for(int i=0;i<40;i++){char name[24];snprintf(name,sizeof(name),"item %02d.txt",i);make_file(source,name,i);}
    fm_refresh();selected(original);fm_manual_scroll=1;fm_first=1000;files_scroll(4);
    assert(fm_first==fm_vis_count()-4);filter_text("item 01");files_scroll(4);
    assert(fm_first==0&&fm_count==1&&fm_selected==-1);
    fm_filter_clear(1);fm_first=1000;fm_manual_scroll=1;files_scroll(1000);assert(fm_first==0);
    assert(files_rows(50,200)==3);fm_filter_show();assert(files_rows(50,200)==1);
    assert(files_list_y(50)==178);fm_filter_clear(1);
    /* Actual click hit-testing shares the same compact geometry. */
    mouse_x=8+60+3+5;mouse_y=TITLE_H+INFO_H+8;
    handle_files_click(0,0,360,200);assert(fm_view.sort==FILE_VIEW_TYPE);
    mouse_x=360-SB-8-31;handle_files_click(0,0,360,200);assert(fm_filter_open);
    filter_text("note");mouse_x=360-SB-8-90;mouse_y=files_filter_y(0)+12;
    handle_files_click(0,0,360,200);assert(!fm_view.filter[0]&&fm_filter_open);
    mouse_x=360-SB-8-28;handle_files_click(0,0,360,200);assert(!fm_filter_open);
}
int main(void) {
    selection_and_filter();stale_selections_and_rename();paste_sort_and_scroll();
    puts("Files kernel: selection identity, filtering, input, rename, paste, stale drag/folder and scrolling passed");
    return 0;
}
