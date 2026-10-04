/* Functional tests only: no guest execution, memory-fault probes or fuzzing. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "platform.h"
#include "history.h"
#include "writer.h"
#include "sheet.h"
#include "file_view.h"
static unsigned char node_arena[FS_CAPACITY], image_arena[FS_IMG_CAPACITY], pool_arena[FS_POOL_CAPACITY];
#undef FS_BASE
#undef FS_IMG_BASE
#undef FS_POOL_BASE
#define FS_BASE ((uintptr_t)node_arena)
#define FS_IMG_BASE ((uintptr_t)image_arena)
#define FS_POOL_BASE ((uintptr_t)pool_arena)
#include "../src/fs.c"
static unsigned char floppy[DISK_SECTORS * SECTOR_SIZE], ide[DATA_DISK_SECTORS * SECTOR_SIZE];
static int ide_present;
void platform_poll(void) {}
int platform_memory_range_available(uint32_t base, uint32_t end) { (void)base; (void)end; return 0; }
void platform_log(const char *text) { (void)text; }
uint32_t timer_ticks(void) { return 0; }
unsigned disk_sector_count(void) { return DISK_SECTORS; }
int disk_read(unsigned lba, void *buf, int count) {
    assert(count >= 0 && lba + count <= DISK_SECTORS);
    memcpy(buf, floppy + lba * SECTOR_SIZE, count * SECTOR_SIZE); return 0;
}
int disk_write(unsigned lba, const void *buf, int count) {
    assert(count >= 0 && lba + count <= DISK_SECTORS);
    memcpy(floppy + lba * SECTOR_SIZE, buf, count * SECTOR_SIZE); return 0;
}
int ata_probe(void) { return ide_present; }
unsigned ata_sector_count(void) { return DATA_DISK_SECTORS; }
int ata_read(unsigned lba, void *buf, int count) {
    assert(count >= 0 && lba + count <= DATA_DISK_SECTORS);
    memcpy(buf, ide + lba * SECTOR_SIZE, count * SECTOR_SIZE); return 0;
}
int ata_write(unsigned lba, const void *buf, int count) {
    assert(count >= 0 && lba + count <= DATA_DISK_SECTORS);
    memcpy(ide + lba * SECTOR_SIZE, buf, count * SECTOR_SIZE); return 0;
}
int ata_flush(void) { return 0; }
#define MAX_WIN 8
#define EDIT_BUF_SIZE 65536
#define PAINT_W 160
#define PAINT_H 100
#include "editor_kernel_types.inc"
static WindowState states[MAX_WIN], *window_state=states;
static int context_slot, dirty, name_dlg, name_failed;
static char edit_scratch[EDIT_BUF_SIZE];
static const char *name_failure_message, *edit_conflict_message="Source changed. Save with a new name.";
static const char *session_status="";
static int paint_ready;
static unsigned char paint_pix[PAINT_W*PAINT_H];
enum { WK_NONE=-1, WK_FILES=4, WK_EDIT=5, WK_TERM=11, WK_PROPERTIES=19, WK_WRITER=22, WK_SPREADSHEET=23 };
typedef struct { int open,kind,x,y,w,h,min,z; } Win;
static Win wins[MAX_WIN];
static int wm_z;
static int find_open_kind(int kind) { for(int i=0;i<MAX_WIN;i++)if(wins[i].open&&wins[i].kind==kind)return i;return -1; }
static void context_set(int slot) { if(slot>=0&&slot<MAX_WIN)context_slot=slot; }
static int win_front(void) { return context_slot; }
static void fm_refresh(void) {}
static void namedlg_open(int target,const char *name) { (void)target;(void)name;name_dlg=1;name_failed=0;name_failure_message=0; }
#include "editor_kernel_ops.inc"
static int win_open(int kind) {
    for(int i=0;i<MAX_WIN;i++)if(!wins[i].open){
        memset(&states[i],0,sizeof states[i]);context_set(i);fm_set_cwd(fs_root());
        states[i].history=(History){0,0,8,sizeof(Document),(unsigned char *)states[i].undo};
        edit_clear();
#ifdef SPREADSHEET_RECOVERY_TEST
        if(kind==WK_SPREADSHEET)spreadsheet_init();
#endif
        wins[i]=(Win){.open=1,.kind=kind,.w=600,.h=400,.z=i};return i;
    }return -1;
}
static void win_clamp(Win *win) { (void)win; }
static int term_cwd(void) { return 0; }
static void term_set_cwd(int id) { (void)id; }
static void paint_init(void) { paint_ready=1; }
int writer_file(void) { return -1; }
unsigned writer_file_identity(void) { return 0; }
unsigned writer_caret(void) { return 0; }
const unsigned char *writer_snapshot(unsigned *size) { (void)size;return 0; }
int writer_binding(WriterBinding *binding) { (void)binding;return 0; }
int writer_binding_matches(int file,const WriterBinding *binding) { (void)file;(void)binding;return 0; }
int writer_open_file(int file) { (void)file;return 0; }
int writer_restore(const unsigned char *data,unsigned size,int file,unsigned identity,int dirty,
                   unsigned caret,unsigned anchor) {
    (void)data;(void)size;(void)file;(void)identity;(void)dirty;(void)caret;(void)anchor;return 0;
}
#ifndef SPREADSHEET_RECOVERY_TEST
int spreadsheet_file(void) { return -1; }
unsigned spreadsheet_file_identity(void) { return 0; }
unsigned spreadsheet_caret(void) { return 0; }
unsigned spreadsheet_anchor(void) { return 0; }
const unsigned char *spreadsheet_snapshot(unsigned *size) { (void)size;return 0; }
int spreadsheet_binding(SpreadsheetBinding *binding) { (void)binding;return 0; }
int spreadsheet_binding_matches(int file,const SpreadsheetBinding *binding) { (void)file;(void)binding;return 0; }
int spreadsheet_open_file(int file) { (void)file;return 0; }
int spreadsheet_restore(const unsigned char *data,unsigned size,int file,unsigned identity,int dirty,
                   unsigned caret,unsigned anchor) {
    (void)data;(void)size;(void)file;(void)identity;(void)dirty;(void)caret;(void)anchor;return 0;
}
#endif
#include "editor_kernel_session.inc"
static void desktop_reset(void) {
    memset(states,0,sizeof states);memset(wins,0,sizeof wins);context_slot=wm_z=0;
    name_dlg=name_failed=paint_ready=0;session_status="";session_ready=1;
}
static void reset(int large) {
    memset(floppy,0,sizeof floppy);memset(ide,0,sizeof ide);ide_present=large;
    if(large){unsigned marker[]={DATA_MARKER_MAGIC,DATA_MARKER_VERSION,DATA_DISK_SECTORS,
        DATA_SLOT_SECTORS,DATA_FIRST_LBA,DATA_SECOND_LBA,0};marker[6]=crc32(marker,24);memcpy(ide,marker,sizeof marker);}
    fs_init();assert(fs_load_disk()==FS_LOAD_BLANK);fs_empty_dir(0);desktop_reset();
}
static void reboot(void) { assert(!fs_sync());fs_init();assert(!fs_load_disk());desktop_reset();session_restore(); }
static int make_file(int dir,const char *name,const char *text) {
    int id=fs_create(dir,name);assert(id>=0);assert(fs_write(id,text,(int)strlen(text))==(int)strlen(text));return id;
}
static int open_editor(int id) { int slot=win_open(WK_EDIT);assert(slot>=0);edit_load(id);fm_set_cwd(fs_parent(id));return slot; }
static void replace_text(const char *text) {
    edit_record();edit_len=(int)strlen(text);memcpy(edit_buf,text,edit_len+1);edit_caret=edit_len;edit_sel_collapse();edit_saved_ok=0;
}
static void expect_file(int id,const char *text) { assert(fs_size(id)==(int)strlen(text)&&!memcmp(fs_data(id),text,strlen(text))); }
static int pref(const char *name) { return fs_find_child(fs_find_child(0,"prefs"),name); }
static void live_workflows(void) {
    reset(1);int id=make_file(0,"report.txt","Original edition.");
    int one=open_editor(id),two=open_editor(id);replace_text("Second draft.");
    context_set(one);replace_text("First saved.");assert(edit_save());EditorSource baseline=edit_source;
    context_set(two);assert(!edit_save()&&name_dlg&&name_failed&&!edit_saved_ok);
    assert(!strcmp(edit_buf,"Second draft."));expect_file(id,"First saved.");
    assert(!edit_write_named("report.txt")&&!strcmp(name_failure_message,edit_conflict_message));
    expect_file(id,"First saved.");assert(edit_write_named("second.txt"));expect_file(edit_file,"Second draft.");
    context_set(one);edit_undo(0);assert(!strcmp(edit_buf,"Original edition."));
    assert(!memcmp(&baseline,&edit_source,sizeof baseline));assert(edit_save());expect_file(id,"Original edition.");
    assert(!fs_rename(id,"renamed.txt"));replace_text("After rename.");assert(edit_save());
    assert(fs_write(id,"After rename.",13)==13);replace_text("Identical rewrite accepted.");assert(edit_save());
    assert(fs_write(id,"Changed outside.",16)==16);replace_text("Retained draft.");assert(!edit_save());
    expect_file(id,"Changed outside.");
    assert(!fs_delete(id));int reused=make_file(0,"replacement.txt","Replacement is safe.");assert(reused==id);
    assert(!edit_save()&&edit_file<0&&!edit_source.valid);expect_file(reused,"Replacement is safe.");
    assert(edit_write_named("rescued.txt"));expect_file(edit_file,"Retained draft.");
    /* Failed creation/write rolls back even when a deleted old file ID is reused. */
    id=edit_file;replace_text("Retained draft exceeds the freed source bytes.");
    static char fill[FS_FILE_MAX];int a=fs_create(0,"full-a"),b=fs_create(0,"full-b"),c=fs_create(0,"full-c"),d=fs_create(0,"full-d");
    assert(a>=0&&b>=0&&c>=0&&d>=0);
    assert(fs_write(a,fill,sizeof fill)==sizeof fill&&fs_write(b,fill,sizeof fill)==sizeof fill&&fs_write(c,fill,sizeof fill)==sizeof fill);
    assert(fs_write(d,fill,(int)(fs_capacity()-fs_used_bytes()))>=0);
    EditorSource full_baseline=edit_source;name_dlg=0;assert(!edit_save()&&!name_dlg&&!edit_saved_ok);
    assert(!memcmp(&full_baseline,&edit_source,sizeof full_baseline));expect_file(id,"Retained draft.");
    assert(!fs_delete(id));int before=fs_node_count();assert(!edit_write_named("no-space.txt"));assert(fs_node_count()==before&&fs_find_child(0,"no-space.txt")<0);
    puts("Editor live conflicts, same-byte replacement, rename, undo baselines, reused IDs and create rollback passed");
}
static void readonly_retries(void) {
    reset(0);int id=make_file(0,"report.txt","Original");assert(!fs_sync());
    /* An unknown optional volume exposes saved floppy files in supported read-only recovery. */
    ide_present=1;memset(ide,0x63,sizeof ide);fs_init();assert(fs_load_disk()<0);desktop_reset();
    id=fs_find_child(0,"report.txt");open_editor(id);replace_text("Pending RAM write");
    assert(!edit_save()&&!edit_saved_ok&&!name_dlg);expect_file(id,"Pending RAM write");
    EditorSource pending=edit_source;assert(edit_source_unchanged());edit_undo(0);
    assert(!memcmp(&pending,&edit_source,sizeof pending));assert(!edit_save()&&!name_dlg);expect_file(id,"Original");
    replace_text("Save As pending");assert(!edit_write_named("new.txt"));int owned=edit_file;
    assert(owned>=0&&owned!=id&&edit_source_unchanged()&&!edit_saved_ok);
    replace_text("Retry pending");assert(!edit_write_named("new.txt")&&edit_file==owned&&!name_failure_message);
    expect_file(owned,"Retry pending");
    assert(fs_write(owned,"Someone else",12)==12);assert(!edit_write_named("new.txt")&&name_failure_message);
    expect_file(owned,"Someone else");
    assert(win_open(WK_EDIT)>=0);int count=fs_node_count();assert(!edit_write_named("empty.txt"));
    int empty=edit_file;assert(empty>=0&&fs_size(empty)==0&&fs_node_count()==count+1&&edit_source_unchanged());
    assert(!edit_write_named("empty.txt")&&edit_file==empty&&!name_failure_message);
    for(unsigned i=0;i<sizeof ide;i++)assert(ide[i]==0x63);
    puts("Editor read-only recovery preserves pending RAM baselines and safe direct/Save As retries");
}
static void recovery_workflows(void) {
    reset(1);int docs=fs_mkdir(0,"Documents");
    for(int i=0;i<MAX_WIN;i++){char name[24],saved[32],draft[32];snprintf(name,sizeof name,"doc%d.txt",i);
        snprintf(saved,sizeof saved,"Saved source %d",i);snprintf(draft,sizeof draft,"Recovered draft %d",i);
        open_editor(make_file(docs,name,saved));replace_text(draft);
        if(i==MAX_WIN-1){memset(edit_buf,'M',EDIT_BUF_SIZE-1);edit_buf[EDIT_BUF_SIZE-1]=0;edit_len=edit_caret=EDIT_BUF_SIZE-1;edit_sel_collapse();}}
    session_save();assert(!*session_status&&fs_size(pref("editor-bindings"))==248);
    SavedSession snap;memcpy(&snap,fs_data(pref("session")),sizeof snap);assert(snap.version==1);
    reboot();
    for(int i=0;i<MAX_WIN;i++){context_set(i);char draft[32];snprintf(draft,sizeof draft,"Recovered draft %d",i);
        if(i==MAX_WIN-1){assert(edit_len==EDIT_BUF_SIZE-1&&edit_buf[0]=='M'&&edit_buf[edit_len-1]=='M');}
        else assert(!strcmp(edit_buf,draft));
        assert(!edit_saved_ok&&edit_binding_valid()&&edit_source_unchanged()&&fm_cwd==docs);}
    /* Re-saving a restored draft retains the source baseline rather than adopting the draft. */
    session_save();int source=states[0].doc.file;assert(fs_write(source,"Offline changed",15)==15);reboot();
    context_set(0);assert(edit_file<0&&!edit_saved_ok&&!edit_source.valid&&!strcmp(edit_buf,"Recovered draft 0"));
    expect_file(source,"Offline changed");context_set(1);assert(edit_binding_valid());
    /* A changed recovery draft is still recovered, but cannot inherit an old binding. */
    assert(fs_write(pref("draft1.txt"),"Other complete draft",20)==20);reboot();context_set(1);
    assert(edit_file<0&&!edit_saved_ok&&!strcmp(edit_buf,"Other complete draft"));
    /* Missing legacy sidecars preserve readable version-1 sessions conservatively. */
    assert(!fs_delete(pref("editor-bindings")));reboot();
    for(int i=0;i<MAX_WIN;i++){context_set(i);assert(edit_file<0&&!edit_saved_ok&&!edit_source.valid);}
    puts("Editor eight-window v1 recovery, offline changes, draft pairing and missing legacy metadata passed");
}
static void recovery_pairing(void) {
    reset(1);int id=make_file(0,"source.txt","source");open_editor(id);replace_text("complete draft");session_save();assert(!*session_status);
    SavedEditorBindings original,unsupported;memcpy(&original,fs_data(pref("editor-bindings")),sizeof original);
    unsupported=original;unsupported.version=2;
    assert(fs_write(pref("editor-bindings"),(const char *)&unsupported,sizeof unsupported)==sizeof unsupported);
    reboot();assert(edit_file<0&&!edit_saved_ok&&!strcmp(edit_buf,"complete draft"));expect_file(id,"source");
    assert(fs_write(pref("editor-bindings"),(const char *)&original,sizeof original)==sizeof original);
    reboot();assert(edit_binding_valid()&&edit_source_unchanged());
    SavedSession other_session;memcpy(&other_session,fs_data(pref("session")),sizeof other_session);other_session.win[0].x++;
    assert(fs_write(pref("session"),(const char *)&other_session,sizeof other_session)==sizeof other_session);
    reboot();assert(edit_file<0&&!edit_saved_ok&&!strcmp(edit_buf,"complete draft"));expect_file(id,"source");
    puts("Editor unsupported binding version and mismatched session generation recover conservatively");
}
static void session_capacity(void) {
    reset(1);int id=make_file(0,"source.txt","source");open_editor(id);replace_text("draft baseline");
    session_save();assert(!*session_status);int draft=pref("draft0.txt");
    assert(!fs_delete(pref("editor-bindings")));int count=fs_node_count();
    while(fs_node_count()<fs_node_limit()){char name[24];snprintf(name,sizeof name,"f%d",fs_node_count());assert(fs_create(0,name)>=0);}
    replace_text("draft must remain only in editor");session_save();assert(*session_status);
    expect_file(draft,"draft baseline");assert(pref("editor-bindings")<0&&fs_node_count()>count);
    /* No prefs directory is allocated when all required nodes cannot fit. */
    reset(1);open_editor(make_file(0,"source.txt","source"));
    while(fs_node_count()<fs_node_limit()-1){char name[24];snprintf(name,sizeof name,"f%d",fs_node_count());assert(fs_create(0,name)>=0);}
    count=fs_node_count();session_save();assert(*session_status&&fs_node_count()==count&&fs_find_child(0,"prefs")<0);
    /* A complete Editor draft is never truncated to a floppy's 16 KiB limit. */
    reset(0);open_editor(make_file(0,"source.txt","source"));memset(edit_buf,'L',20000);edit_buf[20000]=0;edit_len=20000;
    count=fs_node_count();session_save();assert(*session_status&&fs_node_count()==count&&fs_find_child(0,"prefs")<0);
    assert(!edit_write_named("too-large.txt")&&fs_node_count()==count&&fs_find_child(0,"too-large.txt")<0);
    /* Existing shrinking drafts precede new zero-byte drafts even when creating
       a node above 64 would reduce the available capacity. */
    reset(1);open_editor(make_file(0,"source.txt","source"));replace_text("A useful draft with reclaimable storage");session_save();assert(!*session_status);
    wins[1]=wins[0];states[1]=states[0];states[1].history.items=(unsigned char *)states[1].undo;wins[0].open=0;
    context_set(1);session_save();assert(!*session_status);wins[0].open=1;context_set(0);edit_clear();
    context_set(1);replace_text("");
    assert(!fs_delete(pref("draft0.txt")));
    while(fs_node_count()<64){char name[24];snprintf(name,sizeof name,"f%d",fs_node_count());assert(fs_create(0,name)>=0);}
    static char fill[FS_FILE_MAX];int bulk[4];
    for(int i=0;i<4;i++){char name[24];snprintf(name,sizeof name,"bulk%d",i);bulk[i]=fs_create(0,name);assert(bulk[i]>=0);}
    for(int i=0;i<4;i++){unsigned n=fs_capacity()-fs_used_bytes();if(n>sizeof fill)n=sizeof fill;assert(fs_write(bulk[i],fill,n)==(int)n);}
    assert(fs_used_bytes()==fs_capacity());
    /* Reclaim enough space to pay for one additional node and the unchanged
       sidecar; enlarging the existing draft is an ordinary fixture operation. */
    int smaller=fs_size(bulk[3])-100;assert(fs_write(bulk[3],fill,smaller)==smaller);
    char reclaim[100];memset(reclaim,'R',sizeof reclaim);assert(fs_write(pref("draft1.txt"),reclaim,sizeof reclaim)==sizeof reclaim);
    unsigned before=fs_used_bytes();session_save();assert(!*session_status&&pref("draft0.txt")>=0&&fs_size(pref("draft1.txt"))==0&&fs_used_bytes()<before);
    SavedSession previous_session;SavedEditorBindings previous_bindings;
    memcpy(&previous_session,fs_data(pref("session")),sizeof previous_session);
    memcpy(&previous_bindings,fs_data(pref("editor-bindings")),sizeof previous_bindings);
    unsigned growth=fs_capacity()-fs_used_bytes();int filled=fs_size(bulk[3])+(int)growth;
    assert(fs_write(bulk[3],fill,filled)==filled&&fs_used_bytes()==fs_capacity());
    context_set(1);memset(edit_buf,'G',300);edit_buf[300]=0;edit_len=300;edit_saved_ok=0;
    count=fs_node_count();session_save();assert(*session_status&&edit_len==300&&fs_node_count()==count);
    assert(fs_size(pref("draft0.txt"))==0&&fs_size(pref("draft1.txt"))==0);
    assert(!memcmp(&previous_session,fs_data(pref("session")),sizeof previous_session));
    assert(!memcmp(&previous_bindings,fs_data(pref("editor-bindings")),sizeof previous_bindings));
    puts("Editor session node/byte preflight, full-document rejection and shrink-before-create ordering passed");
}
int main(void) {
    EditorBinding hash;edit_fingerprint("123456789",9,&hash);assert(hash.size==9&&hash.hash_b==0xcbf43926u&&hash.hash_a==0xbb86b11cu);
    live_workflows();readonly_retries();recovery_workflows();recovery_pairing();session_capacity();return 0;
}
