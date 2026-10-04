/* Reuse the ordinary Writer/FS fixture, then execute exact desktop dialog code. */
#define main writer_fixture_main
#include "writer_host.c"
#undef main
#include "app.h"
#define MAX_WIN 8
#define MENUBAR_H 36
#define BTN_H 30
#define SPREADSHEET_SAVE_OK 1
static struct { int open, seq; } wins[MAX_WIN];
static int context_slot, name_dlg, name_target, name_focus, name_owner, name_owner_seq;
static int name_len, name_failed, dirty, mouse_x, mouse_y, key_sc, shift_down;
static unsigned name_pdf_paper;
static char name_buf[FS_NAME_LEN], key_char;
static const char *name_failure_message;
static int edit_close_owner = -1, document_action, document_target, finishes, other_writes;
static unsigned document_target_identity;
static int edit_close_failed, edit_close_dlg, edit_close_focus;
static int document_wait_for_save(void) { return 0; }
static int document_destination_valid(void) { return 1; }
static int fm_checked_cwd(void) { return 0; }
static void context_set(int owner) { context_slot = owner; }
static void edit_close_cancel(void) { edit_close_owner = -1; }
static int edit_close_valid(void) { return edit_close_owner >= 0; }
static int edit_write_named(const char *name) { (void)name; other_writes++; return 1; }
static int paint_write_named(const char *name) { return edit_write_named(name); }
static int spreadsheet_export_retry_name(char *out, unsigned capacity) { (void)out; (void)capacity; return 0; }
static int spreadsheet_save_as(int parent, const char *name) { (void)parent; return edit_write_named(name); }
static int spreadsheet_export_csv(int parent, const char *name) { (void)parent; return edit_write_named(name); }
static void document_finish(int owner, int action, int target, unsigned identity) {
    assert(owner == 0 && action == 7 && target == 3 && identity == 123); finishes++;
}
#include "writer_dialog_ops.inc"
static void press(int sc, int shift, char ch) { key_sc = sc; shift_down = shift; key_char = ch; namedlg_key(); }
static void name(const char *text) {
    assert(!name_focus);
    while (name_len) press(KEY_BACKSPACE, 0, 0);
    while (*text) press(0, 0, *text++);
}
static void click(int x, int y) { mouse_x = x; mouse_y = y; assert(namedlg_click()); }
int main(void) {
    files[0].valid = files[0].folder = 1; files[0].identity = 1;
    wins[0].open = 1; wins[0].seq = 12;
    gfx_init(back, linear, 800, 600, 32, 3200); writer_init(); type("Native dirty text");
    namedlg_open(6, "document.pdf");
    assert(name_dlg && !name_focus && name_pdf_paper == WRITER_PDF_LETTER);
    int x, y, w, h; namedlg_geom(&x, &y, &w, &h); assert(w == 420 && h == 260);
    int sx, cx, by, bw; namedlg_buttons(x, y, w, h, &sx, &cx, &by, &bw);
    for (int i = 0; i < 5; i++) { press(KEY_TAB, 0, 0); assert(name_focus == (i + 1) % 5); }
    for (int i = 0; i < 5; i++) { press(KEY_TAB, 1, 0); assert(name_focus == (4 - i + 5) % 5); }
    name(""); press(KEY_ENTER, 0, 0); assert(name_dlg && name_failed && strstr(writer_status(), "1-23"));
    name("letter.pdf"); press(KEY_TAB, 0, 0); assert(name_focus == 1);
    press(KEY_RIGHT, 0, 0); assert(name_focus == 2 && name_pdf_paper == WRITER_PDF_A4);
    press(KEY_ENTER, 0, 0); assert(name_dlg && fs_find_child(0, "letter.pdf") < 0);
    press(0, 0, 'x'); assert(!strcmp(name_buf, "letter.pdf"));
    press(KEY_LEFT, 0, 0); press(KEY_SPACE, 0, ' ');
    assert(name_focus == 1 && name_pdf_paper == WRITER_PDF_LETTER);
    press(KEY_TAB, 1, 0); assert(!name_focus);
    press(KEY_TAB, 1, 0); assert(name_focus == 4);
    storage_busy = 1; int busy_creates = create_count, busy_writes = write_count;
    press(KEY_ENTER, 0, 0);
    assert(name_dlg && name_failed && fs_find_child(0, "letter.pdf") < 0 && writer_dirty());
    assert(strstr(writer_status(), "Disk is saving; retry shortly"));
    assert(create_count == busy_creates && write_count == busy_writes);
    storage_busy = 0;
    press(KEY_ENTER, 0, 0); assert(!name_dlg && fs_find_child(0, "letter.pdf") >= 0 && writer_dirty());
    namedlg_open(6, "a4.pdf"); click(x + 250, y + 160);
    assert(name_focus == 2 && name_pdf_paper == WRITER_PDF_A4);
    click(x + 30, y + 120); assert(!name_focus);
    sync_failure = 1; click(sx + 4, by + 4);
    assert(name_dlg && name_failed && strstr(writer_status(), "RAM") && writer_dirty());
    int id = fs_find_child(0, "a4.pdf"), writes = write_count; assert(id >= 0);
    click(sx + 4, by + 4); assert(name_dlg && write_count == writes);
    storage_busy = 1; click(sx + 4, by + 4);
    assert(name_dlg && name_failed && write_count == writes && writer_dirty());
    assert(strstr(writer_status(), "Disk is saving; retry shortly"));
    storage_busy = sync_failure = 0; click(sx + 4, by + 4);
    assert(!name_dlg && write_count == writes && writer_dirty());
    namedlg_open(6, "letter.pdf"); press(KEY_ENTER, 0, 0);
    assert(name_dlg && name_failed && strstr(writer_status(), "existing")); press(KEY_ESC, 0, 0); assert(!name_dlg);
    namedlg_open(6, "wrong.txt"); press(KEY_ENTER, 0, 0);
    assert(name_dlg && name_failed && strstr(writer_status(), ".pdf"));
    click(cx + 4, by + 4); assert(!name_dlg && fs_find_child(0, "wrong.txt") < 0);
    namedlg_open(6, "cancelled.pdf"); for (int i = 0; i < 3; i++) press(KEY_TAB, 0, 0);
    press(KEY_ENTER, 0, 0); assert(!name_dlg && fs_find_child(0, "cancelled.pdf") < 0);
    namedlg_open(6, "outside.pdf"); click(x - 1, y); assert(!name_dlg && fs_find_child(0, "outside.pdf") < 0);
    namedlg_open(6, "orphan.pdf"); wins[0].seq++;
    press(KEY_ENTER, 0, 0); assert(!name_dlg && fs_find_child(0, "orphan.pdf") < 0);
    /* Legacy targets retain their three focus stops and old dispatch. */
    namedlg_open(3, "legacy.rtf"); namedlg_geom(&x, &y, &w, &h); assert(h == 168);
    for (int i = 0; i < 3; i++) { press(KEY_TAB, 0, 0); assert(name_focus == (i + 1) % 3); }
    press(KEY_ENTER, 0, 0); assert(!name_dlg && fs_find_child(0, "legacy.rtf") >= 0 && writer_dirty());
    namedlg_open(2, "saved.bwr"); edit_close_owner = 0; document_action = 7; document_target = 3; document_target_identity = 123;
    storage_busy = 1; press(KEY_ENTER, 0, 0);
    assert(name_dlg && name_failed && writer_dirty() && !finishes && fs_find_child(0, "saved.bwr") < 0);
    storage_busy = 0;
    press(KEY_ENTER, 0, 0); assert(!name_dlg && !writer_dirty() && finishes == 1);
    for (int target = 0; target <= 5; target++) if (target != 2 && target != 3) {
        namedlg_open(target, "ordinary"); press(KEY_ENTER, 0, 0); assert(!name_dlg);
    }
    assert(other_writes == 4);
    for (int i = 1; i < 20; i++) if (files[i].valid) free(files[i].data);
    puts("Writer dialog: exact production keyboard/mouse paper choice, errors, sync retry, owner guards and legacy dispatch passed.");
    return 0;
}
