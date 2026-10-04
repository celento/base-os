/* Ordinary model/adapter workflows with a deterministic storage service fixture.
 * No device pumping happens inside begin/poll. Revision limits are direct unit
 * boundaries in the host model, never guest memory edits or fault probes. */
#define main synchronous_ui_fixture_main
#include "sheet_ui_host.c"
#undef main
#define SPREADSHEET_HOST_EXTERNAL_CLIPBOARD
#include "../src/sheet.c"

static void fixture_reset(void) {
    storage_busy = 0; native_result = BOS_PENDING; native_begin_result = BOS_OK;
    create_failure = write_failure = short_write = sync_failure = 0; version_budget = -1;
    file_limit = FS_FILE_MAX; async_backend = 1;
    spreadsheet_new(); assert(!native_handle);
    for (int i = 1; i < FILE_COUNT; i++) if (fs_valid(i)) assert(!fs_delete(i));
    assert(!spreadsheet_dirty());
}
static DocumentSave save_info(void) {
    DocumentSave save; spreadsheet_save_info(&save); return save;
}
static void complete(int result) {
    assert(native_handle); native_result = result; storage_busy = 0;
    unsigned releases = native_release_count;
    assert(spreadsheet_persistence_poll() == SPREADSHEET_CHANGED);
    assert(!save_info().pending && !native_handle && native_release_count == releases + 1);
    assert(!spreadsheet_persistence_poll());
    assert(native_release_count == releases + 1);
    native_result = BOS_PENDING;
}
static void read_native(int id) {
    assert(!sheet_native_decode(&decoded, (const unsigned char *)fs_data(id), (unsigned)fs_size(id)));
}
static void assert_pending_native(int id) {
    DocumentSave save = save_info();
    assert(save.owner && save.handle && save.pending && save.kind == DOCUMENT_SAVE_NATIVE);
    assert(save.result == BOS_PENDING && save.target.file == id && document_target_matches(&save.target));
    assert(save.revision.epoch && save.revision.counter && spreadsheet_dirty());
    assert(!sync_count);
}
static void test_native_pending(void) {
    fixture_reset(); type("10");
    assert(spreadsheet_save_as(0, "pending.bsh") == SPREADSHEET_SAVE_PENDING);
    int id = spreadsheet_file(); assert_pending_native(id);
    assert(!spreadsheet_editing()); read_native(id); assert(!strcmp(decoded.cells[0].text, "10"));
    DocumentSave first = save_info(); DocumentRevision old_saved = state.saved_revision;
    int writes = write_count; unsigned begins = native_begin_count;
    jump("B1"); type("20");
    unsigned caret = state.edit_caret, anchor = state.edit_anchor;
    assert(spreadsheet_save() == SPREADSHEET_SAVE_PENDING);
    assert(spreadsheet_save_as(0, "other.bsh") == SPREADSHEET_SAVE_ERROR);
    assert(spreadsheet_export_csv(0, "busy.csv") == -1);
    assert(!fs_valid(fs_find_child(0, "other.bsh")) && fs_find_child(0, "busy.csv") < 0);
    assert(write_count == writes && native_begin_count == begins && state.edit_caret == caret && state.edit_anchor == anchor);
    assert(spreadsheet_editing() && !strcmp(state.edit, "20"));
    assert(save_info().handle == first.handle && document_revision_equal(state.saved_revision, old_saved));
    unsigned length; const unsigned char *draft = spreadsheet_snapshot(&length);
    assert(draft && !sheet_native_decode(&decoded, draft, length));
    assert(!strcmp(decoded.cells[1].text, "20"));
    /* Submission must not consume a completed event before the desktop hook. */
    native_result = BOS_OK; storage_busy = 0;
    assert(spreadsheet_save() == SPREADSHEET_SAVE_PENDING && save_info().pending);
    assert(document_revision_equal(state.saved_revision, old_saved) && write_count == writes);
    complete(BOS_OK);
    assert(document_revision_equal(state.saved_revision, first.revision));
    assert(spreadsheet_editing() && !strcmp(state.edit, "20") && spreadsheet_dirty());
    assert(state.edit_caret == caret && state.edit_anchor == anchor);
    key(0x01, 0); assert(!spreadsheet_dirty());
    enter("A1", "11"); assert(spreadsheet_dirty());
    key(0x2c, SPREADSHEET_MOD_CTRL); assert(!spreadsheet_dirty());
    key(0x15, SPREADSHEET_MOD_CTRL); assert(spreadsheet_dirty());
    assert(spreadsheet_save() == SPREADSHEET_SAVE_PENDING);
    DocumentSave second = save_info(); assert(second.handle != first.handle && second.owner == first.owner);
    key(0x2c, SPREADSHEET_MOD_CTRL); assert(spreadsheet_dirty());
    assert(!spreadsheet_editing()); complete(BOS_OK);
    assert(spreadsheet_dirty()); key(0x15, SPREADSHEET_MOD_CTRL); assert(!spreadsheet_dirty());
    assert(document_revision_equal(snapshot()->revision, second.revision));
    assert(spreadsheet_save_as(0, "clean-copy.bsh") == SPREADSHEET_SAVE_PENDING && spreadsheet_dirty());
    assert(spreadsheet_file() != id); complete(BOS_OK); assert(!spreadsheet_dirty());
    /* A clean resave must remain dirty until its own new boundary completes. */
    assert(spreadsheet_save() == SPREADSHEET_SAVE_PENDING && spreadsheet_dirty());
    complete(BOS_OK); assert(!spreadsheet_dirty());
    assert(spreadsheet_save() == SPREADSHEET_SAVE_PENDING);
    DocumentRevision submitted = save_info().revision;
    for (char digit = '2'; digit <= '8'; digit++) {
        char text[2] = {digit, 0}; enter("A1", text);
    }
    assert(state.count == HISTORY); complete(BOS_OK);
    assert(document_revision_equal(state.saved_revision, submitted) && spreadsheet_dirty());
    for (unsigned i = 0; i < HISTORY; i++) {
        key(0x2c, SPREADSHEET_MOD_CTRL); assert(spreadsheet_dirty());
    }
    puts("Spreadsheet async native: explicit pending, one submission, new editor preservation, undo/redo and clean Save As passed");
}
static void test_failures_and_versions(void) {
    fixture_reset(); enter("A1", "101");
    native_begin_result = BOS_E_CAPACITY;
    assert(spreadsheet_save_as(0, "capacity.bsh") == SPREADSHEET_SAVE_ERROR);
    int id = spreadsheet_file(); DocumentSave rejected = save_info();
    assert(id > 0 && fs_size(id) > 0 && rejected.result == BOS_E_CAPACITY && !rejected.handle && spreadsheet_dirty());
    assert(strstr(spreadsheet_status(), "capacity"));
    SpreadsheetBinding binding; assert(spreadsheet_binding(&binding) && spreadsheet_binding_matches(id, &binding));
    native_begin_result = BOS_E_PROTECTED;
    enter("B1", "202"); assert(spreadsheet_save() == SPREADSHEET_SAVE_ERROR);
    assert(save_info().result == BOS_E_PROTECTED && spreadsheet_dirty());
    read_native(id); assert(!strcmp(decoded.cells[1].text, "202"));
    native_begin_result = BOS_OK;
    assert(spreadsheet_save() == SPREADSHEET_SAVE_PENDING); complete(BOS_E_IO);
    assert(spreadsheet_dirty() && save_info().result == BOS_E_IO && fs_size(id) > 0);
    enter("C1", "303"); assert(spreadsheet_save() == SPREADSHEET_SAVE_PENDING); complete(BOS_OK);
    assert(!spreadsheet_dirty()); read_native(id); assert(!strcmp(decoded.cells[2].text, "303"));
    /* A completed service outcome cannot clean bytes changed before collection. */
    assert(spreadsheet_save() == SPREADSHEET_SAVE_PENDING);
    native_result = BOS_OK; storage_busy = 0;
    unsigned size = (unsigned)fs_size(id); memcpy(encoded, fs_data(id), size);
    assert(fs_write(id, (const char *)encoded, (int)size) == (int)size);
    assert(spreadsheet_persistence_poll() && save_info().result == BOS_E_STALE && spreadsheet_dirty());
    native_result = BOS_PENDING;
    assert(spreadsheet_save() == SPREADSHEET_SAVE_PENDING);
    storage_busy = 0; native_result = BOS_OK; mount_incarnation++;
    assert(spreadsheet_persistence_poll() && save_info().result == BOS_E_STALE && spreadsheet_dirty());
    assert(spreadsheet_save() == SPREADSHEET_SAVE_NEEDS_NAME);
    assert(spreadsheet_open_file(id));
    native_result = BOS_PENDING;
    assert(spreadsheet_save() == SPREADSHEET_SAVE_PENDING);
    storage_busy = 0; strcpy(files[id].name, "renamed.bsh"); complete(BOS_OK);
    assert(!spreadsheet_dirty() && !strcmp(spreadsheet_title(), "renamed.bsh"));
    assert(spreadsheet_save() == SPREADSHEET_SAVE_PENDING);
    storage_busy = 0; strcpy(files[id].name, "renamed.csv"); complete(BOS_OK);
    assert(spreadsheet_dirty() && save_info().result == BOS_E_STALE && spreadsheet_file() == -1);
    strcpy(files[id].name, "renamed.bsh");
    assert(spreadsheet_save() == SPREADSHEET_SAVE_PENDING);
    storage_busy = 0; assert(!fs_delete(id)); assert(make_file("unrelated.bsh", "safe", 4) == id);
    complete(BOS_OK); assert(save_info().result == BOS_E_STALE && spreadsheet_dirty());
    int writes = write_count; assert(spreadsheet_save() == SPREADSHEET_SAVE_NEEDS_NAME && write_count == writes);
    assert(!memcmp(fs_data(id), "safe", 4));
    /* Honest immediate success still uses the submitted full revision. */
    native_result = BOS_OK;
    assert(spreadsheet_save_as(0, "immediate.bsh") == SPREADSHEET_SAVE_OK && !spreadsheet_dirty());
    assert(!native_handle && !save_info().pending && save_info().result == BOS_OK);
    native_result = BOS_PENDING;
    int unknown = spreadsheet_file(); unsigned version = files[unknown].version;
    files[unknown].version = 0;
    assert(spreadsheet_open_file(unknown) && spreadsheet_dirty() && spreadsheet_file() < 0);
    assert(!spreadsheet_binding(&binding));
    unsigned length = (unsigned)fs_size(unknown); memcpy(encoded, fs_data(unknown), length);
    assert(spreadsheet_restore(encoded, length, unknown, fs_identity(unknown), 0, 0, 0));
    assert(spreadsheet_file() < 0 && spreadsheet_dirty());
    files[unknown].version = version; files[unknown].identity = 0;
    assert(spreadsheet_open_file(unknown) && spreadsheet_dirty() && spreadsheet_file() < 0);
    assert(spreadsheet_restore(encoded, length, unknown, 0, 0, 0, 0));
    assert(spreadsheet_file() < 0 && spreadsheet_dirty());
    assert(!sync_count);
    puts("Spreadsheet async failures: capacity/protected/I/O retain RAM, replacement/remount/type/identity guard and immediate success passed");
}
static void test_busy_and_capacity(void) {
    fixture_reset(); type("42");
    unsigned count = state.count, current = state.current;
    DocumentRevision next = state.next_revision;
    int writes = write_count; unsigned begins = native_begin_count;
    storage_busy = 1;
    assert(spreadsheet_save() == SPREADSHEET_SAVE_ERROR);
    assert(spreadsheet_save_as(0, "busy.bsh") == SPREADSHEET_SAVE_ERROR);
    assert(spreadsheet_export_csv(0, "busy.csv") == -1);
    assert(state.mode == 1 && !strcmp(state.edit, "42") && state.count == count && state.current == current);
    assert(document_revision_equal(state.next_revision, next) && write_count == writes && native_begin_count == begins);
    storage_busy = 0; version_budget = 0;
    assert(spreadsheet_save_as(0, "no-version.bsh") == SPREADSHEET_SAVE_ERROR);
    assert(spreadsheet_export_csv(0, "no-version.csv") == -1);
    assert(state.mode == 1 && !strcmp(state.edit, "42") && state.count == count && state.current == current);
    assert(document_revision_equal(state.next_revision, next) && write_count == writes);
    version_budget = 1;
    assert(spreadsheet_save_as(0, "create-only.bsh") == SPREADSHEET_SAVE_ERROR);
    assert(fs_find_child(0, "create-only.bsh") < 0 && write_count == writes && spreadsheet_file() < 0);
    assert(!spreadsheet_editing() && !strcmp(document()->cells[0].text, "42"));
    version_budget = 1;
    assert(spreadsheet_export_csv(0, "create-only.csv") == -1);
    assert(fs_find_child(0, "create-only.csv") < 0 && write_count == writes);
    version_budget = -1; write_failure = 1;
    assert(spreadsheet_save_as(0, "failed.bsh") == SPREADSHEET_SAVE_ERROR);
    assert(spreadsheet_export_csv(0, "failed.csv") == -1);
    assert(fs_find_child(0, "failed.bsh") < 0 && fs_find_child(0, "failed.csv") < 0);
    write_failure = 0; short_write = 1;
    assert(spreadsheet_save_as(0, "short.bsh") == SPREADSHEET_SAVE_ERROR);
    assert(spreadsheet_export_csv(0, "short.csv") == -1);
    assert(fs_find_child(0, "short.bsh") < 0 && fs_find_child(0, "short.csv") < 0);
    assert(!native_handle); short_write = 0;
    assert(!sync_count);
    puts("Spreadsheet async admission: lease and version checks preserve pending editor; incomplete placeholders roll back passed");
}
static void test_exports(void) {
    fixture_reset(); enter("A1", "'=2+3");
    assert(spreadsheet_save_as(0, "native.bsh") == SPREADSHEET_SAVE_PENDING); complete(BOS_OK);
    int source = spreadsheet_file(); DocumentRevision durable = state.saved_revision;
    assert(spreadsheet_export_csv(0, "values.csv") == SPREADSHEET_EXPORT_PENDING);
    int csv = fs_find_child(0, "values.csv"); DocumentSave export = save_info();
    assert(export.kind == DOCUMENT_SAVE_CSV && export.target.file == csv && !spreadsheet_dirty());
    int writes = write_count; unsigned begins = native_begin_count;
    jump("B1"); type("pending");
    assert(spreadsheet_save() == SPREADSHEET_SAVE_ERROR);
    assert(spreadsheet_save_as(0, "blocked.bsh") == SPREADSHEET_SAVE_ERROR);
    assert(spreadsheet_export_csv(0, "other.csv") == -1);
    assert(write_count == writes && native_begin_count == begins && spreadsheet_editing());
    complete(BOS_E_IO);
    char retry_name[FS_NAME_LEN] = "unchanged";
    assert(!spreadsheet_export_retry_name(retry_name, sizeof retry_name) && !strcmp(retry_name, "unchanged"));
    assert(state.export_retry.valid && spreadsheet_file() == source && document_revision_equal(state.saved_revision, durable));
    assert(spreadsheet_export_csv(0, "values.csv") == -1 && write_count == writes && spreadsheet_editing());
    key(0x01, 0); assert(!spreadsheet_dirty());
    assert(!spreadsheet_export_retry_name(retry_name, 2) && !strcmp(retry_name, "unchanged"));
    assert(!spreadsheet_export_retry_name(NULL, sizeof retry_name));
    assert(spreadsheet_export_retry_name(retry_name, sizeof retry_name) && !strcmp(retry_name, "values.csv"));
    assert(spreadsheet_save() == SPREADSHEET_SAVE_PENDING); complete(BOS_OK);
    assert(state.export_retry.valid && !spreadsheet_dirty());
    writes = write_count;
    assert(spreadsheet_export_csv(0, "values.csv") == SPREADSHEET_EXPORT_PENDING);
    assert(write_count == writes && save_info().handle != export.handle && save_info().target.file == csv);
    complete(BOS_OK);
    assert(!state.export_retry.valid && !spreadsheet_dirty() && spreadsheet_file() == source);
    assert(!spreadsheet_export_retry_name(retry_name, sizeof retry_name));
    assert(strstr(spreadsheet_status(), "Formula-like"));
    assert(spreadsheet_export_csv(0, "values.csv") == -1);
    /* A new export failure before installing complete bytes retains old retry. */
    native_begin_result = BOS_E_CAPACITY;
    assert(spreadsheet_export_csv(0, "retry.csv") == -1);
    SpreadsheetExportRetry retained = state.export_retry; assert(retained.valid);
    create_failure = 1; assert(spreadsheet_export_csv(0, "failed-new.csv") == -1); create_failure = 0;
    assert(!memcmp(&state.export_retry, &retained, sizeof retained));
    write_failure = 1; assert(spreadsheet_export_csv(0, "failed-new.csv") == -1); write_failure = 0;
    assert(!memcmp(&state.export_retry, &retained, sizeof retained) && fs_find_child(0, "failed-new.csv") < 0);
    native_begin_result = BOS_OK;
    enter("B1", "changed"); writes = write_count;
    assert(!spreadsheet_export_retry_name(retry_name, sizeof retry_name));
    assert(spreadsheet_export_csv(0, "retry.csv") == -1 && write_count == writes);
    key(0x2c, SPREADSHEET_MOD_CTRL);
    /* A parent node reuse or target same-byte replacement invalidates retry. */
    assert(spreadsheet_export_retry_name(retry_name, sizeof retry_name) && !strcmp(retry_name, "retry.csv"));
    unsigned parent_identity = files[0].identity; files[0].identity = ++next_identity;
    assert(!spreadsheet_export_retry_name(retry_name, sizeof retry_name));
    assert(spreadsheet_export_csv(0, "retry.csv") == -1 && write_count == writes);
    files[0].identity = parent_identity;
    csv = fs_find_child(0, "retry.csv");
    unsigned length = (unsigned)fs_size(csv); memcpy(encoded, fs_data(csv), length);
    assert(fs_write(csv, (const char *)encoded, (int)length) == (int)length); writes = write_count;
    assert(spreadsheet_export_csv(0, "retry.csv") == -1 && write_count == writes);
    assert(!spreadsheet_export_retry_name(retry_name, sizeof retry_name));
    assert(!sync_count);
    puts("Spreadsheet async exports: isolated dirty state, sync-only retry, pending editor/version/parent checks and retained old retry passed");
}
static void test_empty_export_and_lifecycle(void) {
    fixture_reset(); native_begin_result = BOS_E_PROTECTED;
    assert(spreadsheet_export_csv(0, "empty.csv") == -1);
    int empty = fs_find_child(0, "empty.csv"); assert(empty > 0 && fs_size(empty) == 0 && state.export_retry.valid);
    assert(!spreadsheet_dirty()); native_begin_result = BOS_OK;
    int writes = write_count;
    assert(spreadsheet_export_csv(0, "empty.csv") == SPREADSHEET_EXPORT_PENDING && write_count == writes);
    complete(BOS_E_IO); assert(fs_valid(empty) && !fs_size(empty) && state.export_retry.valid);
    assert(spreadsheet_export_csv(0, "empty.csv") == SPREADSHEET_EXPORT_PENDING && write_count == writes);
    complete(BOS_OK); assert(fs_valid(empty) && !fs_size(empty) && !spreadsheet_dirty());
    enter("A1", "7"); assert(spreadsheet_save_as(0, "source.bsh") == SPREADSHEET_SAVE_PENDING);
    DocumentSave pending = save_info(); unsigned releases = native_release_count;
    assert(!spreadsheet_open_file(-1)); assert(save_info().handle == pending.handle && native_release_count == releases);
    unsigned length; const unsigned char *data = spreadsheet_snapshot(&length);
    assert(data); memcpy(encoded, data, length);
    assert(!spreadsheet_restore(encoded, length - 1u, -1, 0, 1, 0, 0));
    assert(save_info().handle == pending.handle && native_release_count == releases);
    spreadsheet_new(); assert(!save_info().owner && !native_handle && native_release_count == releases + 1);
    assert(storage_busy && !spreadsheet_dirty());
    /* Detaching interest did not end the global snapshot lease. */
    assert(spreadsheet_save_as(0, "next.bsh") == SPREADSHEET_SAVE_ERROR);
    storage_busy = 0;
    assert(spreadsheet_restore(encoded, length, -1, 0, 1, 0, 0));
    assert(spreadsheet_save_as(0, "next.bsh") == SPREADSHEET_SAVE_PENDING);
    assert(save_info().owner != pending.owner && save_info().handle != pending.handle);
    pending = save_info();
    assert(spreadsheet_open_file(pending.target.file));
    assert(!save_info().owner && !native_handle && storage_busy);
    storage_busy = 0;
    assert(spreadsheet_save() == SPREADSHEET_SAVE_PENDING); pending = save_info();
    assert(spreadsheet_restore(encoded, length, -1, 0, 1, 0, 0));
    assert(!save_info().owner && !native_handle && storage_busy && spreadsheet_dirty());
    storage_busy = 0;
    assert(spreadsheet_save_as(0, "close.bsh") == SPREADSHEET_SAVE_PENDING); spreadsheet_close();
    assert(!save_info().owner && !native_handle && storage_busy);
    storage_busy = 0;
    puts("Spreadsheet async lifecycle: zero-byte CSV retention, invalid transition preservation, detach without cancel and fresh owner passed");
}
static void test_document_bounds(void) {
    fixture_reset();
    assert(spreadsheet_save_as(0, "blank.bsh") == SPREADSHEET_SAVE_PENDING && spreadsheet_dirty());
    int blank = spreadsheet_file(); assert(fs_size(blank) == SHEET_NATIVE_HEADER_SIZE);
    complete(BOS_OK); assert(!spreadsheet_dirty());
    sheet_init(&fixture); char maximum[SHEET_TEXT_MAX + 1u];
    memset(maximum, '"', SHEET_TEXT_MAX); maximum[SHEET_TEXT_MAX] = 0;
    for (unsigned row = 0; row < SHEET_ROWS; row++) for (unsigned col = 0; col < SHEET_COLS; col++) {
        assert(!sheet_set(&fixture, row, col, SHEET_TEXT, maximum, SHEET_TEXT_MAX));
        assert(!sheet_set_format(&fixture, row, col, SHEET_FORMAT_CURRENCY));
    }
    for (unsigned col = 0; col < SHEET_COLS; col++) assert(!sheet_set_column_width(&fixture, col, SHEET_COLUMN_WIDTH_MAX));
    assert(!sheet_recalculate(&fixture));
    unsigned length; assert(!sheet_native_encode(&fixture, encoded, sizeof encoded, &length));
    assert(length == SHEET_NATIVE_MAX_SIZE);
    int input = make_file("maximum.bsh", encoded, length); assert(spreadsheet_open_file(input));
    assert(spreadsheet_save_as(0, "maximum-copy.bsh") == SPREADSHEET_SAVE_PENDING);
    int output = spreadsheet_file(); assert(fs_size(output) == (int)length && !memcmp(encoded, fs_data(output), length));
    key(0x05, SPREADSHEET_MOD_CTRL); complete(BOS_OK); assert(spreadsheet_dirty());
    key(0x2c, SPREADSHEET_MOD_CTRL); assert(!spreadsheet_dirty());
    assert(spreadsheet_export_csv(0, "maximum.csv") == SPREADSHEET_EXPORT_PENDING);
    int csv = fs_find_child(0, "maximum.csv"); assert(fs_size(csv) == SHEET_CSV_MAX_SIZE);
    assert(!sheet_csv_import(&decoded, (const unsigned char *)fs_data(csv), (unsigned)fs_size(csv)));
    for (unsigned i = 0; i < SHEET_CELLS; i++) {
        assert(decoded.cells[i].kind == SHEET_TEXT && decoded.cells[i].length == SHEET_TEXT_MAX);
        assert(!memcmp(decoded.cells[i].text, maximum, SHEET_TEXT_MAX));
    }
    assert(!spreadsheet_dirty()); complete(BOS_OK); assert(!spreadsheet_dirty());
    puts("Spreadsheet async bounds: empty native and maximum native/quoted CSV complete byte counts passed");
}
static void test_revision_boundaries(void) {
    fixture_reset(); enter("A1", "1");
    assert(spreadsheet_save_as(0, "revision.bsh") == SPREADSHEET_SAVE_PENDING); complete(BOS_OK);
    DocumentRevision saved = state.saved_revision;
    state.next_revision = (DocumentRevision){7u, ~0u};
    enter("A1", "2"); assert(snapshot()->revision.epoch == 8u && snapshot()->revision.counter == 1u);
    assert(!document_revision_equal(snapshot()->revision, saved) && spreadsheet_dirty());
    assert(spreadsheet_save() == SPREADSHEET_SAVE_PENDING); DocumentSave pending = save_info();
    enter("A1", "3"); complete(BOS_OK); assert(spreadsheet_dirty());
    key(0x2c, SPREADSHEET_MOD_CTRL); assert(!spreadsheet_dirty() && document_revision_equal(snapshot()->revision, pending.revision));
    /* Terminal exhaustion rejects acceptance before touching history/editor. */
    type("4"); state.next_revision = (DocumentRevision){~0u, ~0u};
    unsigned count = state.count, current = state.current, first = state.first;
    DocumentRevision revision = snapshot()->revision;
    prior = *document(); unsigned caret = state.edit_caret, anchor = state.edit_anchor;
    int writes = write_count;
    key(0x1c, 0);
    assert(spreadsheet_editing() && state.edit_changed && !strcmp(state.edit, "4"));
    assert(state.edit_caret == caret && state.edit_anchor == anchor);
    assert(state.first == first && state.count == count && state.current == current);
    compare_docs(&prior, document()); assert(document_revision_equal(snapshot()->revision, revision));
    assert(spreadsheet_save() == SPREADSHEET_SAVE_ERROR && write_count == writes && spreadsheet_editing());
    assert(spreadsheet_export_csv(0, "exhausted.csv") == -1 && fs_find_child(0, "exhausted.csv") < 0);
    key(0x03, SPREADSHEET_MOD_CTRL); key(0x0d, SPREADSHEET_MOD_CTRL);
    assert(spreadsheet_editing() && state.count == count && state.current == current); compare_docs(&prior, document());
    DocumentSave before = save_info();
    spreadsheet_new(); assert(spreadsheet_editing() && save_info().owner == before.owner); compare_docs(&prior, document());
    int source = spreadsheet_file();
    assert(!spreadsheet_open_file(source));
    unsigned size; const unsigned char *bytes = spreadsheet_snapshot(&size); memcpy(encoded, bytes, size);
    assert(!spreadsheet_restore(encoded, size, source, fs_identity(source), 0, 0, 0));
    assert(spreadsheet_editing() && state.count == count && state.current == current); compare_docs(&prior, document());
    key(0x01, 0);
    key(0x53, 0); compare_docs(&prior, document());
    spreadsheet_clipboard_set("9", 1); key(0x2f, SPREADSHEET_MOD_CTRL); compare_docs(&prior, document());
    assert(state.count == count && state.current == current && state.first == first);
    /* Two-step formatting cannot use the final identity for only its edit. */
    state.next_revision = (DocumentRevision){~0u, ~0u - 1u}; type("5");
    key(0x03, SPREADSHEET_MOD_CTRL);
    assert(spreadsheet_editing() && state.next_revision.counter == ~0u - 1u); compare_docs(&prior, document());
    key(0x1c, 0); assert(!spreadsheet_editing());
    assert(snapshot()->revision.epoch == ~0u && snapshot()->revision.counter == ~0u);
    assert(spreadsheet_save() == SPREADSHEET_SAVE_PENDING);
    spreadsheet_close(); assert(!save_info().owner && !native_handle && storage_busy);
    storage_busy = 0;
    puts("Spreadsheet async revisions: epoch rollover, terminal history/edit/lifecycle rejection and compound-action preflight passed");
}
int main(void) {
    files[0].valid = files[0].folder = 1; files[0].identity = files[0].version = 1;
    gfx_init(back, linear, 800, 600, 32, 3200); spreadsheet_init();
    test_native_pending(); test_failures_and_versions(); test_busy_and_capacity();
    test_exports(); test_empty_export_and_lifecycle(); test_document_bounds(); test_revision_boundaries();
    assert(!sync_count && native_begin_count > 20 && native_release_count > 20);
    for (int i = 1; i < FILE_COUNT; i++) if (files[i].valid) free(files[i].data);
    puts("All Spreadsheet async host functional checks passed."); return 0;
}
