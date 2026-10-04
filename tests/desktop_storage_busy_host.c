/* Ordinary UI write/retry paths against the real incremental filesystem. */
#define main filesystem_fixture_main
#include "fs_host.c"
#undef main
#include "file_view.h"
static int dirty,files_action_busy,fm_cwd,fm_selected;
static unsigned frame_count,files_message_until;
static int fm_renaming,fm_rename_id,fm_rename_len,open_dlg;
static unsigned fm_rename_identity;
static char fm_rename_buf[FS_NAME_LEN];
static const char *name_failure_message;
enum { WK_FILES=4 };
static int front_kind(void){return WK_FILES;}
static int fm_cwd_valid(void){return fs_is_dir(fm_cwd);}
static void fm_refresh(void){}
static void fm_select_id(int id){fm_selected=id;}
static void fm_rename_cancel(void){fm_renaming=0;}
static int find_open_kind(int kind){(void)kind;return -1;}
#define PAINT_W 160
#define PAINT_H 100
static unsigned char paint_pix[PAINT_W*PAINT_H],paint_memory[0xC000+8+PAINT_W*PAINT_H];
#undef PAINT_MEM
#define PAINT_MEM ((uintptr_t)paint_memory)
static void paint_init(void){}
#include "desktop_storage_ops.inc"
int main(void){
    reset();memset(data_disk,0,sizeof data_disk);unsigned *m=(unsigned *)data_disk;
    m[0]=DATA_MARKER_MAGIC;m[1]=DATA_MARKER_VERSION;m[2]=DATA_DISK_SECTORS;
    m[3]=DATA_SLOT_SECTORS;m[4]=DATA_FIRST_LBA;m[5]=DATA_SECOND_LBA;m[6]=crc32(m,24);
    data_present=1;assert(!fs_init()&&fs_load_disk()==FS_LOAD_BLANK);assert(!fs_empty_dir(0));
    int id=fs_create(0,"before.txt");assert(id>0&&fs_write(id,"document",8)==8);
    fm_renaming=1;fm_rename_id=id;fm_rename_identity=fs_identity(id);
    strcpy(fm_rename_buf,"after.txt");fm_rename_len=9;
    FsSyncTicket ticket;assert(!fs_sync_request(&ticket)&&fs_sync_busy());
    int count=fs_node_count();frame_count=20;
    fm_new_file();assert(files_action_busy&&dirty&&files_message_until==20+8*TIMER_HZ);
    fm_rename_commit();assert(fm_renaming&&fm_rename_id==id&&!strcmp(fm_rename_buf,"after.txt"));
    do_new_folder();assert(fm_renaming&&fs_node_count()==count);
    assert(!paint_write_named("drawing.pbm")&&strstr(name_failure_message,"Retry Save"));
    assert(fs_node_count()==count&&fs_find_child(0,"Pictures")<0&&!strcmp(fs_name(id),"before.txt"));
    unsigned steps=0;while(fs_sync_busy()){assert(fs_sync_step()!=FS_SYNC_IDLE);assert(++steps<100000);}
    assert(!fs_sync_result(ticket)&&!fs_sync_release(ticket));
    fm_rename_commit();assert(!fm_renaming&&!files_action_busy&&!strcmp(fs_name(id),"after.txt"));
    fm_new_file();assert(fs_node_count()==count+1&&fm_selected>=0);
    do_new_folder();assert(fs_node_count()==count+2&&fs_is_dir(fm_selected));
    memset(paint_pix,7,sizeof paint_pix);assert(paint_write_named("drawing.pbm")&&!name_failure_message);
    assert(!fs_sync());remount();id=fs_resolve(0,"/Pictures/drawing.pbm");
    assert(id>0&&fs_size(id)==8+PAINT_W*PAINT_H&&!memcmp(fs_data(id),"BOS1",4));
    assert(fs_resolve(0,"/after.txt")>0);
    puts("Files and Paint retain names and pixels during a snapshot, then retry durably");return 0;
}
