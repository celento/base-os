/* Exact production session code plus real filesystem, sheet model and client.
 * Functional source, recovery and space admission checks; no fault injection. */
#define SPREADSHEET_RECOVERY_TEST
#define main editor_harness_main
#include "editor_binding_host.c"
#undef main
#include "example_sheet.h"
static unsigned char buffer[SHEET_NATIVE_MAX_SIZE];
static SheetDoc checked;
static void sheet_type(const char *text) { while(*text)spreadsheet_key(0,*text++,0); }
static void sheet_key(int sc,int mods) { spreadsheet_key(sc,0,mods); }
static void sheet_jump(const char *address) {
    sheet_key(0x22,SPREADSHEET_MOD_CTRL);sheet_key(0x1e,SPREADSHEET_MOD_CTRL);
    sheet_type(address);sheet_key(0x1c,0);
}
static void sheet_enter(const char *address,const char *text) {
    sheet_jump(address);sheet_type(text);sheet_key(0x1c,0);
}
static void expect_cell(unsigned row,unsigned col,const char *text) {
    const SheetCell *cell=sheet_cell(spreadsheet_document(),row,col);
    assert(cell&&cell->length==strlen(text)&&!memcmp(cell->text,text,cell->length));
}
static int sheet_start(void) { int slot=win_open(WK_SPREADSHEET);assert(slot>=0);return slot; }
static void sheet_reboot(void) { assert(!fs_sync());fs_init();assert(!fs_load_disk());spreadsheet_close();desktop_reset();session_restore(); }
static void test_pending_recovery(void) {
    reset(1);int docs=fs_mkdir(0,"Documents");sheet_start();fm_set_cwd(docs);
    sheet_enter("A1","12.50");sheet_enter("B1","=A1*2");
    assert(spreadsheet_save_as(docs,"budget.bsh")==SPREADSHEET_SAVE_OK);
    int source=spreadsheet_file();SpreadsheetBinding baseline;assert(spreadsheet_binding(&baseline));
    sheet_jump("A1");sheet_type("27.125");assert(spreadsheet_editing());
    unsigned caret=spreadsheet_caret();session_save();assert(!*session_status);
    assert(spreadsheet_editing()&&spreadsheet_caret()==caret);expect_cell(0,0,"12.50");
    assert(spreadsheet_binding_matches(source,&baseline));
    int draft=pref("sheet-draft.bsh");assert(draft>=0);
    assert(!sheet_native_decode(&checked,(const unsigned char *)fs_data(draft),fs_size(draft)));
    assert(!strcmp(checked.cells[0].text,"27.125")&&checked.cells[1].value==54250);
    sheet_reboot();expect_cell(0,0,"27.125");assert(spreadsheet_file()==source&&spreadsheet_dirty());
    assert(spreadsheet_caret()==caret&&fm_cwd==docs);
    assert(spreadsheet_binding_matches(source,&baseline));
    assert(spreadsheet_save()==SPREADSHEET_SAVE_OK);
    assert(!spreadsheet_binding_matches(source,&baseline));
    puts("Spreadsheet session: pending edit preserved live, encoded, rebound and saved after reboot");
}
static void saved_fixture(void) {
    spreadsheet_close();reset(1);sheet_start();sheet_enter("A1","100");
    assert(spreadsheet_save_as(0,"source.bsh")==SPREADSHEET_SAVE_OK);
    sheet_enter("B2","Draft survives");session_save();assert(!*session_status);
}
static void expect_unbound(void) {
    sheet_reboot();expect_cell(1,1,"Draft survives");assert(spreadsheet_file()==-1&&spreadsheet_dirty());
    assert(spreadsheet_save()==SPREADSHEET_SAVE_NEEDS_NAME);
}
static void test_pairing(void) {
    saved_fixture();int source=spreadsheet_file();
    unsigned n=(unsigned)fs_size(source);memcpy(buffer,fs_data(source),n);
    assert(buffer[n-3]=='1');buffer[n-3]='2';assert(fs_write(source,(const char *)buffer,n)==(int)n);
    expect_unbound();assert(fs_data(source)[n-3]=='2');
    saved_fixture();assert(!fs_delete(pref("sheet-binding")));expect_unbound();
    saved_fixture();SavedSheetBinding binding;memcpy(&binding,fs_data(pref("sheet-binding")),sizeof binding);
    binding.version++;assert(fs_write(pref("sheet-binding"),(const char *)&binding,sizeof binding)==sizeof binding);expect_unbound();
    saved_fixture();int draft=pref("sheet-draft.bsh");n=fs_size(draft);memcpy(buffer,fs_data(draft),n);
    assert(buffer[20]=='1');buffer[20]='3';assert(fs_write(draft,(const char *)buffer,n)==(int)n);
    expect_unbound();expect_cell(0,0,"300");
    saved_fixture();SavedSession snap;memcpy(&snap,fs_data(pref("session")),sizeof snap);snap.win[0].x++;
    assert(fs_write(pref("session"),(const char *)&snap,sizeof snap)==sizeof snap);expect_unbound();
    saved_fixture();memcpy(&binding,fs_data(pref("sheet-binding")),sizeof binding);binding.anchor=0;
    assert(fs_write(pref("sheet-binding"),(const char *)&binding,sizeof binding)==sizeof binding);
    sheet_reboot();assert(spreadsheet_file()>=0&&spreadsheet_anchor()==0);
    puts("Spreadsheet session: changed/missing/versioned/mixed bindings recover intact unbound drafts");
}
static void test_admission(void) {
    saved_fixture();int draft=pref("sheet-draft.bsh");unsigned n=fs_size(draft);memcpy(buffer,fs_data(draft),n);
    assert(!fs_delete(pref("sheet-binding")));
    while(fs_node_count()<fs_node_limit()){char name[24];snprintf(name,sizeof name,"node%d",fs_node_count());assert(fs_create(0,name)>=0);}
    sheet_enter("C3","Cannot fit metadata");session_save();assert(*session_status);
    assert(fs_size(draft)==(int)n&&!memcmp(fs_data(draft),buffer,n)&&pref("sheet-binding")<0);
    saved_fixture();draft=pref("sheet-draft.bsh");n=fs_size(draft);memcpy(buffer,fs_data(draft),n);
    static char bulk[FS_FILE_MAX];int fill[12];unsigned count=0;
    while(fs_capacity()-fs_used_bytes()>0){
        assert(count<12);char name[24];snprintf(name,sizeof name,"bulk%u",count);
        fill[count]=fs_create(0,name);assert(fill[count]>=0);
        unsigned bytes=fs_capacity()-fs_used_bytes();if(bytes>sizeof bulk)bytes=sizeof bulk;
        assert(fs_write(fill[count],bulk,bytes)==(int)bytes);count++;
    }
    sheet_enter("C3","No byte room");session_save();assert(*session_status);
    assert(fs_size(draft)==(int)n&&!memcmp(fs_data(draft),buffer,n));
    /* Shrink the native draft before creating the first Editor draft, even at
     * full capacity. Small metadata already exists and remains the same size. */
    unsigned need=sizeof(SavedEditorBindings)+80;
    int last=fill[count-1];assert(fs_size(last)>(int)need);
    assert(fs_write(last,bulk,fs_size(last)-(int)need)>=0);
    int editor=win_open(WK_EDIT);replace_text("");session_save();assert(!*session_status);
    int editor_draft=pref("draft1.txt");assert(editor_draft>=0);assert(!fs_delete(editor_draft));
    context_set(0);sheet_jump("A1");sheet_key(0x1e,SPREADSHEET_MOD_CTRL);sheet_key(0x53,0);
    context_set(editor);replace_text("Small reclaimed draft");
    last=fill[count-1];unsigned free=fs_capacity()-fs_used_bytes();assert(fs_write(last,bulk,fs_size(last)+(int)free)>=0);
    session_save();assert(!*session_status&&fs_size(pref("sheet-draft.bsh"))==16);
    assert(fs_size(pref("draft1.txt"))==21);
    puts("Spreadsheet session: node/byte preflight and cross-app shrink-before-growth ordering passed");
}
static void test_budget(void) {
    unsigned n=example_budget_sheet(buffer,sizeof buffer);assert(n>16&&n<1024);
    assert(!sheet_native_decode(&checked,buffer,n));
    assert(checked.cells[157].value==1900000&&checked.cells[158].value==1836350&&checked.cells[159].value==63650);
    unsigned bytes;assert(!sheet_csv_export(&checked,buffer,sizeof buffer,&bytes));
    assert(bytes==strlen(example_budget_csv)&&!memcmp(buffer,example_budget_csv,bytes));
    puts("Spreadsheet budget: native formulas and exact value CSV agree");
}
int main(void) { test_pending_recovery();test_pairing();test_admission();test_budget();return 0; }
