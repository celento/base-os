/* Both real document models and the unchanged native coordinator on supported
 * deterministic default/large disks. No hardware or debugger access. */
#ifdef DOCUMENT_LARGE_PROFILE
#define main document_large_fixture_main
#include "large_volume_host.c"
#undef main
#include "../src/native_sync.c"
unsigned large_test_polls;
static void model_setup(void) {
    high_available = 1; mark(); assert(!fs_init() && fs_load_disk() == FS_LOAD_BLANK);
    assert(fs_large_profile() && !fs_empty_dir(0));
}
#else
#define NATIVE_SYNC_FIXTURE
#include "native_sync_host.c"
static void model_setup(void) { setup(); }
#endif
#include "writer.h"
#include "sheet.h"
static unsigned model_polls;
void platform_poll(void) { ++model_polls; }
static void model_drain(void) {
#ifndef DOCUMENT_LARGE_PROFILE
    clear_trace();
#endif
    unsigned steps = 0;
    while (fs_sync_busy()) { fs_sync_step(); native_sync_tick(); assert(++steps < 200000); }
    native_sync_tick();
}
static void text(const char *value) { while (*value) assert(writer_key(0, *value++, 0) == WRITER_CHANGED); }
static void cell_text(const char *value) { while (*value) assert(spreadsheet_key(0, *value++, 0) == SPREADSHEET_CHANGED); }
static WriterDoc writer_check;
static SheetDoc sheet_check;
static void verify_writer(const char *name, const char *value) {
    int id = fs_find_child(0, name); assert(id >= 0);
    assert(!writer_native_decode(&writer_check, (const unsigned char *)fs_data(id), (unsigned)fs_size(id)));
    assert(writer_check.length == strlen(value) && !memcmp(writer_check.text, value, writer_check.length));
}
static void verify_sheet(const char *name, const char *value) {
    int id = fs_find_child(0, name); assert(id >= 0);
    assert(!sheet_native_decode(&sheet_check, (const unsigned char *)fs_data(id), (unsigned)fs_size(id)));
    assert(!strcmp(sheet_check.cells[0].text, value));
}
static unsigned char model_encoded[SHEET_NATIVE_MAX_SIZE > WRITER_NATIVE_MAX_SIZE ? SHEET_NATIVE_MAX_SIZE : WRITER_NATIVE_MAX_SIZE];
static void maximum_models(void) {
    writer_doc_init(&writer_check); writer_check.length = WRITER_TEXT_MAX;
    for (unsigned i = 0; i < writer_check.length; ++i) writer_check.text[i] = i % 71 == 70 ? '\n' : 'W';
    unsigned length;
    assert(!writer_native_encode(&writer_check, model_encoded, sizeof model_encoded, &length));
    assert(writer_restore(model_encoded, length, -1, 0, 1, 0, 0));
    assert(writer_save_as(0, "max-writer.bwr") == WRITER_SAVE_PENDING); model_drain();
    assert(writer_persistence_poll() && !writer_dirty() && writer_length() == WRITER_TEXT_MAX);
    assert(fs_size(writer_file()) == (int)length && !memcmp(fs_data(writer_file()), model_encoded, length));
    sheet_init(&sheet_check); char value[SHEET_TEXT_MAX + 1]; memset(value, 'S', SHEET_TEXT_MAX); value[SHEET_TEXT_MAX] = 0;
    for (unsigned i = 0; i < SHEET_CELLS; ++i) assert(!sheet_set(&sheet_check, i / SHEET_COLS, i % SHEET_COLS, SHEET_TEXT, value, SHEET_TEXT_MAX));
    assert(!sheet_native_encode(&sheet_check, model_encoded, sizeof model_encoded, &length));
    assert(spreadsheet_restore(model_encoded, length, -1, 0, 1, 0, 0));
    assert(spreadsheet_save_as(0, "max-sheet.bsh") == SPREADSHEET_SAVE_PENDING); model_drain();
    assert(spreadsheet_persistence_poll() && !spreadsheet_dirty());
    assert(fs_size(spreadsheet_file()) == (int)length && !memcmp(fs_data(spreadsheet_file()), model_encoded, length));
    writer_close(); spreadsheet_close(); assert(!fs_init() && !fs_load_disk());
    assert(writer_open_file(fs_find_child(0, "max-writer.bwr")) && writer_length() == WRITER_TEXT_MAX);
    assert(spreadsheet_open_file(fs_find_child(0, "max-sheet.bsh")));
    assert(spreadsheet_document()->cells[SHEET_CELLS - 1].length == SHEET_TEXT_MAX);
    puts("document models: maximum Writer text and all 3,328 maximum-size Sheet cells survive exact native boundary/remount");
}
int main(void) {
    model_setup(); writer_init(); spreadsheet_init();
    text("First native boundary");
    assert(writer_save_as(0, "writer.bwr") == WRITER_SAVE_PENDING && writer_dirty());
    cell_text("12.5"); assert(spreadsheet_editing());
    unsigned caret = spreadsheet_caret();
    assert(spreadsheet_save_as(0, "sheet.bsh") == SPREADSHEET_SAVE_ERROR);
    assert(spreadsheet_editing() && spreadsheet_caret() == caret && fs_find_child(0, "sheet.bsh") < 0);
    text(" + private edit"); model_drain(); assert(writer_persistence_poll() && writer_dirty());
    verify_writer("writer.bwr", "First native boundary");
    assert(spreadsheet_save_as(0, "sheet.bsh") == SPREADSHEET_SAVE_PENDING);
    assert(!spreadsheet_editing() && spreadsheet_dirty());
    /* A newer uncommitted editor must survive completion collection, including
     * background/minimized clients whose ordinary blink tick is inactive. */
    cell_text("27.125"); assert(spreadsheet_editing());
    model_drain(); assert(spreadsheet_persistence_poll() && spreadsheet_editing() && spreadsheet_dirty());
    verify_sheet("sheet.bsh", "12.5");
    assert(writer_save() == WRITER_SAVE_PENDING); model_drain(); assert(writer_persistence_poll() && !writer_dirty());
    assert(spreadsheet_save() == SPREADSHEET_SAVE_PENDING); model_drain(); assert(spreadsheet_persistence_poll() && !spreadsheet_dirty());
    verify_writer("writer.bwr", "First native boundary + private edit"); verify_sheet("sheet.bsh", "27.125");
    assert(writer_export_rtf(0, "writer.rtf") == WRITER_EXPORT_PENDING); model_drain(); assert(writer_persistence_poll());
    assert(writer_export_pdf(0, "writer.pdf", WRITER_PDF_A4) == WRITER_EXPORT_PENDING); model_drain(); assert(writer_persistence_poll());
    assert(spreadsheet_export_csv(0, "sheet.csv") == SPREADSHEET_EXPORT_PENDING); model_drain(); assert(spreadsheet_persistence_poll());
    assert(!writer_dirty() && !spreadsheet_dirty());
    writer_close(); spreadsheet_close(); assert(!fs_init() && !fs_load_disk());
    verify_writer("writer.bwr", "First native boundary + private edit"); verify_sheet("sheet.bsh", "27.125");
    int rtf = fs_find_child(0, "writer.rtf"), pdf = fs_find_child(0, "writer.pdf"), csv = fs_find_child(0, "sheet.csv");
    assert(rtf >= 0 && !memcmp(fs_data(rtf), "{\\rtf", 5));
    assert(pdf >= 0 && !memcmp(fs_data(pdf), "%PDF-1.4", 8));
    assert(csv >= 0 && fs_size(csv) == 8 && !memcmp(fs_data(csv), "27.125\r\n", 8));
    assert(writer_open_file(fs_find_child(0, "writer.bwr")) && !writer_dirty());
    assert(spreadsheet_open_file(fs_find_child(0, "sheet.bsh")) && !spreadsheet_dirty());
    maximum_models();
    assert(model_polls);
    puts("document models: real default/large FS pending boundaries, independent edits, exports and remount passed");
    return 0;
}
