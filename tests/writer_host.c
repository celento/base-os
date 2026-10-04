/* Deterministic ordinary Writer workflows, graphics and persistence outcomes. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "writer.h"
#include "layout.h"
static unsigned char presented[FB_CAPACITY];
#undef PRESENT_BASE
#define PRESENT_BASE ((uintptr_t)presented)
#define GFX_HOST_TEST
#include "../src/gfx.c"

static unsigned polls, ticks, clipboard_generation, clipboard_length;
static unsigned char clipboard[WRITER_TEXT_MAX + 1];
static int write_failure, sync_failure, sync_count, write_count;
static unsigned next_identity = 1;
static unsigned file_limit = 2097152;
typedef struct { int valid, folder, size; unsigned identity; char name[FS_NAME_LEN]; unsigned char *data; } File;
static File files[20];
static unsigned char back[800 * 600], linear[800 * 600 * 4];
static WriterDoc fixture, prior;
static unsigned char encoded[WRITER_NATIVE_MAX_SIZE];
void kmemset(void *d, int value, int count) { memset(d, value, (size_t)count); }
void kmemcpy(void *d, const void *s, int count) { memcpy(d, s, (size_t)count); }
void platform_poll(void) { polls++; }
unsigned timer_ticks(void) { return ticks; }
unsigned writer_clipboard_set(const char *text, unsigned length) {
    assert(length <= WRITER_TEXT_MAX); memcpy(clipboard, text, length); clipboard[length] = 0;
    clipboard_length = length; return ++clipboard_generation;
}
int writer_clipboard_get(char *text, unsigned capacity, unsigned *generation) {
    if (clipboard_length >= capacity) return -1;
    memcpy(text, clipboard, clipboard_length + 1); *generation = clipboard_generation;
    return (int)clipboard_length;
}
int fs_valid(int id) { return id >= 0 && id < 20 && files[id].valid; }
int fs_is_dir(int id) { return fs_valid(id) && files[id].folder; }
int fs_is_app(int id) { (void)id; return 0; }
unsigned fs_identity(int id) { return fs_valid(id) ? files[id].identity : 0; }
int fs_size(int id) { return fs_valid(id) ? files[id].size : -1; }
const char *fs_name(int id) { return fs_valid(id) ? files[id].name : ""; }
const char *fs_data(int id) { return fs_valid(id) ? (const char *)files[id].data : NULL; }
int fs_find_child(int parent, const char *name) {
    if (parent != 0 || !name) return -1;
    for (int i = 1; i < 20; i++) if (files[i].valid && !strcmp(name, files[i].name)) return i;
    return -1;
}
int fs_create(int parent, const char *name) {
    if (write_failure || parent != 0 || !name || !*name || strlen(name) >= FS_NAME_LEN || strchr(name, '/') || fs_find_child(parent, name) >= 0) return -1;
    for (int i = 1; i < 20; i++) if (!files[i].valid) {
        files[i].valid = 1; files[i].identity = ++next_identity; files[i].size = 0;
        files[i].data = malloc(1); assert(files[i].data); files[i].data[0] = 0;
        strcpy(files[i].name, name); return i;
    }
    return -1;
}
int fs_write(int id, const char *text, int length) {
    write_count++;
    if (write_failure || !fs_valid(id) || files[id].folder || length < 0 || length > 2097152) return -1;
    unsigned char *data = malloc((unsigned)length + 1); assert(data);
    memcpy(data, text, (unsigned)length); data[length] = 0;
    free(files[id].data); files[id].data = data; files[id].size = length;
    return length;
}
int fs_delete(int id) {
    if (!fs_valid(id) || files[id].folder) return -1;
    free(files[id].data); memset(&files[id], 0, sizeof files[id]); return 0;
}
unsigned fs_file_limit(void) { return file_limit; }
int fs_sync(void) { sync_count++; return sync_failure ? -1 : 0; }
static void key(int sc, int modifiers) { assert(writer_key(sc, 0, modifiers) & WRITER_CHANGED); }
static void type(const char *s) { while (*s) assert(writer_key(0, *s++, 0) & WRITER_CHANGED); }
static void select_range(unsigned lo, unsigned hi) {
    key(0x47, WRITER_MOD_CTRL);
    for (unsigned i = 0; i < lo; i++) key(0x4d, 0);
    for (unsigned i = lo; i < hi; i++) key(0x4d, WRITER_MOD_SHIFT);
    assert(writer_anchor() == lo && writer_caret() == hi);
}
static void check_text(const char *text) {
    const WriterDoc *d = writer_document();
    assert(d->length == strlen(text)); assert(!memcmp(d->text, text, d->length));
    assert(!writer_doc_validate(d));
}
static void compare_docs(const WriterDoc *a, const WriterDoc *b) {
    assert(a->length == b->length);
    assert(!memcmp(a->text, b->text, a->length));
    assert(!memcmp(a->style, b->style, a->length + 1));
    assert(!memcmp(a->paragraph, b->paragraph, a->length + 1));
}
static void load_fixture(const char *text) {
    assert(!writer_plain_import(&fixture, (const unsigned char *)text, (unsigned)strlen(text)));
    unsigned n; assert(!writer_native_encode(&fixture, encoded, sizeof encoded, &n));
    int id = fs_find_child(0, "fixture.bwr"); if (id < 0) id = fs_create(0, "fixture.bwr");
    assert(fs_write(id, (const char *)encoded, (int)n) >= 0); assert(writer_open_file(id));
}
static void test_editing(void) {
    assert(!writer_tick()); writer_release(); assert(!writer_dirty());
    writer_init(); assert(!writer_dirty());
    type("Hello world"); check_text("Hello world"); assert(writer_dirty());
    select_range(6, 11); key(0x30, WRITER_MOD_CTRL); key(0x17, WRITER_MOD_CTRL); key(0x16, WRITER_MOD_CTRL);
    for (int i = 0; i < 11; i++) assert(writer_document()->style[i] == (i >= 6 ? 7 : 0));
    key(0x02, WRITER_MOD_CTRL); key(0x12, WRITER_MOD_CTRL);
    assert(writer_document()->paragraph[0] == (WRITER_PARAGRAPH_HEADING | WRITER_ALIGN_CENTER));
    key(0x2e, WRITER_MOD_CTRL); writer_new(); key(0x2f, WRITER_MOD_CTRL);
    check_text("world"); assert(writer_document()->style[0] == 7);
    assert(writer_document()->paragraph[0] == 5);
    /* Same bytes with a different clipboard owner must lose the Writer styles. */
    writer_clipboard_set("world", 5); writer_new(); key(0x2f, WRITER_MOD_CTRL);
    check_text("world"); assert(!writer_document()->style[0]); assert(!writer_document()->paragraph[0]);
    /* Unsupported clipboard content is rejected before replacing selection. */
    writer_clipboard_set("caf\xc3\xa9", 5); key(0x1e, WRITER_MOD_CTRL);
    prior = *writer_document(); key(0x2f, WRITER_MOD_CTRL); compare_docs(&prior, writer_document());
    assert(writer_caret() == 5 && writer_anchor() == 0);
    /* Multiple paragraphs retain canonical attributes through deletion/joins. */
    load_fixture("Alpha\nBeta\nGamma\n");
    select_range(6, 10); key(0x13, WRITER_MOD_CTRL); key(0x02, WRITER_MOD_CTRL);
    assert(writer_document()->paragraph[6] == 6);
    select_range(2, 8); type("X"); check_text("AlXta\nGamma\n");
    assert(!writer_document()->paragraph[0]);
    select_range(0, 0); key(0x1c, 0); check_text("\nAlXta\nGamma\n");
    key(0x0e, 0); check_text("AlXta\nGamma\n");
    key(0x4f, WRITER_MOD_CTRL); key(0x0e, 0); check_text("AlXta\nGamma");
    key(0x0f, 0); check_text("AlXta\nGamma\t");
    puts("Writer editing: inline/paragraph formatting, rich/plain clipboard, validated replacement passed");
}
static void test_history(void) {
    writer_new(); type("abcdefghijkl");
    for (int i = 0; i < 8; i++) key(0x2c, WRITER_MOD_CTRL);
    check_text("abcd"); key(0x2c, WRITER_MOD_CTRL); check_text("abcd");
    for (int i = 0; i < 8; i++) key(0x15, WRITER_MOD_CTRL);
    check_text("abcdefghijkl"); key(0x2c, WRITER_MOD_CTRL); type("Z");
    check_text("abcdefghijkZ"); key(0x15, WRITER_MOD_CTRL); check_text("abcdefghijkZ");
    key(0x2c, WRITER_MOD_CTRL | WRITER_MOD_SHIFT); check_text("abcdefghijkZ");
    puts("Writer history: eight-step bound, redo, branching and alternate shortcut passed");
}
static void test_files(void) {
    writer_new(); type("Saved document"); key(0x1e, WRITER_MOD_CTRL); key(0x30, WRITER_MOD_CTRL);
    assert(writer_save() == WRITER_SAVE_NEEDS_NAME);
    assert(writer_save_as(0, "notes.txt") == WRITER_SAVE_ERROR);
    assert(writer_save_as(0, "notes.bwr") == WRITER_SAVE_OK);
    int id = writer_file(); unsigned identity = writer_file_identity();
    assert(id > 0 && identity && !writer_dirty()); prior = *writer_document();
    key(0x4f, WRITER_MOD_CTRL); type("!"); assert(writer_dirty());
    key(0x2c, WRITER_MOD_CTRL); assert(!writer_dirty());
    key(0x15, WRITER_MOD_CTRL); assert(writer_dirty());
    sync_failure = 1; assert(writer_save() == WRITER_SAVE_ERROR); assert(writer_dirty());
    sync_failure = 0; assert(writer_save() == WRITER_SAVE_OK); assert(!writer_dirty());
    assert(writer_file() == id && writer_file_identity() == identity);
    /* Renames preserve stable binding. Reused numeric node IDs do not. */
    strcpy(files[id].name, "renamed.bwr"); type("?"); assert(writer_save() == WRITER_SAVE_OK);
    assert(!strcmp(writer_title(), "renamed.bwr"));
    assert(fs_delete(id) == 0); assert(writer_dirty()); int reused = fs_create(0, "unrelated.bwr"); assert(reused == id);
    assert(fs_write(reused, "safe", 4) == 4); type("x");
    int writes = write_count; assert(writer_save() == WRITER_SAVE_NEEDS_NAME); assert(write_count == writes);
    assert(writer_save_as(0, "unrelated.bwr") == WRITER_SAVE_ERROR);
    assert(fs_size(reused) == 4 && !memcmp(fs_data(reused), "safe", 4));
    /* Failed writes and malformed opens leave the entire current document. */
    prior = *writer_document();
    assert(!writer_open_file(reused)); compare_docs(&prior, writer_document());
    write_failure = 1; assert(writer_save_as(0, "retry.bwr") == WRITER_SAVE_ERROR); write_failure = 0;
    compare_docs(&prior, writer_document()); assert(writer_dirty());
    assert(writer_save_as(0, "retry.bwr") == WRITER_SAVE_OK); assert(!writer_dirty());
    unsigned length; const unsigned char *draft = writer_snapshot(&length); assert(draft && length);
    unsigned char *saved = malloc(length); assert(saved); memcpy(saved, draft, length);
    int bound = writer_file(); unsigned bound_identity = writer_file_identity();
    writer_new(); assert(writer_restore(saved, length, bound, bound_identity, 1, 5, 2));
    compare_docs(&prior, writer_document()); assert(writer_dirty() && writer_caret() == 5 && writer_anchor() == 2);
    assert(writer_file() == bound);
    /* Snapshot's borrowed output itself can safely feed restore through staging. */
    draft = writer_snapshot(&length); assert(writer_restore(draft, length, bound, bound_identity, 0, 0, 0));
    assert(!writer_dirty());
    assert(writer_export_rtf(0, "wrong.txt") < 0);
    assert(writer_export_rtf(0, "notes.rtf") > 0); assert(!writer_dirty());
    assert(writer_export_rtf(0, "notes.rtf") < 0);
    int plain = fs_create(0, "plain.txt"); assert(fs_write(plain, "plain\ntext", 10) == 10);
    assert(writer_open_file(plain)); check_text("plain\ntext"); assert(writer_file() == -1 && writer_dirty());
    assert(writer_save() == WRITER_SAVE_NEEDS_NAME);
    int bad = fs_create(0, "binary.txt"); assert(fs_write(bad, "A\x01" "B", 3) == 3);
    prior = *writer_document(); assert(!writer_open_file(bad)); compare_docs(&prior, writer_document());
    assert(writer_restore(saved, length, 19, 1, 0, 999, 999)); assert(writer_file() == -1 && writer_dirty());
    free(saved);
    writer_new(); type("limit"); file_limit = 20;
    assert(writer_save_as(0, "limit.bwr") == WRITER_SAVE_ERROR);
    assert(fs_find_child(0, "limit.bwr") < 0); file_limit = 2097152;
    writer_close(); writer_init(); assert(!writer_dirty() && !writer_length());
    puts("Writer files: round trip, identities, reused nodes, failed sync/write, safe import and recovery passed");
}
static void test_layout(void) {
    load_fixture("iiii WWWW and words\ncenter\nright\n");
    writer_layout(90); assert(writer_line_count() > 4);
    int x0, x1, y0, y1, h;
    assert(writer_position(0, &x0, &y0, &h)); assert(writer_position(4, &x1, &y1, NULL));
    assert(y0 == y1 && x1 - x0 == 4 * ui_advance('i'));
    assert(writer_hit_position(x1, y1) == 4);
    select_range(20, 26); key(0x12, WRITER_MOD_CTRL);
    writer_layout(300); assert(writer_position(20, &x1, &y1, &h));
    assert(x1 > 80); assert(writer_hit_position(x1, y1) == 20);
    select_range(27, 32); key(0x13, WRITER_MOD_CTRL); key(0x02, WRITER_MOD_CTRL);
    writer_layout(300); assert(writer_position(27, &x1, &y1, &h)); assert(h == 38 && x1 > 200);
    assert(writer_hit_position(x1, y1) == 27);
    key(0x47, WRITER_MOD_CTRL); key(0x50, WRITER_MOD_SHIFT); assert(writer_caret() > 0 && writer_anchor() == 0);
    key(0x51, 0); key(0x49, 0); key(0x4f, WRITER_MOD_CTRL); assert(writer_caret() == writer_length());
    /* At maximum text capacity, one LF per byte exercises every line slot. */
    memset(clipboard, '\n', WRITER_TEXT_MAX); clipboard_length = WRITER_TEXT_MAX; clipboard[WRITER_TEXT_MAX] = 0; clipboard_generation++;
    writer_new(); key(0x2f, WRITER_MOD_CTRL); assert(writer_length() == WRITER_TEXT_MAX);
    writer_layout(90); assert(writer_line_count() == WRITER_TEXT_MAX + 1);
    assert(writer_position(WRITER_TEXT_MAX, &x1, &y1, &h)); assert(y1 == (int)WRITER_TEXT_MAX * 24);
    type("x"); assert(writer_length() == WRITER_TEXT_MAX); assert(!writer_doc_validate(writer_document()));
    load_fixture("one two three four five six seven");
    writer_layout(55); key(0x47, WRITER_MOD_CTRL); key(0x4f, 0);
    assert(writer_position(writer_caret(), &x1, &y1, &h)); assert(y1 == 0 && x1 > 0);
    key(0x50, 0); assert(writer_position(writer_caret(), &x1, &y1, &h)); assert(y1 == 24);
    key(0x48, 0); assert(writer_position(writer_caret(), &x1, &y1, &h)); assert(y1 == 0);
    puts("Writer geometry: proportional wrapping, alignment, headings, navigation and max-capacity lines passed");
}
static void write_ppm(const char *path) {
    FILE *f = fopen(path, "wb"); assert(f); fprintf(f, "P6\n800 600\n255\n");
    for (unsigned i = 0; i < sizeof back; i++) {
        unsigned rgb = pal32[back[i]];
        unsigned char pixel[3] = {(unsigned char)(rgb >> 16), (unsigned char)(rgb >> 8), (unsigned char)rgb};
        assert(fwrite(pixel, 1, 3, f) == 3);
    }
    fclose(f);
}
static void test_drawing(const char *path) {
    load_fixture("A real document\nA small Writer with proportional wrapping, bold emphasis and a real editing model.\n\nSelect text and choose a style. Paragraphs can be left aligned, centered or right aligned.\n\nNative .bwr preserves formatting. Export RTF for another word processor.\n");
    select_range(0, 15); key(0x02, WRITER_MOD_CTRL); key(0x30, WRITER_MOD_CTRL);
    select_range(65, 78); key(0x17, WRITER_MOD_CTRL); key(0x16, WRITER_MOD_CTRL);
    select_range(17, 31);
    const int boxes[][4] = {{30,20,720,520},{30,20,420,260},{-12,-10,420,260},{730,540,420,260},{30,20,160,120}};
    for (unsigned b = 0; b < sizeof boxes / sizeof boxes[0]; b++) {
        int x=boxes[b][0], y=boxes[b][1], w=boxes[b][2], h=boxes[b][3];
        memset(back, COLOR_MAGENTA, sizeof back); writer_draw(x,y,w,h);
        for (int py = 0; py < 600; py++) for (int px = 0; px < 800; px++)
            if (px < x || px >= x+w || py < y || py >= y+h) assert(back[py*800+px] == COLOR_MAGENTA);
    }
    memset(back, COLOR_LTGRAY, sizeof back); writer_draw(30,20,720,520);
    /* Shared toolbar geometry: B click toggles current selection, no text hit. */
    unsigned before = writer_document()->style[17];
    assert(writer_click(30,20,720,520,46,34,0) & WRITER_CHANGED);
    assert(writer_document()->style[17] != before);
    writer_release();
    assert(writer_click(30,20,720,520,60,100,0) & WRITER_CHANGED);
    unsigned anchor = writer_anchor(); assert(writer_drag(30,20,720,520,180,130) & WRITER_CHANGED);
    assert(writer_anchor() == anchor && writer_caret() != anchor); writer_release();
    assert(!writer_drag(30,20,720,520,200,180));
    if (path) { select_range(17,31); writer_draw(30,20,720,520); write_ppm(path); }
    puts("Writer rendering: normal/minimum/offscreen clipping, toolbar hits and mouse selection passed");
}
int main(int argc, char **argv) {
    files[0].valid = files[0].folder = 1; files[0].identity = 1;
    gfx_init(back, linear, 800, 600, 32, 3200);
    test_editing(); test_history(); test_files(); test_layout(); test_drawing(argc > 1 ? argv[1] : NULL);
    assert(polls > 100); assert(sync_count >= 4);
    for (int i = 1; i < 20; i++) if (files[i].valid) free(files[i].data);
    puts("All Writer host functional checks passed.");
}
