/* Ordinary Spreadsheet client workflows, using real graphics and host sanitizers. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sheet.h"
#include "fs.h"
#include "layout.h"
static unsigned char presented[FB_CAPACITY];
#undef PRESENT_BASE
#define PRESENT_BASE ((uintptr_t)presented)
#define GFX_HOST_TEST
#include "../src/gfx.c"

#define FILE_COUNT 40
static unsigned polls, ticks, clipboard_generation, clipboard_length;
static unsigned char clipboard[SPREADSHEET_CLIPBOARD_CAPACITY + 1u];
static int create_failure, write_failure, sync_failure, write_count, sync_count;
static int clipboard_set_failure, clipboard_get_failure;
static int storage_busy;
int fs_sync_busy(void) { return storage_busy; }
static unsigned next_identity = 1, file_limit = FS_FILE_MAX;
typedef struct {
    int valid, folder, size;
    unsigned identity;
    char name[FS_NAME_LEN];
    unsigned char *data;
} File;
static File files[FILE_COUNT];
static unsigned char back[800 * 600], linear[800 * 600 * 4];
static SheetDoc fixture, prior, decoded;
static unsigned char encoded[SHEET_NATIVE_MAX_SIZE];

void kmemset(void *d, int value, int count) { memset(d, value, (size_t)count); }
void kmemcpy(void *d, const void *s, int count) { memcpy(d, s, (size_t)count); }
int kstrlen(const char *s) { return (int)strlen(s); }
int kstrcmp(const char *a, const char *b) { return strcmp(a, b); }
void kstrcpy(char *d, const char *s) { strcpy(d, s); }
void platform_poll(void) { polls++; }
unsigned timer_ticks(void) { return ticks; }
unsigned spreadsheet_clipboard_set(const char *text, unsigned length) {
    assert(length <= SPREADSHEET_CLIPBOARD_CAPACITY);
    if (clipboard_set_failure) return 0;
    memcpy(clipboard, text, length); clipboard[length] = 0;
    clipboard_length = length; return ++clipboard_generation;
}
int spreadsheet_clipboard_get(char *text, unsigned capacity, unsigned *generation) {
    if (clipboard_get_failure || clipboard_length > capacity) return -1;
    memcpy(text, clipboard, clipboard_length);
    if (clipboard_length < capacity) text[clipboard_length] = 0;
    *generation = clipboard_generation; return (int)clipboard_length;
}
int fs_valid(int id) { return id >= 0 && id < FILE_COUNT && files[id].valid; }
int fs_is_dir(int id) { return fs_valid(id) && files[id].folder; }
int fs_is_app(int id) { (void)id; return 0; }
unsigned fs_identity(int id) { return fs_valid(id) ? files[id].identity : 0; }
int fs_size(int id) { return fs_valid(id) ? files[id].size : -1; }
const char *fs_name(int id) { return fs_valid(id) ? files[id].name : ""; }
const char *fs_data(int id) { return fs_valid(id) ? (const char *)files[id].data : NULL; }
int fs_find_child(int parent, const char *name) {
    if (parent != 0 || !name) return -1;
    for (int i = 1; i < FILE_COUNT; i++)
        if (files[i].valid && !strcmp(name, files[i].name)) return i;
    return -1;
}
int fs_create(int parent, const char *name) {
    if (storage_busy) return FS_ERR_BUSY;
    if (create_failure || parent != 0 || !name || !*name || strlen(name) >= FS_NAME_LEN ||
        strchr(name, '/') || fs_find_child(parent, name) >= 0) return -1;
    for (int i = 1; i < FILE_COUNT; i++) if (!files[i].valid) {
        files[i].valid = 1; files[i].identity = ++next_identity; files[i].size = 0;
        files[i].data = malloc(1); assert(files[i].data); files[i].data[0] = 0;
        strcpy(files[i].name, name); return i;
    }
    return -1;
}
int fs_write(int id, const char *text, int length) {
    if (storage_busy) return FS_ERR_BUSY;
    write_count++;
    if (write_failure || !fs_valid(id) || files[id].folder || length < 0 ||
        (unsigned)length > file_limit) return -1;
    unsigned char *data = malloc((unsigned)length + 1u); assert(data);
    memcpy(data, text, (unsigned)length); data[length] = 0;
    free(files[id].data); files[id].data = data; files[id].size = length; return length;
}
int fs_delete(int id) {
    if (storage_busy) return FS_ERR_BUSY;
    if (!fs_valid(id) || files[id].folder) return -1;
    free(files[id].data); memset(&files[id], 0, sizeof files[id]); return 0;
}
unsigned fs_file_limit(void) { return file_limit; }
int fs_sync(void) { sync_count++; return sync_failure ? -1 : 0; }

static void key(int scancode, int modifiers) { (void)spreadsheet_key(scancode, 0, modifiers); }
static void type(const char *text) {
    while (*text) assert(spreadsheet_key(0, *text++, 0) & SPREADSHEET_CHANGED);
}
static void jump(const char *label) {
    unsigned row, col;
    assert(!sheet_reference(label, (unsigned)strlen(label), &row, &col));
    key(0x22, SPREADSHEET_MOD_CTRL); key(0x1e, SPREADSHEET_MOD_CTRL);
    type(label); key(0x1c, 0);
    assert(spreadsheet_caret() == row * SHEET_COLS + col);
    assert(spreadsheet_anchor() == spreadsheet_caret());
}
static void enter(const char *label, const char *source) {
    jump(label); type(source); assert(spreadsheet_editing()); key(0x1c, 0);
    assert(!spreadsheet_editing());
}
static void cell(unsigned row, unsigned col, SheetKind kind, const char *text,
                 int32_t value, SheetError error) {
    const SheetCell *c = sheet_cell(spreadsheet_document(), row, col); assert(c);
    if (c->kind != kind || c->length != strlen(text) || strcmp(c->text, text) ||
        c->value != value || c->error != error)
        fprintf(stderr, "Cell %u,%u expected kind %u [%s] value %d error %u; got kind %u [%s] value %d error %u. %s\n",
                row, col, kind, text, value, error, c->kind, c->text, c->value,
                c->error, spreadsheet_status());
    assert(c->kind == kind && c->length == strlen(text)); assert(!strcmp(c->text, text));
    assert(c->value == value && c->error == error);
    assert(!sheet_validate(spreadsheet_document()));
}
static void compare_docs(const SheetDoc *a, const SheetDoc *b) {
    for (unsigned i = 0; i < SHEET_CELLS; i++) {
        const SheetCell *x = &a->cells[i], *y = &b->cells[i];
        assert(x->kind == y->kind && x->length == y->length);
        assert(!memcmp(x->text, y->text, x->length + 1u));
        assert(x->value == y->value && x->error == y->error);
    }
    assert(!sheet_validate(a) && !sheet_validate(b));
}
static int make_file(const char *name, const void *data, unsigned size) {
    int id = fs_find_child(0, name); if (id < 0) id = fs_create(0, name);
    assert(id > 0); assert(fs_write(id, data, (int)size) == (int)size); return id;
}
static int load_fixture(void) {
    unsigned size; assert(!sheet_native_encode(&fixture, encoded, sizeof encoded, &size));
    int id = make_file("fixture.bsh", encoded, size); assert(spreadsheet_open_file(id)); return id;
}
static void basic_fixture(void) {
    sheet_init(&fixture);
    assert(!sheet_set(&fixture, 0, 0, SHEET_NUMBER, "12.5000", 7));
    assert(!sheet_set(&fixture, 0, 1, SHEET_FORMULA, "=A1*2", 5));
    assert(!sheet_set(&fixture, 1, 0, SHEET_TEXT, "=1+2", 4));
    assert(!sheet_set(&fixture, 1, 1, SHEET_TEXT, "'literal", 8));
    assert(!sheet_set(&fixture, 2, 0, SHEET_TEXT, "", 0));
    assert(!sheet_recalculate(&fixture));
}
static void test_editing(void) {
    spreadsheet_init(); assert(!spreadsheet_dirty()); assert(!spreadsheet_editing());
    assert(spreadsheet_caret() == 0 && spreadsheet_anchor() == 0);
    type("12.5000"); assert(spreadsheet_editing());
    cell(0, 0, SHEET_EMPTY, "", 0, SHEET_OK);
    key(0x01, 0); assert(!spreadsheet_editing() && !spreadsheet_dirty());
    type("12.5000"); key(0x1c, 0); cell(0, 0, SHEET_NUMBER, "12.5000", 12500, SHEET_OK);
    assert(spreadsheet_caret() == SHEET_COLS && spreadsheet_dirty());
    jump("B1"); type("=A1*2"); key(0x0f, 0);
    assert(spreadsheet_caret() == 2); cell(0, 1, SHEET_FORMULA, "=A1*2", 25000, SHEET_OK);
    jump("B1"); key(0x3c, 0); assert(spreadsheet_editing()); key(0x4f, 0);
    key(0x0e, 0); type("3"); key(0x1c, 0);
    cell(0, 1, SHEET_FORMULA, "=A1*3", 37500, SHEET_OK);
    enter("C1", "'=1+2"); cell(0, 2, SHEET_TEXT, "=1+2", 0, SHEET_OK);
    jump("C1"); key(0x3c, 0); key(0x4f, 0); type("0"); key(0x1c, 0);
    cell(0, 2, SHEET_TEXT, "=1+20", 0, SHEET_OK);
    jump("C1"); key(0x3c, 0); key(0x47, 0); type("cancel"); key(0x01, 0);
    cell(0, 2, SHEET_TEXT, "=1+20", 0, SHEET_OK);
    enter("C1", "=1+2"); cell(0, 2, SHEET_FORMULA, "=1+2", 3000, SHEET_OK);
    enter("D1", "'001.00"); jump("D1"); key(0x3c, 0); key(0x4f, 0); type("0"); key(0x1c, 0);
    cell(0, 3, SHEET_TEXT, "001.000", 0, SHEET_OK);
    enter("A1", "4"); cell(0, 1, SHEET_FORMULA, "=A1*3", 12000, SHEET_OK);
    enter("E1", "=1/0"); cell(0, 4, SHEET_FORMULA, "=1/0", 0, SHEET_ERR_DIV0);
    prior = *spreadsheet_document(); jump("B2"); type("discard"); key(0x01, 0);
    compare_docs(&prior, spreadsheet_document());
    assert(spreadsheet_key(0x1f, 0, SPREADSHEET_MOD_CTRL) == SPREADSHEET_REQUEST_SAVE);
    assert(spreadsheet_key(0x1f, 0, SPREADSHEET_MOD_CTRL | SPREADSHEET_MOD_SHIFT) == SPREADSHEET_REQUEST_SAVE_AS);
    assert(spreadsheet_key(0x12, 0, SPREADSHEET_MOD_CTRL | SPREADSHEET_MOD_SHIFT) == SPREADSHEET_REQUEST_EXPORT);
    puts("Spreadsheet editing: staged edit/cancel, F2 raw source, literal kinds, Enter/Tab and recalculation passed");
}
static void test_navigation(void) {
    spreadsheet_new(); jump("A1"); key(0x4b, 0); key(0x48, 0); assert(spreadsheet_caret() == 0);
    jump("Z128"); key(0x4d, 0); key(0x50, 0); assert(spreadsheet_caret() == SHEET_CELLS - 1u);
    jump("B2"); key(0x4d, SPREADSHEET_MOD_SHIFT); key(0x50, SPREADSHEET_MOD_SHIFT);
    assert(spreadsheet_anchor() == 27 && spreadsheet_caret() == 54);
    key(0x4b, 0); assert(spreadsheet_anchor() == spreadsheet_caret());
    unsigned caret = spreadsheet_caret();
    key(0x22, SPREADSHEET_MOD_CTRL); key(0x1e, SPREADSHEET_MOD_CTRL); type("Z129"); key(0x1c, 0);
    assert(spreadsheet_caret() == caret); key(0x01, 0); assert(!spreadsheet_dirty());
    jump("b12"); assert(spreadsheet_caret() == 11 * SHEET_COLS + 1);
    puts("Spreadsheet navigation: address field, invalid address, boundaries and Shift range anchors passed");
}
static void test_history(void) {
    spreadsheet_new();
    for (char digit = '1'; digit <= '6'; digit++) {
        char source[2] = {digit, 0}; enter("A1", source);
    }
    for (int i = 0; i < 4; i++) key(0x2c, SPREADSHEET_MOD_CTRL);
    cell(0, 0, SHEET_NUMBER, "2", 2000, SHEET_OK);
    key(0x2c, SPREADSHEET_MOD_CTRL); cell(0, 0, SHEET_NUMBER, "2", 2000, SHEET_OK);
    for (int i = 0; i < 4; i++) key(0x15, SPREADSHEET_MOD_CTRL);
    cell(0, 0, SHEET_NUMBER, "6", 6000, SHEET_OK);
    key(0x2c, SPREADSHEET_MOD_CTRL); enter("A1", "9"); key(0x15, SPREADSHEET_MOD_CTRL);
    cell(0, 0, SHEET_NUMBER, "9", 9000, SHEET_OK);
    key(0x2c, SPREADSHEET_MOD_CTRL); cell(0, 0, SHEET_NUMBER, "5", 5000, SHEET_OK);
    key(0x2c, SPREADSHEET_MOD_CTRL | SPREADSHEET_MOD_SHIFT);
    cell(0, 0, SHEET_NUMBER, "9", 9000, SHEET_OK);
    puts("Spreadsheet history: four-step undo bound, redo and redo-branch replacement passed");
}
static void select_2x2(const char *label) {
    jump(label); key(0x4d, SPREADSHEET_MOD_SHIFT); key(0x50, SPREADSHEET_MOD_SHIFT);
}
static void test_clipboard(void) {
    basic_fixture(); load_fixture(); select_2x2("A1"); key(0x2e, SPREADSHEET_MOD_CTRL);
    assert(clipboard_length > 0); jump("D4"); key(0x2f, SPREADSHEET_MOD_CTRL);
    cell(3, 3, SHEET_NUMBER, "12.5000", 12500, SHEET_OK);
    cell(3, 4, SHEET_FORMULA, "=A1*2", 25000, SHEET_OK);
    cell(4, 3, SHEET_TEXT, "=1+2", 0, SHEET_OK);
    cell(4, 4, SHEET_TEXT, "'literal", 0, SHEET_OK);
    /* Another owner writing identical bytes invalidates the private rich copy. */
    spreadsheet_clipboard_set((const char *)clipboard, clipboard_length);
    jump("G4"); key(0x2f, SPREADSHEET_MOD_CTRL);
    cell(3, 6, SHEET_NUMBER, "12.5", 12500, SHEET_OK);
    cell(3, 7, SHEET_NUMBER, "25", 25000, SHEET_OK);
    cell(4, 6, SHEET_TEXT, "=1+2", 0, SHEET_OK);
    cell(4, 7, SHEET_TEXT, "'literal", 0, SHEET_OK);
    select_2x2("D4"); key(0x2d, SPREADSHEET_MOD_CTRL);
    cell(3, 3, SHEET_EMPTY, "", 0, SHEET_OK); cell(4, 4, SHEET_EMPTY, "", 0, SHEET_OK);
    key(0x2c, SPREADSHEET_MOD_CTRL);
    cell(3, 3, SHEET_NUMBER, "12.5000", 12500, SHEET_OK);
    jump("J4"); key(0x2f, SPREADSHEET_MOD_CTRL);
    cell(3, 10, SHEET_FORMULA, "=A1*2", 25000, SHEET_OK);
    const char external[] = "7\t=2+3\n+001.5000\t'=literal";
    spreadsheet_clipboard_set(external, sizeof external - 1u); jump("A8"); key(0x2f, SPREADSHEET_MOD_CTRL);
    cell(7, 0, SHEET_NUMBER, "7", 7000, SHEET_OK);
    cell(7, 1, SHEET_TEXT, "=2+3", 0, SHEET_OK);
    cell(8, 0, SHEET_NUMBER, "+001.5000", 1500, SHEET_OK);
    cell(8, 1, SHEET_TEXT, "'=literal", 0, SHEET_OK);
    /* Oversize cells, non-ASCII bytes and out-of-bounds ranges are atomic. */
    select_2x2("A8"); prior = *spreadsheet_document();
    unsigned caret = spreadsheet_caret(), anchor = spreadsheet_anchor();
    char large[SHEET_TEXT_MAX + 2u]; memset(large, 'x', sizeof large);
    spreadsheet_clipboard_set(large, sizeof large); key(0x2f, SPREADSHEET_MOD_CTRL);
    compare_docs(&prior, spreadsheet_document());
    assert(spreadsheet_caret() == caret && spreadsheet_anchor() == anchor);
    spreadsheet_clipboard_set("caf\xc3\xa9", 5); key(0x2f, SPREADSHEET_MOD_CTRL);
    compare_docs(&prior, spreadsheet_document());
    spreadsheet_clipboard_set("", 0); key(0x2f, SPREADSHEET_MOD_CTRL);
    compare_docs(&prior, spreadsheet_document());
    spreadsheet_clipboard_set("1\t2\n3\t4", 7); jump("Z128"); key(0x2f, SPREADSHEET_MOD_CTRL);
    compare_docs(&prior, spreadsheet_document());
    /* Delete range is one ordinary history step. */
    select_2x2("A8"); key(0x53, 0); cell(7, 0, SHEET_EMPTY, "", 0, SHEET_OK);
    cell(8, 1, SHEET_EMPTY, "", 0, SHEET_OK); key(0x2c, SPREADSHEET_MOD_CTRL);
    compare_docs(&prior, spreadsheet_document());
    puts("Spreadsheet clipboard: exact own kinds/sources, unchanged formula references, external literal TSV and atomic rejection passed");
}
static void test_clipboard_edges(void) {
    basic_fixture(); load_fixture(); select_2x2("A1"); key(0x2e, SPREADSHEET_MOD_CTRL);
    unsigned old_generation = clipboard_generation, old_length = clipboard_length;
    prior = *spreadsheet_document();
    clipboard_set_failure = 1;
    jump("B1"); key(0x2d, SPREADSHEET_MOD_CTRL);
    compare_docs(&prior, spreadsheet_document());
    assert(clipboard_generation == old_generation && clipboard_length == old_length);
    key(0x3c, 0); key(0x1e, SPREADSHEET_MOD_CTRL); key(0x2d, SPREADSHEET_MOD_CTRL);
    assert(spreadsheet_editing());
    unsigned length; const unsigned char *draft = spreadsheet_snapshot(&length);
    assert(draft && !sheet_native_decode(&decoded, draft, length)); compare_docs(&prior, &decoded);
    key(0x01, 0); clipboard_set_failure = 0;
    jump("D4"); key(0x2f, SPREADSHEET_MOD_CTRL);
    cell(3, 3, SHEET_NUMBER, "12.5000", 12500, SHEET_OK);
    cell(3, 4, SHEET_FORMULA, "=A1*2", 25000, SHEET_OK);
    cell(4, 3, SHEET_TEXT, "=1+2", 0, SHEET_OK);
    cell(4, 4, SHEET_TEXT, "'literal", 0, SHEET_OK);
    /* Unavailable shared bytes do not grant use of a stale private copy. */
    prior = *spreadsheet_document(); clipboard_get_failure = 1;
    jump("J8"); key(0x2f, SPREADSHEET_MOD_CTRL); compare_docs(&prior, spreadsheet_document());
    clipboard_get_failure = 0;
    /* Embedded tabs/newlines are inside cells; quoting preserves their exact bytes. */
    const char quoted[] = "\"one\ttwo\"\t\"line\nbreak\"\r\n\"quoted\"\"word\"\t=SUM(A1:B1)";
    spreadsheet_clipboard_set(quoted, sizeof quoted - 1); jump("A8"); key(0x2f, SPREADSHEET_MOD_CTRL);
    cell(7, 0, SHEET_TEXT, "one\ttwo", 0, SHEET_OK);
    cell(7, 1, SHEET_TEXT, "line\nbreak", 0, SHEET_OK);
    cell(8, 0, SHEET_TEXT, "quoted\"word", 0, SHEET_OK);
    cell(8, 1, SHEET_TEXT, "=SUM(A1:B1)", 0, SHEET_OK);
    select_2x2("A8"); key(0x2e, SPREADSHEET_MOD_CTRL);
    spreadsheet_clipboard_set((const char *)clipboard, clipboard_length);
    jump("D8"); key(0x2f, SPREADSHEET_MOD_CTRL);
    cell(7, 3, SHEET_TEXT, "one\ttwo", 0, SHEET_OK);
    cell(7, 4, SHEET_TEXT, "line\nbreak", 0, SHEET_OK);
    /* Copy and Cut reject a full long-text grid without replacing prior copy. */
    old_generation = clipboard_generation; old_length = clipboard_length;
    sheet_init(&fixture); char text[96]; memset(text, 'x', 95); text[95] = 0;
    for (unsigned r = 0; r < SHEET_ROWS; r++) for (unsigned c = 0; c < SHEET_COLS; c++)
        assert(!sheet_set(&fixture, r, c, SHEET_TEXT, text, 95));
    assert(!sheet_recalculate(&fixture)); load_fixture(); prior = *spreadsheet_document();
    key(0x1e, SPREADSHEET_MOD_CTRL); key(0x2d, SPREADSHEET_MOD_CTRL);
    compare_docs(&prior, spreadsheet_document());
    assert(clipboard_generation == old_generation && clipboard_length == old_length);
    assert(!spreadsheet_dirty());
    /* A typed cell accepts 95 bytes and visibly rejects an extra byte. */
    spreadsheet_new(); type(text); type("!"); assert(strstr(spreadsheet_status(), "95"));
    key(0x1c, 0); cell(0, 0, SHEET_TEXT, text, 0, SHEET_OK);
    /* Explicit empty TEXT survives a self-owned zero-byte external copy. */
    basic_fixture(); load_fixture(); jump("A3"); key(0x2e, SPREADSHEET_MOD_CTRL);
    assert(!clipboard_length); jump("C3"); key(0x2f, SPREADSHEET_MOD_CTRL);
    cell(2, 2, SHEET_TEXT, "", 0, SHEET_OK);
    puts("Spreadsheet clipboard edges: rejected Cut, private ownership, controls, size bounds and explicit empty text passed");
}
static void dump_file(const char *directory, const char *name, int id) {
    if (!directory) return;
    char path[1024]; int n = snprintf(path, sizeof path, "%s/%s", directory, name);
    assert(n > 0 && (unsigned)n < sizeof path);
    FILE *f = fopen(path, "wb"); assert(f);
    assert(fwrite(fs_data(id), 1, (unsigned)fs_size(id), f) == (unsigned)fs_size(id));
    assert(!fclose(f));
}
static void test_files(const char *directory) {
    spreadsheet_new(); enter("A1", "Item"); enter("B1", "Amount");
    enter("A2", "Rent"); enter("B2", "12.5000");
    enter("A3", "Total"); enter("B3", "=B2*2");
    enter("A4", "'=2+3"); enter("B4", "'007");
    enter("A5", "comma, \"quoted\""); enter("B5", "''literal");
    assert(spreadsheet_save() == SPREADSHEET_SAVE_NEEDS_NAME);
    assert(spreadsheet_save_as(0, "budget.txt") == SPREADSHEET_SAVE_ERROR);
    assert(spreadsheet_save_as(0, "budget.bsh") == SPREADSHEET_SAVE_OK);
    int source = spreadsheet_file(); unsigned identity = spreadsheet_file_identity();
    assert(source > 0 && identity && !spreadsheet_dirty()); prior = *spreadsheet_document();
    assert(!sheet_native_decode(&decoded, (const unsigned char *)fs_data(source), (unsigned)fs_size(source)));
    compare_docs(&prior, &decoded); dump_file(directory, "ui-native.bsh", source);
    assert(spreadsheet_export_csv(0, "wrong.txt") < 0);
    int csv = spreadsheet_export_csv(0, "budget.csv"); assert(csv > 0);
    assert(spreadsheet_file() == source && spreadsheet_file_identity() == identity && !spreadsheet_dirty());
    dump_file(directory, "ui-export.csv", csv);
    assert(spreadsheet_export_csv(0, "budget.csv") < 0);
    assert(spreadsheet_open_file(source)); compare_docs(&prior, spreadsheet_document());
    assert(!spreadsheet_dirty()); enter("B2", "13"); assert(spreadsheet_dirty());
    key(0x2c, SPREADSHEET_MOD_CTRL); assert(!spreadsheet_dirty());
    key(0x15, SPREADSHEET_MOD_CTRL); assert(spreadsheet_dirty());
    cell(2, 1, SHEET_FORMULA, "=B2*2", 26000, SHEET_OK);
    int exported = spreadsheet_export_csv(0, "draft.csv"); assert(exported > 0);
    assert(spreadsheet_dirty() && spreadsheet_file() == source);
    write_failure = 1; prior = *spreadsheet_document();
    assert(spreadsheet_save() == SPREADSHEET_SAVE_ERROR); assert(spreadsheet_dirty());
    assert(spreadsheet_save_as(0, "failed.bsh") == SPREADSHEET_SAVE_ERROR);
    assert(fs_find_child(0, "failed.bsh") < 0); compare_docs(&prior, spreadsheet_document());
    assert(spreadsheet_file() == source && spreadsheet_file_identity() == identity);
    write_failure = 0; sync_failure = 1;
    assert(spreadsheet_save() == SPREADSHEET_SAVE_ERROR && spreadsheet_dirty());
    sync_failure = 0; assert(spreadsheet_save() == SPREADSHEET_SAVE_OK && !spreadsheet_dirty());
    strcpy(files[source].name, "renamed.bsh"); enter("C1", "rename-safe");
    assert(spreadsheet_save() == SPREADSHEET_SAVE_OK); assert(!strcmp(spreadsheet_title(), "renamed.bsh"));
    assert(spreadsheet_file_identity() == identity);
    /* Reusing a deleted numeric node ID never licenses overwriting its new owner. */
    assert(!fs_delete(source)); int reused = make_file("unrelated.bsh", "safe", 4); assert(reused == source);
    enter("C2", "retained draft"); int writes = write_count;
    assert(spreadsheet_save() == SPREADSHEET_SAVE_NEEDS_NAME && write_count == writes);
    assert(spreadsheet_save_as(0, "unrelated.bsh") == SPREADSHEET_SAVE_ERROR);
    assert(fs_size(reused) == 4 && !memcmp(fs_data(reused), "safe", 4));
    prior = *spreadsheet_document(); unsigned caret = spreadsheet_caret(), anchor = spreadsheet_anchor();
    assert(!spreadsheet_open_file(reused)); compare_docs(&prior, spreadsheet_document());
    assert(spreadsheet_caret() == caret && spreadsheet_anchor() == anchor && spreadsheet_dirty());
    int bad_csv = make_file("broken.csv", "\"unterminated", 13);
    assert(!spreadsheet_open_file(bad_csv)); compare_docs(&prior, spreadsheet_document());
    create_failure = 1; assert(spreadsheet_save_as(0, "retry.bsh") == SPREADSHEET_SAVE_ERROR);
    create_failure = 0; assert(spreadsheet_save_as(0, "retry.bsh") == SPREADSHEET_SAVE_OK);
    assert(!spreadsheet_dirty());
    /* CSV is value import, remains unbound, and cannot turn formula-looking text executable. */
    assert(spreadsheet_open_file(csv)); assert(spreadsheet_file() == -1 && spreadsheet_dirty());
    cell(1, 1, SHEET_NUMBER, "12.5", 12500, SHEET_OK);
    cell(2, 1, SHEET_NUMBER, "25", 25000, SHEET_OK);
    cell(3, 0, SHEET_TEXT, "=2+3", 0, SHEET_OK);
    cell(3, 1, SHEET_NUMBER, "007", 7000, SHEET_OK);
    assert(spreadsheet_save() == SPREADSHEET_SAVE_NEEDS_NAME);
    spreadsheet_new(); enter("A1", "longer than tiny filesystem limit"); file_limit = 20;
    assert(spreadsheet_save_as(0, "limit.bsh") == SPREADSHEET_SAVE_ERROR);
    assert(fs_find_child(0, "limit.bsh") < 0); file_limit = FS_FILE_MAX;
    puts("Spreadsheet files: native round trip, calculated CSV, safe import, renamed/reused IDs and write/sync failures passed");
}
static void test_bindings(void) {
    basic_fixture(); int source = load_fixture(); unsigned identity = spreadsheet_file_identity();
    SpreadsheetBinding baseline, current; assert(spreadsheet_binding(&baseline));
    assert(baseline.size == (unsigned)fs_size(source)); assert(spreadsheet_binding_matches(source, &baseline));
    int duplicate = make_file("duplicate.bsh", fs_data(source), (unsigned)fs_size(source));
    assert(fs_identity(duplicate) != identity && spreadsheet_binding_matches(duplicate, &baseline));
    strcpy(files[duplicate].name, "duplicate.csv"); assert(!spreadsheet_binding_matches(duplicate, &baseline));
    prior = *spreadsheet_document();
    assert(!sheet_set(&fixture, 0, 0, SHEET_NUMBER, "99.0000", 7));
    unsigned length; assert(!sheet_native_encode(&fixture, encoded, sizeof encoded, &length));
    assert(length == baseline.size); assert(fs_write(source, (const char *)encoded, (int)length) == (int)length);
    assert(fs_identity(source) == identity && !spreadsheet_binding_matches(source, &baseline));
    int writes = write_count;
    assert(spreadsheet_save() == SPREADSHEET_SAVE_NEEDS_NAME && write_count == writes);
    assert(spreadsheet_save_as(0, "fixture.bsh") == SPREADSHEET_SAVE_NEEDS_NAME && write_count == writes);
    compare_docs(&prior, spreadsheet_document()); assert(spreadsheet_dirty());
    assert(!memcmp(fs_data(source), encoded, length));
    assert(spreadsheet_save_as(0, "separate.bsh") == SPREADSHEET_SAVE_OK); source = spreadsheet_file();
    assert(spreadsheet_binding(&current) && !memcmp(&baseline, &current, sizeof baseline));
    enter("A1", "2"); write_failure = 1;
    assert(spreadsheet_save() == SPREADSHEET_SAVE_ERROR && spreadsheet_dirty());
    assert(spreadsheet_binding(&current) && !memcmp(&baseline, &current, sizeof baseline));
    assert(spreadsheet_binding_matches(source, &baseline));
    write_failure = 0; sync_failure = 1;
    assert(spreadsheet_save() == SPREADSHEET_SAVE_ERROR && spreadsheet_dirty());
    assert(spreadsheet_binding(&current)); assert(spreadsheet_binding_matches(source, &current));
    assert(!spreadsheet_binding_matches(source, &baseline));
    sync_failure = 0; assert(spreadsheet_save() == SPREADSHEET_SAVE_OK && !spreadsheet_dirty());
    spreadsheet_new(); current = baseline; assert(!spreadsheet_binding(&current));
    assert(!memcmp(&baseline, &current, sizeof baseline)); assert(!spreadsheet_binding_matches(source, NULL));
    puts("Spreadsheet bindings: same-ID/same-size external replacement, byte fingerprints and safe sync retry passed");
}
static void test_recovery(void) {
    basic_fixture(); int source = load_fixture(); unsigned identity = spreadsheet_file_identity();
    SpreadsheetBinding baseline, current; assert(spreadsheet_binding(&baseline));
    jump("A2"); key(0x3c, 0); key(0x4f, 0); type("0");
    assert(spreadsheet_editing()); prior = *spreadsheet_document();
    unsigned length; const unsigned char *snapshot = spreadsheet_snapshot(&length); assert(snapshot && length);
    assert(spreadsheet_editing()); compare_docs(&prior, spreadsheet_document());
    assert(!sheet_native_decode(&decoded, snapshot, length));
    assert(decoded.cells[SHEET_COLS].kind == SHEET_TEXT && !strcmp(decoded.cells[SHEET_COLS].text, "=1+20"));
    assert(spreadsheet_binding(&current) && !memcmp(&baseline, &current, sizeof baseline));
    unsigned char *saved = malloc(length); assert(saved); memcpy(saved, snapshot, length);
    /* Recovery snapshots may feed restore directly without invalidating borrowed bytes. */
    assert(spreadsheet_restore(snapshot, length, source, identity, 1, 54, 27));
    assert(!spreadsheet_editing() && spreadsheet_dirty()); compare_docs(&decoded, spreadsheet_document());
    assert(spreadsheet_file() == source && spreadsheet_file_identity() == identity);
    assert(spreadsheet_caret() == 54 && spreadsheet_anchor() == 27);
    assert(spreadsheet_binding(&current) && !memcmp(&baseline, &current, sizeof baseline));
    prior = *spreadsheet_document(); assert(!spreadsheet_restore(saved, length - 1u, source, identity, 0, 0, 0));
    compare_docs(&prior, spreadsheet_document()); assert(spreadsheet_caret() == 54 && spreadsheet_anchor() == 27);
    spreadsheet_new(); assert(spreadsheet_restore(saved, length, source, identity, 1, 3, 1));
    compare_docs(&prior, spreadsheet_document()); assert(spreadsheet_dirty());
    assert(spreadsheet_save() == SPREADSHEET_SAVE_OK && !spreadsheet_dirty());
    assert(!spreadsheet_binding_matches(source, &baseline));
    snapshot = spreadsheet_snapshot(&length);
    assert(spreadsheet_restore(snapshot, length, source, identity, 0, 0, 0)); assert(!spreadsheet_dirty());
    int csv = make_file("recovery.csv", "1,2\r\n", 5);
    assert(spreadsheet_restore(saved, length, csv, fs_identity(csv), 0, SHEET_CELLS + 8u, SHEET_CELLS + 9u));
    assert(spreadsheet_file() == -1 && spreadsheet_dirty());
    assert(spreadsheet_caret() < SHEET_CELLS && spreadsheet_anchor() < SHEET_CELLS);
    assert(fs_size(csv) == 5 && !memcmp(fs_data(csv), "1,2\r\n", 5));
    assert(spreadsheet_restore(saved, length, source, identity + 100u, 0, 0, 0));
    assert(spreadsheet_file() == -1 && spreadsheet_dirty());
    free(saved); spreadsheet_close(); spreadsheet_init();
    cell(0, 0, SHEET_EMPTY, "", 0, SHEET_OK); assert(!spreadsheet_dirty());
    puts("Spreadsheet recovery: non-disruptive edit snapshots, aliased restore, selection, baseline and unbound recovery passed");
}
static void check_clip(int x, int y, int w, int h) {
    memset(back, COLOR_MAGENTA, sizeof back); spreadsheet_draw(x, y, w, h);
    for (int py = 0; py < 600; py++) for (int px = 0; px < 800; px++)
        if (px < x || px >= x+w || py < y || py >= y+h)
            assert(back[py * 800 + px] == COLOR_MAGENTA);
}
static void write_ppm(const char *directory) {
    if (!directory) return;
    char path[1024]; int n = snprintf(path, sizeof path, "%s/ui-preview.ppm", directory);
    assert(n > 0 && (unsigned)n < sizeof path);
    FILE *f = fopen(path, "wb"); assert(f); fprintf(f, "P6\n800 600\n255\n");
    for (unsigned i = 0; i < sizeof back; i++) {
        unsigned rgb = pal32[back[i]];
        unsigned char pixel[3] = {(unsigned char)(rgb >> 16), (unsigned char)(rgb >> 8), (unsigned char)rgb};
        assert(fwrite(pixel, 1, 3, f) == 3);
    }
    assert(!fclose(f));
}
static void test_drawing(const char *directory) {
    basic_fixture(); load_fixture();
    enter("C1", "long text to clip inside one cell"); enter("D2", "=1/0");
    const int boxes[][4] = {{30,20,720,520}, {30,20,420,260}, {-12,-10,420,260},
        {730,540,420,260}, {30,20,160,120}, {30,20,1,1}, {30,20,0,0}, {-900,-800,420,260}};
    for (unsigned i = 0; i < sizeof boxes / sizeof boxes[0]; i++) {
        int x = boxes[i][0], y = boxes[i][1], w = boxes[i][2], h = boxes[i][3];
        select_2x2("A1"); check_clip(x,y,w,h);
        jump("A2"); key(0x3c,0); type("editing"); check_clip(x,y,w,h); key(0x01,0);
        key(0x22, SPREADSHEET_MOD_CTRL); check_clip(x,y,w,h); key(0x01,0);
    }
    jump("A1"); spreadsheet_draw(30,20,720,520);
    int x, y, w, h, x2, y2, w2, h2;
    assert(spreadsheet_cell_position(0,0,&x,&y,&w,&h)); assert(w > 0 && h > 0);
    assert(x >= 0 && y >= 0 && x + w <= 720 && y + h <= 520);
    assert(spreadsheet_cell_position(2,2,&x2,&y2,&w2,&h2));
    assert(spreadsheet_click(30,20,720,520,30+x+w/2,20+y+h/2,0) & SPREADSHEET_CHANGED);
    assert(spreadsheet_caret() == 0 && spreadsheet_anchor() == 0);
    assert(spreadsheet_drag(30,20,720,520,30+x2+w2/2,20+y2+h2/2) & SPREADSHEET_CHANGED);
    assert(spreadsheet_caret() == 54 && spreadsheet_anchor() == 0); spreadsheet_release();
    assert(!spreadsheet_drag(30,20,720,520,30+x+w/2,20+y+h/2));
    assert(spreadsheet_click(30,20,720,520,30+x+w/2,20+y+h/2,SPREADSHEET_MOD_SHIFT) & SPREADSHEET_CHANGED);
    assert(spreadsheet_anchor() == 0 && spreadsheet_caret() == 0); spreadsheet_release();
    jump("Z128"); spreadsheet_draw(30,20,420,260);
    assert(spreadsheet_first_row() > 0 && spreadsheet_first_col() > 0);
    assert(spreadsheet_cell_position(127,25,&x,&y,&w,&h));
    assert(!spreadsheet_cell_position(0,0,NULL,NULL,NULL,NULL));
    spreadsheet_scroll(-1000); spreadsheet_draw(30,20,420,260); assert(spreadsheet_first_row() == 0);
    jump("A1"); spreadsheet_draw(30,20,720,520); assert(spreadsheet_first_row() == 0 && spreadsheet_first_col() == 0);
    ticks += 71; (void)spreadsheet_tick();
    memset(back, COLOR_LTGRAY, sizeof back); select_2x2("A1"); spreadsheet_draw(30,20,720,520); write_ppm(directory);
    puts("Spreadsheet rendering: every-pixel client clipping, normal/minimum/small/offscreen, address/edit fields and mouse range passed");
}
static void test_storage_busy(void) {
    basic_fixture(); int id = load_fixture();
    unsigned identity = spreadsheet_file_identity();
    SpreadsheetBinding before, after; assert(spreadsheet_binding(&before));
    int writes_before = write_count, syncs = sync_count;
    storage_busy = 1;
    assert(spreadsheet_save() == SPREADSHEET_SAVE_ERROR && !spreadsheet_dirty());
    assert(spreadsheet_save_as(0, "lease.bsh") == SPREADSHEET_SAVE_ERROR);
    assert(spreadsheet_export_csv(0, "lease.csv") < 0);
    assert(strstr(spreadsheet_status(), "Disk is saving; retry shortly"));
    assert(fs_find_child(0, "lease.bsh") < 0 && fs_find_child(0, "lease.csv") < 0);
    assert(write_count == writes_before && sync_count == syncs && !spreadsheet_dirty());
    assert(spreadsheet_file() == id && spreadsheet_file_identity() == identity);
    assert(spreadsheet_binding(&after) && !memcmp(&before, &after, sizeof before));

    /* Busy Save/Save As/Export must not commit or dismiss a pending cell edit. */
    prior = *spreadsheet_document(); type("77");
    assert(spreadsheet_dirty() && spreadsheet_editing());
    unsigned caret = spreadsheet_caret(), anchor = spreadsheet_anchor();
    assert(spreadsheet_save() == SPREADSHEET_SAVE_ERROR);
    assert(spreadsheet_save_as(0, "lease.bsh") == SPREADSHEET_SAVE_ERROR);
    assert(spreadsheet_export_csv(0, "lease.csv") < 0);
    assert(spreadsheet_editing() && spreadsheet_dirty()); compare_docs(&prior, spreadsheet_document());
    assert(spreadsheet_caret() == caret && spreadsheet_anchor() == anchor);
    assert(spreadsheet_binding(&after) && !memcmp(&before, &after, sizeof before));
    assert(write_count == writes_before && sync_count == syncs);
    unsigned length; const unsigned char *draft = spreadsheet_snapshot(&length);
    assert(draft && !sheet_native_decode(&decoded, draft, length));
    assert(!strcmp(sheet_cell(&decoded, 0, 0)->text, "77"));
    type("8"); assert(spreadsheet_editing());
    storage_busy = 0;
    assert(spreadsheet_save() == SPREADSHEET_SAVE_OK && !spreadsheet_dirty() && !spreadsheet_editing());
    cell(0, 0, SHEET_NUMBER, "778", 778000, SHEET_OK);
    assert(spreadsheet_file() == id && spreadsheet_file_identity() == identity);

    enter("A1", "42"); sync_failure = 1;
    assert(spreadsheet_save_as(0, "lease.bsh") == SPREADSHEET_SAVE_ERROR);
    int owned = spreadsheet_file(); unsigned owned_identity = spreadsheet_file_identity();
    assert(owned != id && spreadsheet_dirty() && spreadsheet_binding(&before));
    writes_before = write_count; syncs = sync_count; storage_busy = 1;
    assert(spreadsheet_save() == SPREADSHEET_SAVE_ERROR);
    assert(spreadsheet_save_as(0, "lease.bsh") == SPREADSHEET_SAVE_ERROR);
    assert(spreadsheet_dirty() && spreadsheet_file() == owned && spreadsheet_file_identity() == owned_identity);
    assert(spreadsheet_binding(&after) && !memcmp(&before, &after, sizeof before));
    assert(write_count == writes_before && sync_count == syncs);
    storage_busy = sync_failure = 0;
    assert(spreadsheet_save_as(0, "lease.bsh") == SPREADSHEET_SAVE_OK && !spreadsheet_dirty());
    assert(spreadsheet_file() == owned && spreadsheet_file_identity() == owned_identity);
    assert(fs_write(owned, "external", 8) == 8);
    assert(spreadsheet_save() == SPREADSHEET_SAVE_NEEDS_NAME && strstr(spreadsheet_status(), "changed outside"));
    assert(!memcmp(fs_data(owned), "external", 8));
    assert(spreadsheet_open_file(id)); write_failure = 1;
    assert(spreadsheet_save() == SPREADSHEET_SAVE_ERROR && strstr(spreadsheet_status(), "Save failed"));
    write_failure = 0;
    assert(spreadsheet_save() == SPREADSHEET_SAVE_OK && !spreadsheet_dirty());
    assert(!fs_delete(owned));
    puts("Spreadsheet storage lease: clean/dirty bindings, pending cell edits, safe save retry and honest errors passed");
}
int main(int argc, char **argv) {
    files[0].valid = files[0].folder = 1; files[0].identity = 1;
    gfx_init(back, linear, 800, 600, 32, 3200);
    test_storage_busy(); spreadsheet_close();
    test_editing(); test_navigation(); test_history(); test_clipboard(); test_clipboard_edges();
    test_files(argc > 1 ? argv[1] : NULL); test_bindings(); test_recovery();
    test_drawing(argc > 1 ? argv[1] : NULL);
    assert(polls > 100 && sync_count >= 4);
    for (int i = 1; i < FILE_COUNT; i++) if (files[i].valid) free(files[i].data);
    puts("All Spreadsheet UI host functional checks passed."); return 0;
}
