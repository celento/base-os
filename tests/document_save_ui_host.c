/* Exact production desktop continuations with both real singleton models. */
#define main writer_fixture_main
#include "writer_host.c"
#undef main
#include "sheet.h"
#define MAX_WIN 8
#define MENU_NONE (-1)
enum { WK_EDIT = 5, WK_WRITER = 22, WK_SPREADSHEET = 23 };
static struct { int open, kind, seq; } wins[MAX_WIN];
typedef struct { int saved_ok, file; unsigned identity; } Document;
static struct { Document doc; } window_state[MAX_WIN];
static int context_slot, name_dlg, name_target, name_focus, name_owner, name_owner_seq;
static int name_len, name_failed, dirty;
static unsigned name_pdf_paper;
static char name_buf[FS_NAME_LEN];
static const char *name_failure_message;
static int edit_close_owner = -1, edit_close_seq, edit_close_kind;
static int edit_close_dlg, edit_close_focus, edit_close_failed;
enum { DOCUMENT_CLOSE, DOCUMENT_NEW, DOCUMENT_OPEN };
static int document_action, document_target = -1;
static unsigned document_target_identity, document_target_incarnation, document_target_version;
static unsigned document_pending_owner, document_pending_handle;
static int open_menu, dragging_win, resizing_win, drag_active, fm_dragging, fm_drag_active, edit_dragging;
static int closed, cwd;
static int fm_checked_cwd(void) { return 0; }
static void fm_set_cwd(int parent) { cwd = parent; }
int fs_parent(int id) { return fs_valid(id) ? 0 : -1; }
static void context_set(int owner) { context_slot = owner; }
static void win_focus(int owner) { context_set(owner); }
static void win_close(int owner) {
    assert(owner >= 0 && owner < MAX_WIN);
    if (wins[owner].kind == WK_WRITER) writer_close();
    else if (wins[owner].kind == WK_SPREADSHEET) spreadsheet_close();
    wins[owner].open = 0; ++closed;
}
static int edit_save(void) { return 1; }
static int edit_write_named(const char *name) { (void)name; return 1; }
static int paint_write_named(const char *name) { return edit_write_named(name); }
static void namedlg_open(int target, const char *initial);
#include "document_save_ui_ops.inc"
static void writer_window(void) {
    assert(!storage_busy); writer_new(); edit_close_cancel(); name_dlg = 0;
    wins[0].open = 1; wins[0].kind = WK_WRITER; ++wins[0].seq; context_set(0);
}
static void sheet_window(void) {
    assert(!storage_busy); spreadsheet_new(); edit_close_cancel(); name_dlg = 0;
    wins[1].open = 1; wins[1].kind = WK_SPREADSHEET; ++wins[1].seq; context_set(1);
}
static void terminal(int result) {
    async_result = result; storage_busy = 0; document_persistence_poll();
    assert(!async_owner && !async_handle);
}
static void writer_close_flow(void) {
    writer_window(); type("Keep until durable"); int closes = closed;
    document_request(0, DOCUMENT_CLOSE, -1); assert(edit_close_dlg && edit_close_focus == 2);
    edit_close_choose(0); assert(name_dlg && !edit_close_dlg && edit_close_owner == 0);
    strcpy(name_buf, "close.bwr"); name_len = (int)strlen(name_buf); namedlg_commit();
    assert(!name_dlg && edit_close_dlg && document_pending_owner && wins[0].open && closed == closes);
    unsigned owner = document_pending_owner, handle = document_pending_handle;
    edit_close_choose(0); edit_close_choose(1);
    assert(document_pending_owner == owner && document_pending_handle == handle && closed == closes);
    context_set(1); /* Drawing/focus context never redirects singleton completion. */
    terminal(BOS_OK); assert(!wins[0].open && !edit_close_dlg && closed == closes + 1);
    puts("Document UI: Save As acceptance keeps exact close intent until durability passed");
}
static void cancel_and_failure(void) {
    writer_window(); type("Cancel close"); document_request(0, DOCUMENT_CLOSE, -1); edit_close_choose(0);
    strcpy(name_buf, "cancel.bwr"); name_len = (int)strlen(name_buf); namedlg_commit();
    assert(document_pending_owner); edit_close_choose(2);
    assert(edit_close_owner < 0 && !document_pending_owner && storage_busy && wins[0].open);
    terminal(BOS_OK); assert(wins[0].open && !writer_dirty());
    type(" retry"); document_request(0, DOCUMENT_NEW, -1); edit_close_choose(0);
    assert(document_pending_owner && !name_dlg); terminal(BOS_E_IO);
    assert(wins[0].open && edit_close_dlg && edit_close_failed == 1 && edit_close_focus == 2 && writer_dirty());
    edit_close_choose(0); assert(document_pending_owner); terminal(BOS_OK);
    assert(wins[0].open && !writer_length() && !writer_dirty() && edit_close_owner < 0);
    puts("Document UI: Cancel cancels navigation only; failed native save preserves New and exact retry passed");
}
static void newer_edits_and_window_reuse(void) {
    writer_window(); type("A"); assert(writer_save_as(0, "newer.bwr") == WRITER_SAVE_PENDING);
    type("B"); document_request(0, DOCUMENT_CLOSE, -1); edit_close_choose(0); assert(document_pending_owner);
    terminal(BOS_OK); assert(wins[0].open && writer_dirty() && edit_close_failed == 2 && !document_pending_owner);
    edit_close_choose(2); assert(!edit_close_dlg);
    document_request(0, DOCUMENT_CLOSE, -1); edit_close_choose(0); assert(document_pending_owner);
    ++wins[0].seq; terminal(BOS_OK);
    assert(wins[0].open && !document_pending_owner && edit_close_owner < 0);
    /* Same slot/seq/kind cannot substitute a new logical document/request. */
    type("C"); document_request(0, DOCUMENT_CLOSE, -1); edit_close_choose(0);
    assert(document_pending_owner); writer_new(); type("Replacement model");
    storage_busy = 0; document_persistence_poll();
    assert(wins[0].open && edit_close_dlg && edit_close_failed == 1 && writer_dirty()); edit_close_choose(2);
    puts("Document UI: newer edits, reused windows and replaced document owner cannot close newer work passed");
}
static void open_target_version(void) {
    writer_window(); type("Target"); assert(writer_save_as(0, "target.bwr") == WRITER_SAVE_PENDING); terminal(BOS_OK);
    int target = writer_file(); unsigned target_length = (unsigned)fs_size(target);
    unsigned char *bytes = malloc(target_length); assert(bytes); memcpy(bytes, fs_data(target), target_length);
    writer_new(); type("Current"); assert(writer_save_as(0, "current.bwr") == WRITER_SAVE_PENDING); terminal(BOS_OK);
    type(" edit"); document_request(0, DOCUMENT_OPEN, target); edit_close_choose(0); assert(document_pending_owner);
    storage_busy = 0; async_result = BOS_OK;
    assert(fs_write(target, (const char *)bytes, (int)target_length) == (int)target_length); free(bytes);
    document_persistence_poll(); assert(edit_close_dlg && edit_close_failed == 3 && wins[0].open);
    check_text("Current edit"); edit_close_choose(1); assert(edit_close_failed == 3); edit_close_choose(2);
    type("!"); document_request(0, DOCUMENT_OPEN, target); edit_close_choose(0); terminal(BOS_OK);
    check_text("Target"); assert(writer_file() == target && !writer_dirty() && !edit_close_dlg);
    puts("Document UI: deferred Open binds exact target content version, then valid retry opens correctly passed");
}
static void exports_and_sheet(void) {
    writer_window(); type("Export"); namedlg_open(3, "ui.rtf"); namedlg_commit();
    assert(!name_dlg && !edit_close_dlg && !document_pending_owner && writer_dirty());
    type(" edit"); terminal(BOS_OK); assert(writer_dirty());
    namedlg_open(6, "ui.pdf"); namedlg_commit(); assert(!name_dlg && !document_pending_owner); terminal(BOS_E_IO);
    assert(wins[0].open && writer_dirty());
    sheet_window(); const char *value = "12.5"; while (*value) spreadsheet_key(0, *value++, 0);
    document_request(1, DOCUMENT_CLOSE, -1); edit_close_choose(0); assert(name_dlg && name_target == 4);
    strcpy(name_buf, "ui.bsh"); name_len = (int)strlen(name_buf); namedlg_commit();
    assert(!name_dlg && document_pending_owner && spreadsheet_dirty());
    edit_close_choose(2); terminal(BOS_OK); assert(wins[1].open && !spreadsheet_dirty());
    namedlg_open(5, "ui.csv"); namedlg_commit(); assert(!name_dlg && !document_pending_owner); terminal(BOS_OK);
    document_request(1, DOCUMENT_NEW, -1); assert(!edit_close_dlg && spreadsheet_file() < 0);
    value = "new cell"; while (*value) spreadsheet_key(0, *value++, 0);
    document_request(1, DOCUMENT_CLOSE, -1); edit_close_choose(0);
    strcpy(name_buf, "close-sheet.bsh"); name_len = (int)strlen(name_buf); namedlg_commit(); terminal(BOS_OK);
    assert(!wins[1].open && !edit_close_dlg);
    puts("Document UI: ordinary export dismissal preserves edits; Sheet native/export/cancel/close paths passed");
}
int main(void) {
    files[0].valid = files[0].folder = 1; files[0].identity = 1;
    gfx_init(back, linear, 800, 600, 32, 3200); writer_init(); spreadsheet_init(); async_mode = 1;
    writer_close_flow(); cancel_and_failure(); newer_edits_and_window_reuse(); open_target_version(); exports_and_sheet();
    for (int i = 1; i < 20; ++i) if (files[i].valid) free(files[i].data);
    puts("All exact document UI persistence checks passed."); return 0;
}
