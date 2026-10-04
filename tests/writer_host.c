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
static int write_failure, payload_failure, sync_failure, sync_count, write_count;
static int create_count, delete_count, node_limit = 20;
static unsigned storage_limit = 8u * 1024u * 1024u, node_cost;
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
    create_count++;
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
    if (write_failure || payload_failure || !fs_valid(id) || files[id].folder || length < 0 || (unsigned)length > file_limit) return -1;
    unsigned char *data = malloc((unsigned)length + 1); assert(data);
    memcpy(data, text, (unsigned)length); data[length] = 0;
    free(files[id].data); files[id].data = data; files[id].size = length;
    return length;
}
int fs_delete(int id) {
    delete_count++;
    if (!fs_valid(id) || files[id].folder) return -1;
    free(files[id].data); memset(&files[id], 0, sizeof files[id]); return 0;
}
unsigned fs_file_limit(void) { return file_limit; }
int fs_node_count(void) { int count = 0; for (int i = 0; i < 20; i++) count += files[i].valid != 0; return count; }
int fs_node_limit(void) { return node_limit; }
unsigned fs_capacity_for_nodes(unsigned count) { return count > (unsigned)node_limit || count * node_cost > storage_limit ? 0 : storage_limit - count * node_cost; }
unsigned fs_used_bytes(void) { unsigned used = 0; for (int i = 0; i < 20; i++) if (files[i].valid) used += (unsigned)files[i].size; return used; }
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
    if (d->length != strlen(text) || memcmp(d->text, text, d->length < strlen(text) ? d->length : strlen(text))) fprintf(stderr, "Expected [%s], got [%.*s] length %u: %s\n", text, (int)d->length, d->text, d->length, writer_status());
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
    type("!"); check_text("world!"); assert(writer_document()->style[5] == 7);
    /* Same bytes with a different clipboard owner must lose the Writer styles. */
    writer_clipboard_set("world", 5); writer_new(); key(0x2f, WRITER_MOD_CTRL);
    check_text("world"); assert(!writer_document()->style[0]); assert(!writer_document()->paragraph[0]);
    /* Unsupported clipboard content is rejected before replacing selection. */
    writer_clipboard_set("caf\xc3\xa9", 5); key(0x1e, WRITER_MOD_CTRL);
    prior = *writer_document(); key(0x2f, WRITER_MOD_CTRL); compare_docs(&prior, writer_document());
    assert(writer_caret() == 5 && writer_anchor() == 0);
    /* Empty shared text is a no-op, even with a nonempty document selection. */
    load_fixture("Keep selection"); select_range(5,14); prior = *writer_document();
    writer_clipboard_set("",0); key(0x2f,WRITER_MOD_CTRL);
    compare_docs(&prior,writer_document()); assert(writer_anchor()==5 && writer_caret()==14 && !writer_dirty());
    key(0x2c,WRITER_MOD_CTRL); compare_docs(&prior,writer_document());
    assert(writer_anchor()==5 && writer_caret()==14 && !writer_dirty());
    /* An empty paste does not split a pending typing undo run either. */
    writer_new(); type("ab"); key(0x2f,WRITER_MOD_CTRL); type("cd");
    key(0x2c,WRITER_MOD_CTRL); check_text(""); assert(!writer_dirty());
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
    writer_new(); for (char c = 'a'; c <= 'l'; c++) { ticks += 71; assert(writer_key(0, c, 0)); }
    for (int i = 0; i < 8; i++) key(0x2c, WRITER_MOD_CTRL);
    check_text("abcd"); key(0x2c, WRITER_MOD_CTRL); check_text("abcd");
    for (int i = 0; i < 8; i++) key(0x15, WRITER_MOD_CTRL);
    check_text("abcdefghijkl"); key(0x2c, WRITER_MOD_CTRL); type("Z");
    check_text("abcdefghijkZ"); key(0x15, WRITER_MOD_CTRL); check_text("abcdefghijkZ");
    key(0x2c, WRITER_MOD_CTRL | WRITER_MOD_SHIFT); check_text("abcdefghijkZ");
    writer_new(); type("a normal sentence"); key(0x2c, WRITER_MOD_CTRL); check_text("");
    key(0x15, WRITER_MOD_CTRL); check_text("a normal sentence");
    for (int i = 0; i < 8; i++) key(0x0e, 0);
    check_text("a normal "); key(0x2c, WRITER_MOD_CTRL); check_text("a normal sentence");
    ticks += 71; type(" one"); ticks += 71; type(" two");
    key(0x2c, WRITER_MOD_CTRL); check_text("a normal sentence one");
    key(0x2c, WRITER_MOD_CTRL); check_text("a normal sentence");
    key(0x4b, 0); type("X"); key(0x4d, 0); type("Y"); key(0x2c, WRITER_MOD_CTRL);
    check_text("a normal sentencXe"); key(0x2c, WRITER_MOD_CTRL); check_text("a normal sentence");
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
    /* Opening starts at the first character's style, not EOF insertion style. */
    key(0x4f, WRITER_MOD_CTRL); key(0x30, WRITER_MOD_CTRL); assert(writer_save() == WRITER_SAVE_OK);
    assert(writer_open_file(id)); type("A"); assert(writer_document()->style[0] == WRITER_STYLE_BOLD);
    key(0x2c, WRITER_MOD_CTRL); assert(!writer_dirty());
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
    assert(writer_restore(saved, length, plain, fs_identity(plain), 0, 0, 0));
    assert(writer_file() == -1 && writer_dirty());
    assert(!memcmp(fs_data(plain), "plain\ntext", 10));
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
static void test_binding_fingerprints(void) {
    load_fixture("baseline"); int source = writer_file(); unsigned identity = writer_file_identity();
    WriterBinding baseline, current;
    assert(writer_binding(&baseline)); assert(baseline.size == 42);
    /* Independent constants: Python struct.pack + zlib.crc32 and FNV-1a. */
    assert(baseline.hash_a == 0xebc8c161u && baseline.hash_b == 0xdca71052u);
    assert(writer_binding_matches(source, &baseline));
    int duplicate = fs_create(0, "duplicate.bwr"); assert(duplicate > 0);
    assert(fs_write(duplicate, fs_data(source), fs_size(source)) == fs_size(source));
    assert(fs_identity(duplicate) != identity && writer_binding_matches(duplicate, &baseline));
    strcpy(files[duplicate].name, "duplicate.txt"); assert(!writer_binding_matches(duplicate, &baseline));
    /* Same byte count and runtime ID, but a different ordinary native file. */
    assert(!writer_plain_import(&fixture, (const unsigned char *)"external", 8));
    unsigned length; assert(!writer_native_encode(&fixture, encoded, sizeof(encoded), &length));
    assert(fs_write(source, (const char *)encoded, (int)length) == (int)length);
    assert(fs_identity(source) == identity && !writer_binding_matches(source, &baseline));
    int writes = write_count; prior = *writer_document();
    assert(writer_save() == WRITER_SAVE_NEEDS_NAME && write_count == writes);
    compare_docs(&prior, writer_document()); assert(writer_dirty());
    assert(writer_save_as(0, "fixture.bwr") == WRITER_SAVE_NEEDS_NAME && write_count == writes);
    assert(!memcmp(fs_data(source), encoded, length));
    assert(writer_save_as(0, "separate.bwr") == WRITER_SAVE_OK); source = writer_file();
    assert(writer_binding(&current)); assert(current.hash_a == baseline.hash_a && current.hash_b == baseline.hash_b);
    key(0x4f, WRITER_MOD_CTRL); type("!");
    payload_failure = 1;
    assert(writer_save() == WRITER_SAVE_ERROR); assert(writer_binding_matches(source, &baseline));
    assert(writer_save_as(0, "write-failed.bwr") == WRITER_SAVE_ERROR);
    assert(fs_find_child(0, "write-failed.bwr") < 0 && writer_file() == source);
    assert(writer_binding(&current) && !memcmp(&current, &baseline, sizeof baseline));
    payload_failure = 0; sync_failure = 1;
    assert(writer_save() == WRITER_SAVE_ERROR && writer_dirty());
    assert(writer_binding(&current)); assert(current.size == baseline.size + 3);
    assert(writer_binding_matches(source, &current) && !writer_binding_matches(source, &baseline));
    sync_failure = 0; assert(writer_save() == WRITER_SAVE_OK && !writer_dirty());
    baseline = current;
    /* Recovery baseline is the verified source, never the unsaved draft. */
    type(" draft"); const unsigned char *draft = writer_snapshot(&length);
    assert(writer_binding_matches(source, &baseline));
    assert(writer_restore(draft, length, source, fs_identity(source), 1, 3, 1));
    assert(writer_binding(&current) && !memcmp(&current, &baseline, sizeof baseline));
    check_text("baseline! draft"); assert(writer_dirty());
    assert(writer_save() == WRITER_SAVE_OK); assert(!writer_binding_matches(source, &baseline));
    writer_new(); current = baseline; assert(!writer_binding(&current));
    assert(!memcmp(&current, &baseline, sizeof baseline));
    assert(!writer_binding_matches(source, NULL));
    puts("Writer bindings: independent fingerprints, same-ID replacement, cross-node match, sync retry and draft baseline passed");
}
static void test_pdf_export(void) {
    /* Leave previous fixtures intact: exports may never overwrite any of them. */
    load_fixture("PDF pages\nOriginal printable text (with \\ and parentheses).\n");
    int source = writer_file(); unsigned identity = writer_file_identity();
    WriterBinding baseline, after; assert(writer_binding(&baseline));
    int clean_bound = writer_export_pdf(0, "clean.pdf", WRITER_PDF_LETTER);
    assert(clean_bound >= 0 && !writer_dirty() && writer_file() == source && writer_file_identity() == identity);
    assert(writer_binding(&after) && !memcmp(&after, &baseline, sizeof baseline)); fs_delete(clean_bound);
    key(0x47, WRITER_MOD_CTRL); key(0x02, WRITER_MOD_CTRL); key(0x12, WRITER_MOD_CTRL);
    key(0x4f, WRITER_MOD_CTRL); type("Unsaved text."); select_range(0, 9);
    prior = *writer_document(); unsigned caret = writer_caret(), anchor = writer_anchor();
    int creates = create_count, writes = write_count, syncs = sync_count;
    const char *bad_names[] = {NULL, "", "wrong.bwr", "bad/name.pdf", "123456789012345678901.pdf"};
    for (unsigned i = 0; i < sizeof bad_names / sizeof bad_names[0]; i++) assert(writer_export_pdf(0, bad_names[i], WRITER_PDF_LETTER) < 0);
    assert(writer_export_pdf(-1, "invalid.pdf", WRITER_PDF_LETTER) < 0);
    assert(writer_export_pdf(source, "invalid.pdf", WRITER_PDF_LETTER) < 0);
    assert(writer_export_pdf(0, "invalid.pdf", 2) < 0);
    assert(create_count == creates && write_count == writes && sync_count == syncs);
    unsigned length, pages; assert(writer_pdf_export(&prior, WRITER_PDF_LETTER, NULL, 0, &length, &pages) == WRITER_PDF_OK);
    file_limit = length - 1; assert(writer_export_pdf(0, "limited.pdf", WRITER_PDF_LETTER) < 0);
    assert(strstr(writer_status(), "per-file")); file_limit = 2097152;
    node_limit = fs_node_count(); assert(writer_export_pdf(0, "slots.pdf", WRITER_PDF_LETTER) < 0);
    assert(strstr(writer_status(), "slots")); node_limit = 20;
    /* New filesystem records consume capacity; include the projected record. */
    node_cost = 40; storage_limit = fs_used_bytes() + length + (unsigned)fs_node_count() * node_cost;
    assert(writer_export_pdf(0, "space.pdf", WRITER_PDF_LETTER) < 0); assert(strstr(writer_status(), "space"));
    storage_limit = fs_used_bytes() - 1; assert(writer_export_pdf(0, "space.pdf", WRITER_PDF_LETTER) < 0);
    storage_limit = 8u * 1024u * 1024u; node_cost = 0;
    assert(create_count == creates && write_count == writes && sync_count == syncs);
    write_failure = 1; assert(writer_export_pdf(0, "create.pdf", WRITER_PDF_LETTER) < 0); write_failure = 0;
    int deletes = delete_count; payload_failure = 1;
    assert(writer_export_pdf(0, "write.pdf", WRITER_PDF_LETTER) < 0); payload_failure = 0;
    assert(fs_find_child(0, "write.pdf") < 0 && delete_count == deletes + 1);
    file_limit = length; /* Exact per-file and destination byte capacity succeed. */
    storage_limit = fs_used_bytes() + length;
    int letter = writer_export_pdf(0, "letter.PDF", WRITER_PDF_LETTER);
    assert(letter >= 0 && fs_size(letter) == (int)length && !memcmp(fs_data(letter), "%PDF-1.4", 8));
    storage_limit = 8u * 1024u * 1024u; file_limit = 2097152;
    assert(writer_file() == source && writer_file_identity() == identity && writer_dirty());
    assert(writer_binding(&after) && !memcmp(&after, &baseline, sizeof baseline));
    assert(writer_caret() == caret && writer_anchor() == anchor); compare_docs(&prior, writer_document());
    writes = write_count; creates = create_count; syncs = sync_count;
    assert(writer_export_pdf(0, "letter.PDF", WRITER_PDF_LETTER) < 0);
    assert(create_count == creates && write_count == writes && sync_count == syncs);
    /* PDF extension and signature never become plain-text import. */
    assert(!writer_open_file(letter)); assert(strstr(writer_status(), "export-only")); compare_docs(&prior, writer_document());
    strcpy(files[letter].name, "signature.txt"); assert(!writer_open_file(letter)); assert(strstr(writer_status(), "PDF"));
    strcpy(files[letter].name, "letter.PDF");
    sync_failure = 1; assert(writer_export_pdf(0, "pending.pdf", WRITER_PDF_A4) < 0);
    int pending = fs_find_child(0, "pending.pdf"); assert(pending >= 0 && fs_size(pending) > 0);
    assert(strstr(writer_status(), "RAM") && strstr(writer_status(), "Retry"));
    writes = write_count; creates = create_count; syncs = sync_count;
    assert(writer_export_pdf(0, "pending.pdf", WRITER_PDF_LETTER) < 0);
    assert(sync_count == syncs && write_count == writes && create_count == creates);
    assert(writer_export_pdf(0, "pending.pdf", WRITER_PDF_A4) < 0); assert(sync_count == syncs + 1);
    assert(write_count == writes && create_count == creates); /* Sync retry never rewrites. */
    key(0x4f, WRITER_MOD_CTRL); type(" changed");
    assert(writer_export_pdf(0, "pending.pdf", WRITER_PDF_A4) < 0); assert(strstr(writer_status(), "differs"));
    key(0x2c, WRITER_MOD_CTRL); compare_docs(&prior, writer_document());
    unsigned char saved_byte = files[pending].data[9]; files[pending].data[9] ^= 1;
    assert(writer_export_pdf(0, "pending.pdf", WRITER_PDF_A4) < 0); assert(strstr(writer_status(), "outside"));
    assert(sync_count == syncs + 1 && write_count == writes); files[pending].data[9] = saved_byte;
    files[pending].size--; assert(writer_export_pdf(0, "pending.pdf", WRITER_PDF_A4) < 0); files[pending].size++;
    sync_failure = 0; assert(writer_export_pdf(0, "pending.pdf", WRITER_PDF_A4) == pending);
    assert(write_count == writes && create_count == creates && sync_count == syncs + 2);
    assert(writer_file() == source && writer_file_identity() == identity && writer_dirty());
    assert(writer_binding(&after) && !memcmp(&after, &baseline, sizeof baseline));
    /* A deleted target's reused ID does not carry pending-export ownership. */
    sync_failure = 1; assert(writer_export_pdf(0, "reused.pdf", WRITER_PDF_A4) < 0);
    int reused = fs_find_child(0, "reused.pdf"); assert(fs_delete(reused) == 0);
    assert(fs_create(0, "reused.pdf") == reused); assert(fs_write(reused, "external", 8) == 8);
    writes = write_count; syncs = sync_count;
    assert(writer_export_pdf(0, "reused.pdf", WRITER_PDF_A4) < 0);
    assert(fs_size(reused) == 8 && !memcmp(fs_data(reused), "external", 8));
    assert(write_count == writes && sync_count == syncs); sync_failure = 0;
    fs_delete(letter); fs_delete(pending); fs_delete(reused);
    sync_failure = 1; assert(writer_export_pdf(0, "orphan.pdf", WRITER_PDF_LETTER) < 0);
    int orphan = fs_find_child(0, "orphan.pdf"); assert(orphan >= 0);
    writer_new(); sync_failure = 0; writes = write_count; syncs = sync_count;
    assert(writer_export_pdf(0, "orphan.pdf", WRITER_PDF_LETTER) < 0);
    assert(write_count == writes && sync_count == syncs && fs_size(orphan) > 0); fs_delete(orphan);
    /* Dense styles exceed the arena while the ordinary document stays valid. */
    writer_doc_init(&fixture); fixture.length = WRITER_TEXT_MAX;
    for (unsigned i = 0; i < fixture.length; i++) { fixture.text[i] = 'W'; fixture.style[i] = (unsigned char)(i & 7); }
    fixture.paragraph[0] = WRITER_PARAGRAPH_HEADING;
    unsigned bytes; assert(!writer_native_encode(&fixture, encoded, sizeof encoded, &bytes));
    assert(writer_restore(encoded, bytes, -1, 0, 1, 8, 3)); prior = *writer_document(); creates = create_count; writes = write_count;
    assert(writer_export_pdf(0, "large.pdf", WRITER_PDF_A4) < 0); assert(strstr(writer_status(), "512 KiB"));
    assert(create_count == creates && write_count == writes); compare_docs(&prior, writer_document());
    assert(writer_dirty() && writer_caret() == 8 && writer_anchor() == 3);
    writer_doc_init(&fixture); fixture.length = WRITER_TEXT_MAX;
    for (unsigned i = 0; i <= fixture.length; i++) {
        if (i < fixture.length) fixture.text[i] = '\n';
        fixture.paragraph[i] = WRITER_PARAGRAPH_HEADING;
    }
    assert(!writer_native_encode(&fixture, encoded, sizeof encoded, &bytes));
    assert(writer_restore(encoded, bytes, -1, 0, 1, 8, 3)); prior = *writer_document();
    assert(writer_pdf_export(&prior, WRITER_PDF_LETTER, NULL, 0, &length, &pages) == WRITER_PDF_OK && pages == 1214);
    int maximum = writer_export_pdf(0, "1214-pages.pdf", WRITER_PDF_LETTER);
    assert(maximum >= 0 && fs_size(maximum) == (int)length && writer_dirty());
    compare_docs(&prior, writer_document()); fs_delete(maximum);
    /* Export never inserts history or consumes redo. */
    writer_new(); type("first"); ticks += 71; type(" second"); key(0x2c, WRITER_MOD_CTRL); check_text("first");
    int clean = writer_export_pdf(0, "history.pdf", WRITER_PDF_LETTER); assert(clean >= 0 && writer_dirty());
    key(0x15, WRITER_MOD_CTRL); check_text("first second"); fs_delete(clean);
    writer_new(); int empty = writer_export_pdf(0, "empty.pdf", WRITER_PDF_LETTER); assert(empty >= 0 && !writer_dirty() && writer_file() == -1); fs_delete(empty);
    puts("Writer PDF: exact storage preflight, safe failures, identity/content sync-only retry, native/history preservation passed");
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

static void search_fields(const char *query, const char *replacement) {
    key(0x23, WRITER_MOD_CTRL); key(0x1e, WRITER_MOD_CTRL); key(0x0e, 0); type(query);
    key(0x0f, 0); key(0x1e, WRITER_MOD_CTRL); key(0x0e, 0); type(replacement);
}
static void test_search(void) {
    load_fixture("Cat cat\nCAT\nend cat");
    select_range(0, 3); key(0x30, WRITER_MOD_CTRL);
    select_range(4, 7); key(0x16, WRITER_MOD_CTRL);
    select_range(8, 11); key(0x17, WRITER_MOD_CTRL); key(0x02, WRITER_MOD_CTRL); key(0x13, WRITER_MOD_CTRL);
    prior = *writer_document();
    assert(writer_word_count() == 5);
    search_fields("cat", "dogs");
    assert(writer_key(0x12,0,WRITER_MOD_CTRL|WRITER_MOD_SHIFT) == WRITER_REQUEST_EXPORT);
    compare_docs(&prior,writer_document());
    assert(writer_key(0x1f,0,WRITER_MOD_CTRL) == WRITER_REQUEST_SAVE);
    assert(writer_key(0x1f,0,WRITER_MOD_CTRL|WRITER_MOD_SHIFT) == WRITER_REQUEST_SAVE_AS);
    key(0x12,WRITER_MOD_CTRL); compare_docs(&prior,writer_document());
    key(0x1c, WRITER_MOD_CTRL | WRITER_MOD_SHIFT);
    check_text("dogs dogs\ndogs\nend dogs");
    const WriterDoc *d = writer_document();
    for (int i = 0; i < 4; i++) assert(d->style[i] == WRITER_STYLE_BOLD);
    for (int i = 5; i < 9; i++) assert(d->style[i] == WRITER_STYLE_UNDERLINE);
    for (int i = 10; i < 14; i++) assert(d->style[i] == WRITER_STYLE_ITALIC);
    assert(d->paragraph[10] == 6 && writer_word_count() == 5);
    key(0x2c, WRITER_MOD_CTRL); compare_docs(&prior, writer_document());
    key(0x15, WRITER_MOD_CTRL); check_text("dogs dogs\ndogs\nend dogs");
    key(0x01, 0);
    /* Find wraps in both directions without changing the document. */
    key(0x47, WRITER_MOD_CTRL); key(0x21, WRITER_MOD_CTRL); key(0x1e, WRITER_MOD_CTRL); type("dogs");
    key(0x1c, 0); assert(writer_anchor() == 0 && writer_caret() == 4);
    key(0x3d, 0); assert(writer_anchor() == 5 && writer_caret() == 9);
    key(0x3d, WRITER_MOD_SHIFT); assert(writer_anchor() == 0 && writer_caret() == 4);
    key(0x3d, WRITER_MOD_SHIFT); assert(writer_anchor() == 19 && writer_caret() == 23);
    key(0x01, 0);
    /* A single replacement inherits the first match byte's character style. */
    search_fields("dogs", "x"); key(0x1c, WRITER_MOD_CTRL); key(0x01, 0);
    check_text("dogs dogs\ndogs\nend x"); assert(writer_document()->style[19] == prior.style[15]);
    /* Case sensitivity is a real control, shared by drawing and hit testing. */
    load_fixture("Cat cat CAT"); search_fields("cat", "dog"); writer_draw(0,0,420,260);
    assert(writer_click(0,0,420,260,120,210,0) & WRITER_CHANGED);
    key(0x1c, WRITER_MOD_CTRL | WRITER_MOD_SHIFT); key(0x01, 0); check_text("Cat dog CAT");
    /* Exact case control is persistent; switch back for subsequent checks. */
    key(0x23, WRITER_MOD_CTRL); writer_draw(0,0,420,260);
    assert(writer_click(0,0,420,260,120,210,0) & WRITER_CHANGED); key(0x01, 0);
    /* Empty replacement keeps empty paragraphs and their heading/alignment. */
    load_fixture("cat\ncat\n"); select_range(4,7); key(0x02,WRITER_MOD_CTRL); key(0x13,WRITER_MOD_CTRL);
    search_fields("cat", ""); key(0x1c, WRITER_MOD_CTRL | WRITER_MOD_SHIFT); key(0x01, 0);
    check_text("\n\n"); assert(writer_document()->paragraph[1] == 6);
    /* Final-size preflight rejects growth atomically; no phantom undo entry. */
    writer_new(); memset(clipboard, 'a', 32760); clipboard_length = 32760; clipboard[32760] = 0; clipboard_generation++;
    key(0x2f, WRITER_MOD_CTRL); prior = *writer_document(); search_fields("a", "aa");
    key(0x1c, WRITER_MOD_CTRL | WRITER_MOD_SHIFT); compare_docs(&prior, writer_document());
    key(0x2c, WRITER_MOD_CTRL); check_text(""); key(0x01, 0);
    /* Search-field paste does not silently truncate or discard selected input. */
    load_fixture("needle retained"); search_fields("needle", "x"); key(0x0f, 0); key(0x1e, WRITER_MOD_CTRL);
    memset(clipboard, 'q', 64); clipboard_length = 64; clipboard[64] = 0; clipboard_generation++;
    key(0x2f, WRITER_MOD_CTRL); key(0x1c, 0); assert(writer_anchor() == 0 && writer_caret() == 6);
    key(0x1e,WRITER_MOD_CTRL); writer_clipboard_set("",0); key(0x2f,WRITER_MOD_CTRL);
    key(0x1c,0); assert(writer_anchor()==0 && writer_caret()==6);
    assert(!writer_dirty()); key(0x01, 0);
    puts("Writer search: wrapping/case, style-preserving single/all replacement, atomic limits and field editing passed");
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
static unsigned colored_pixels(int x, int y, int w, int h, unsigned char color) {
    unsigned count = 0;
    for (int row = y; row < y+h; row++) for (int col = x; col < x+w; col++)
        if (back[row*800+col] == color) count++;
    return count;
}
static void test_viewport_reveal(void) {
    load_fixture("Line 1\nLine 2\nLine 3\nLine 4\nLine 5\nLine 6\nLine 7\nLine 8\nLine 9\nLine 10\nLine 11\nLine 12\nLine 13\nLine 14\nLine 15\nLine 16\nLine 17\nLine 18\nLine 19\nLine 20\ntarget");
    writer_draw(0,0,420,260); key(0x4f,WRITER_MOD_CTRL);
    key(0x21,WRITER_MOD_CTRL); key(0x1e,WRITER_MOD_CTRL); type("target"); key(0x1c,0);
    writer_draw(0,0,420,260);
    assert(colored_pixels(22,74,380,90,COLOR_BLUE) > 10);
    key(0x01,0); key(0x02,WRITER_MOD_CTRL); writer_draw(0,0,420,260);
    assert(colored_pixels(22,74,380,152,COLOR_BLUE) > 10);
    /* A deliberate scroll away from selection survives a larger resize. */
    writer_scroll(-100); writer_draw(0,0,720,520);
    assert(!colored_pixels(22,74,680,412,COLOR_BLUE));
    key(0x4f,WRITER_MOD_CTRL); writer_draw(0,0,720,520);
    assert(colored_pixels(22,74,680,412,COLOR_BLACK) > 10);
    puts("Writer viewport: Find-bar shrink and style reflow reveal selection; manual scrolling survives resize");
}
static void test_heading_coverage(void) {
    load_fixture("A"); key(0x02, WRITER_MOD_CTRL); key(0x47, WRITER_MOD_CTRL);
    writer_draw(0,0,420,260);
    int bpr, h, w; const unsigned char *bits = ui_glyph('A', &bpr, &h, &w);
    assert(h == 18 && w == 16);
    /* Independent four-subpixel area reference for exactly 1.5x magnification.
     * Skip x=0 because the ordinary insertion caret is painted there. */
    unsigned mixed = 0;
    for (int y = 0; y < 27; y++) for (int x = 1; x < 24; x++) {
        int sum = 0;
        for (int sy = 0; sy < 2; sy++) for (int sx = 0; sx < 2; sx++)
            sum += glyph_level(bits,bpr,(4*y+2*sy+1)/6,(4*x+2*sx+1)/6);
        int level = (sum + 2) / 4;
        unsigned char expected = level >= 15 ? COLOR_DKGRAY : gfx_mix(COLOR_WHITE,COLOR_DKGRAY,(level*256+7)/15);
        assert(back[(76+y)*800+22+x] == expected);
        if (level != glyph_level(bits,bpr,y*2/3,x*2/3)) mixed++;
    }
    assert(mixed > 8);
    int x; assert(writer_position(1,&x,NULL,NULL)); assert(x == (ui_advance('A')*3+1)/2);
    puts("Writer headings: independent area-coverage pixels and unchanged 1.5x advances passed");
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
        for (int mode = 0; mode < 2; mode++) {
            key(mode ? 0x23 : 0x21, WRITER_MOD_CTRL);
            memset(back, COLOR_MAGENTA, sizeof back); writer_draw(x,y,w,h);
            for (int py = 0; py < 600; py++) for (int px = 0; px < 800; px++)
                if (px < x || px >= x+w || py < y || py >= y+h) assert(back[py*800+px] == COLOR_MAGENTA);
            key(0x01, 0);
        }
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
    for (int width = WRITER_MIN_W; width <= WRITER_W; width += WRITER_W - WRITER_MIN_W) {
        assert(ui_string_w("Tab: focus   Arrows/Space: paper   Esc: cancel") <= 376);
        assert(ui_string_w("RTF") + 8 <= 42 && ui_string_w("PDF") + 8 <= 42);
        assert(writer_click(30,20,width,260,30+270,20+35,0) == WRITER_REQUEST_EXPORT);
        assert(writer_click(30,20,width,260,30+311,20+59,0) == WRITER_REQUEST_EXPORT);
        assert(writer_click(30,20,width,260,30+316,20+35,0) == WRITER_REQUEST_PDF);
        assert(writer_click(30,20,width,260,30+357,20+59,0) == WRITER_REQUEST_PDF);
        assert(writer_click(30,20,width,260,30+314,20+40,0) == 0);
    }
    assert(writer_key(0x19, 0, WRITER_MOD_CTRL | WRITER_MOD_SHIFT) == WRITER_REQUEST_PDF);
    assert(writer_key(0x19, 0, WRITER_MOD_CTRL) == 0);
    assert(writer_key(0x19, 0, WRITER_MOD_CTRL | WRITER_MOD_SHIFT | WRITER_MOD_ALT) == 0);
    key(0x21, WRITER_MOD_CTRL);
    assert(writer_key(0x19, 0, WRITER_MOD_CTRL | WRITER_MOD_SHIFT) == WRITER_REQUEST_PDF);
    assert(writer_key(0x12, 0, WRITER_MOD_CTRL | WRITER_MOD_SHIFT) == WRITER_REQUEST_EXPORT);
    key(0x01, 0);
    if (path) {
        select_range(17,31); writer_draw(30,20,720,520); write_ppm(path);
        char search_path[256]; snprintf(search_path, sizeof(search_path), "%s-search.ppm", path);
        search_fields("document", "draft"); key(0x1c,0);
        memset(back, COLOR_LTGRAY, sizeof back); writer_draw(30,20,420,260); write_ppm(search_path);
    }
    puts("Writer rendering: normal/minimum/offscreen clipping, toolbar hits and mouse selection passed");
}
int main(int argc, char **argv) {
    files[0].valid = files[0].folder = 1; files[0].identity = 1;
    gfx_init(back, linear, 800, 600, 32, 3200);
    test_editing(); test_history(); test_files(); test_binding_fingerprints(); test_pdf_export(); test_layout(); test_search(); test_viewport_reveal(); test_heading_coverage(); test_drawing(argc > 1 ? argv[1] : NULL);
    assert(polls > 100); assert(sync_count >= 4);
    for (int i = 1; i < 20; i++) if (files[i].valid) free(files[i].data);
    puts("All Writer host functional checks passed.");
    return 0;
}
