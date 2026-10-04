/* Ordinary Writer/model actions through the production value-only save adapter. */
#define main writer_fixture_main
#include "writer_host.c"
#undef main

void writer_unit_revision_counter(unsigned epoch, unsigned counter);
static void complete(int result) {
    storage_busy = 0; async_result = result;
    assert(writer_persistence_poll() == WRITER_CHANGED);
    assert(!async_owner && !async_handle && !writer_persistence_poll());
}
static void reset_files(void) {
    assert(!storage_busy); writer_new();
    for (int i = 1; i < 20; ++i) if (files[i].valid) assert(!fs_delete(i));
    sync_begin_error = sync_failure = payload_failure = 0;
}
static void rejected_requests_preserve_group(void) {
    const int scan[] = {0x1f, 0x12, 0x19};
    const int request[] = {WRITER_REQUEST_SAVE, WRITER_REQUEST_EXPORT, WRITER_REQUEST_PDF};
    for (unsigned i = 0; i < 3; ++i) {
        reset_files(); type("a"); storage_busy = 1;
        assert(writer_key(scan[i], 0, WRITER_MOD_CTRL | (i ? WRITER_MOD_SHIFT : 0)) == request[i]);
        int result = !i ? writer_save() : i == 1 ? writer_export_rtf(0, "busy.rtf") : writer_export_pdf(0, "busy.pdf", 0);
        assert(result == -1); storage_busy = 0; type("b"); key(0x2c, WRITER_MOD_CTRL); check_text("");
    }
    puts("Writer busy requests: rejected Save/RTF/PDF shortcuts preserve the typing undo group passed");
}
static void submitted_revisions(void) {
    reset_files(); type("A"); int syncs = sync_count, writes = write_count;
    assert(writer_save_as(0, "native.bwr") == WRITER_SAVE_PENDING);
    int id = writer_file(); assert(id > 0 && writer_dirty() && sync_count == syncs);
    DocumentSave submitted; writer_save_info(&submitted);
    assert(submitted.pending && submitted.kind == DOCUMENT_SAVE_NATIVE && submitted.owner && submitted.handle);
    unsigned begins = async_begins; type("B");
    assert(writer_save() == WRITER_SAVE_PENDING && async_begins == begins && write_count == writes + 1);
    assert(writer_save_as(0, "second.bwr") == WRITER_SAVE_ERROR && fs_find_child(0, "second.bwr") < 0);
    assert(writer_export_rtf(0, "busy.rtf") == -1 && fs_find_child(0, "busy.rtf") < 0);
    check_text("AB"); complete(BOS_OK); assert(writer_dirty());
    key(0x2c, WRITER_MOD_CTRL); check_text("A"); assert(!writer_dirty());
    key(0x15, WRITER_MOD_CTRL); check_text("AB"); assert(writer_dirty());
    assert(writer_save() == WRITER_SAVE_PENDING);
    key(0x2c, WRITER_MOD_CTRL); check_text("A"); assert(writer_dirty());
    complete(BOS_OK); assert(writer_dirty());
    key(0x15, WRITER_MOD_CTRL); check_text("AB"); assert(!writer_dirty());
    /* Even a clean re-save/Save As must wait for its new durable boundary. */
    assert(writer_save() == WRITER_SAVE_PENDING && writer_dirty()); complete(BOS_OK); assert(!writer_dirty());
    assert(writer_save_as(0, "copy.bwr") == WRITER_SAVE_PENDING && writer_dirty()); complete(BOS_OK); assert(!writer_dirty());
    assert(writer_file() != id && sync_count == syncs);
    puts("Writer async revisions: submitted/current separation, undo/redo, duplicate Save and clean Save As passed");
}
static void model_failures(void) {
    reset_files(); type("RAM only"); sync_begin_error = BOS_E_CAPACITY;
    int syncs = sync_count;
    assert(writer_save_as(0, "retained.bwr") == WRITER_SAVE_ERROR && writer_dirty());
    int id = writer_file(); assert(id >= 0 && fs_size(id) > 0 && !storage_busy && sync_count == syncs);
    WriterBinding binding; assert(writer_binding(&binding) && writer_binding_matches(id, &binding));
    sync_begin_error = BOS_E_PROTECTED;
    assert(writer_save() == WRITER_SAVE_ERROR && writer_dirty() && strstr(writer_status(), "protected"));
    sync_begin_error = 0; assert(writer_save() == WRITER_SAVE_PENDING); complete(BOS_E_IO);
    assert(writer_dirty() && writer_file() == id); assert(writer_save() == WRITER_SAVE_PENDING); complete(BOS_OK); assert(!writer_dirty());
    /* Coordinator success is historical if any later write changed its token. */
    assert(writer_save() == WRITER_SAVE_PENDING); storage_busy = 0; async_result = BOS_OK;
    unsigned n = (unsigned)fs_size(id); unsigned char *bytes = malloc(n); assert(bytes); memcpy(bytes, fs_data(id), n);
    assert(fs_write(id, (const char *)bytes, (int)n) == (int)n); free(bytes);
    assert(writer_persistence_poll() && writer_dirty()); DocumentSave info; writer_save_info(&info); assert(info.result == BOS_E_STALE);
    assert(writer_save() == WRITER_SAVE_PENDING); complete(BOS_OK); assert(!writer_dirty());
    /* Late collection also rejects incompatible native names. */
    assert(writer_save() == WRITER_SAVE_PENDING); strcpy(files[id].name, "renamed.txt"); complete(BOS_OK);
    assert(writer_dirty() && writer_file() < 0); writer_save_info(&info); assert(info.result == BOS_E_STALE);
    puts("Writer async failures: capacity/protection/I/O, safe retry, exact target version and native type passed");
}
static void exports(void) {
    reset_files(); type("Export revision");
    assert(writer_save_as(0, "source.bwr") == WRITER_SAVE_PENDING); complete(BOS_OK); assert(!writer_dirty());
    int source = writer_file(), syncs = sync_count;
    assert(writer_export_rtf(0, "retry.rtf") == WRITER_EXPORT_PENDING);
    assert(!writer_dirty() && writer_file() == source && writer_save() == WRITER_SAVE_ERROR);
    int id = fs_find_child(0, "retry.rtf"), writes = write_count; assert(id >= 0); complete(BOS_E_IO); assert(!writer_dirty());
    char retry_name[FS_NAME_LEN]; unsigned retry_option = 99;
    assert(writer_export_retry_name(DOCUMENT_SAVE_RTF, retry_name, sizeof retry_name, &retry_option));
    assert(!strcmp(retry_name, "retry.rtf") && retry_option == 0);
    assert(!writer_export_retry_name(DOCUMENT_SAVE_PDF, retry_name, sizeof retry_name, &retry_option));
    assert(!writer_export_retry_name(DOCUMENT_SAVE_RTF, retry_name, 4, &retry_option));
    strcpy(files[id].name, "renamed.rtf");
    assert(!writer_export_retry_name(DOCUMENT_SAVE_RTF, retry_name, sizeof retry_name, &retry_option));
    strcpy(files[id].name, "retry.rtf");
    assert(writer_export_rtf(0, "retry.rtf") == WRITER_EXPORT_PENDING && write_count == writes);
    complete(BOS_E_IO);
    /* Failed output creation cannot destroy the previous retry descriptor. */
    payload_failure = 1; assert(writer_export_rtf(0, "failed.rtf") == -1); payload_failure = 0;
    writes = write_count; assert(fs_find_child(0, "failed.rtf") < 0);
    assert(writer_export_rtf(0, "retry.rtf") == WRITER_EXPORT_PENDING && write_count == writes); complete(BOS_OK);
    assert(!writer_dirty() && writer_export_rtf(0, "retry.rtf") == -1);
    assert(writer_export_pdf(0, "retry.pdf", WRITER_PDF_A4) == WRITER_EXPORT_PENDING); complete(BOS_E_IO);
    id = fs_find_child(0, "retry.pdf"); writes = write_count;
    assert(writer_export_pdf(0, "retry.pdf", WRITER_PDF_LETTER) == -1 && write_count == writes);
    type(" newer"); assert(writer_dirty());
    assert(writer_export_pdf(0, "retry.pdf", WRITER_PDF_A4) == -1 && write_count == writes);
    key(0x2c, WRITER_MOD_CTRL); assert(!writer_dirty());
    /* Native Save uses separate state and preserves owned failed-export retry. */
    assert(writer_save() == WRITER_SAVE_PENDING); complete(BOS_OK); writes = write_count;
    assert(writer_export_pdf(0, "retry.pdf", WRITER_PDF_A4) == WRITER_EXPORT_PENDING && write_count == writes);
    type(" private"); complete(BOS_OK); assert(writer_dirty() && writer_file() == source);
    key(0x2c, WRITER_MOD_CTRL); assert(!writer_dirty());
    assert(writer_export_rtf(0, "version.rtf") == WRITER_EXPORT_PENDING); complete(BOS_E_IO);
    id = fs_find_child(0, "version.rtf"); unsigned n = (unsigned)fs_size(id);
    unsigned char *bytes = malloc(n); assert(bytes); memcpy(bytes, fs_data(id), n);
    assert(fs_write(id, (const char *)bytes, (int)n) == (int)n); free(bytes); writes = write_count;
    assert(writer_export_rtf(0, "version.rtf") == -1 && write_count == writes && !writer_dirty());
    assert(sync_count == syncs);
    puts("Writer async exports: RTF/PDF pending, exact retries, native interleave, options/edits and outside writes passed");
}
static void lifetimes(void) {
    reset_files(); type("Old lifetime");
    assert(writer_save_as(0, "old.bwr") == WRITER_SAVE_PENDING);
    DocumentSave old, current; writer_save_info(&old);
    assert(!writer_open_file(-1)); writer_save_info(&current); assert(current.owner == old.owner && current.handle == old.handle);
    assert(!writer_restore((const unsigned char *)"bad", 3, -1, 0, 1, 0, 0));
    writer_save_info(&current); assert(current.owner == old.owner && current.handle == old.handle);
    writer_new(); writer_save_info(&current); assert(!current.owner && !current.handle && storage_busy && !async_owner);
    type("New lifetime"); storage_busy = 0; async_result = BOS_OK;
    assert(!writer_persistence_poll() && writer_dirty());
    assert(writer_save_as(0, "new.bwr") == WRITER_SAVE_PENDING); writer_save_info(&current);
    assert(current.owner != old.owner && current.handle != old.handle); complete(BOS_OK);
    assert(!writer_dirty());
    puts("Writer async lifecycle: rejected replacement preserves interest, accepted New detaches without canceling storage passed");
}
static void unavailable_file_tokens(void) {
    reset_files(); type("Keep private"); assert(writer_save_as(0, "known.bwr") == WRITER_SAVE_PENDING); complete(BOS_OK);
    int id = writer_file(), writes = write_count, creates = create_count;
    version_capacity = 0; assert(writer_save() == WRITER_SAVE_ERROR);
    assert(writer_save_as(0, "no-version.bwr") == WRITER_SAVE_ERROR);
    assert(writer_export_rtf(0, "no-version.rtf") == -1);
    assert(write_count == writes && create_count == creates && !writer_dirty());
    version_capacity = 1; last_create_version = 1;
    assert(writer_save_as(0, "last-token.bwr") == WRITER_SAVE_ERROR);
    assert(fs_find_child(0, "last-token.bwr") < 0 && writer_file() == id && !writer_dirty());
    last_create_version = 0; version_capacity = 1;
    unsigned length; const unsigned char *data = writer_snapshot(&length);
    unsigned char *copy = malloc(length); assert(copy); memcpy(copy, data, length);
    unsigned version = files[id].version; files[id].version = 0;
    assert(writer_open_file(id) && writer_file() < 0 && writer_dirty());
    assert(writer_restore(copy, length, id, fs_identity(id), 0, 0, 0) && writer_file() < 0 && writer_dirty());
    files[id].version = version; unsigned identity = files[id].identity; files[id].identity = 0;
    assert(writer_open_file(id) && writer_file() < 0 && writer_dirty());
    files[id].identity = identity; free(copy);
    puts("Writer file tokens: mutation preflight, last-create rollback and unknown-version/identity binding rejection passed");
}
static void revision_boundaries(void) {
    reset_files(); type("Baseline");
    assert(writer_save_as(0, "boundaries.bwr") == WRITER_SAVE_PENDING); complete(BOS_OK);
    writer_unit_revision_counter(8, ~0u - 1); type("A");
    assert(writer_save() == WRITER_SAVE_PENDING); DocumentSave last; writer_save_info(&last);
    assert(last.revision.epoch == 8 && last.revision.counter == ~0u);
    type("B"); complete(BOS_OK); assert(writer_dirty());
    key(0x2c, WRITER_MOD_CTRL); assert(!writer_dirty()); key(0x15, WRITER_MOD_CTRL); assert(writer_dirty());
    assert(writer_save() == WRITER_SAVE_PENDING); DocumentSave next; writer_save_info(&next);
    assert(next.revision.epoch == 9 && next.revision.counter == 1); complete(BOS_OK); assert(!writer_dirty());
    writer_unit_revision_counter(~0u, ~0u);
    prior = *writer_document(); unsigned caret = writer_caret(), anchor = writer_anchor();
    type("x"); compare_docs(&prior, writer_document()); assert(caret == writer_caret() && anchor == writer_anchor());
    assert(strstr(writer_status(), "exhausted"));
    writer_new(); compare_docs(&prior, writer_document()); assert(!writer_dirty());
    assert(!writer_open_file(writer_file())); compare_docs(&prior, writer_document());
    unsigned length; const unsigned char *data = writer_snapshot(&length);
    assert(!writer_restore(data, length, -1, 0, 1, 0, 0)); compare_docs(&prior, writer_document());
    key(0x30, WRITER_MOD_CTRL); compare_docs(&prior, writer_document());
    search_fields("Baseline", "Replacement");
    caret = writer_caret(); anchor = writer_anchor();
    key(0x1c, WRITER_MOD_CTRL); compare_docs(&prior, writer_document());
    assert(writer_caret() == caret && writer_anchor() == anchor && strstr(writer_status(), "exhausted"));
    key(0x01, 0); select_range(0, 3); unsigned clipboard_before = clipboard_generation;
    key(0x2d, WRITER_MOD_CTRL); compare_docs(&prior, writer_document());
    assert(writer_caret() == 3 && writer_anchor() == 0 && clipboard_generation == clipboard_before);
    puts("Writer revisions: epoch rollover, submitted/history equality and terminal mutation refusal passed");
}
int main(void) {
    files[0].valid = files[0].folder = 1; files[0].identity = 1;
    gfx_init(back, linear, 800, 600, 32, 3200); writer_init(); async_mode = 1;
    rejected_requests_preserve_group(); submitted_revisions(); model_failures(); exports(); lifetimes(); unavailable_file_tokens(); revision_boundaries();
    for (int i = 1; i < 20; ++i) if (files[i].valid) free(files[i].data);
    puts("All Writer async model checks passed."); return 0;
}
