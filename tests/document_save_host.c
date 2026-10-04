/* Real adapter, native coordinator and existing ordinary in-memory disk fixture. */
#define NATIVE_SYNC_FIXTURE
#include "native_sync_host.c"
#include "../src/kernel_owner.c"
#include "../src/document_save.c"

static DocumentRevision revision = {1, 7};
static void adapter_boundary(void) {
    setup(); DocumentSave a = {0}, b = {0};
    assert(document_save_ensure_owner(&a) && document_save_ensure_owner(&b));
    assert((a.owner & 0xf0000000u) == 0xf0000000u && a.owner != b.owner);
    write_new("adapter boundary");
    unsigned length = (unsigned)fs_size(file());
    assert(document_save_begin(&a, DOCUMENT_SAVE_NATIVE, revision, file(), length, 0) == DOCUMENT_SAVE_PENDING);
    assert(a.pending && a.result == BOS_PENDING && a.handle && !operations && !polls);
    assert(document_save_begin(&b, DOCUMENT_SAVE_PDF, revision, file(), length, 1) == DOCUMENT_SAVE_PENDING);
    assert(a.owner != b.owner && a.handle != b.handle && !operations && !polls);
    assert(!document_save_poll(&a));
    unsigned handle = a.handle, owner_a = a.owner;
    document_save_detach(&a); assert(!a.owner && !a.handle && fs_sync_busy());
    assert(native_sync_poll(owner_a, handle) == BOS_E_STALE && live_records() == 1);
    drain(1); protocol();
    assert(document_save_poll(&b) && !b.pending && b.result == BOS_OK);
    assert(!live_records() && !ticket_owned && !document_save_poll(&b));
    assert(document_revision_equal(b.revision, revision));
    document_save_detach(&b);
    assert(!fs_init() && !fs_load_disk()); expect_value("adapter boundary");
}
static void adapter_stale(void) {
    setup(); DocumentSave save = {0}; write_new("historical");
    assert(document_save_begin(&save, DOCUMENT_SAVE_NATIVE, revision, file(), 10, 0) == DOCUMENT_SAVE_PENDING);
    drain(1); write_new("historical");
    assert(document_save_poll(&save) && save.result == BOS_E_STALE && !save.pending);
    document_save_detach(&save); clear_trace();
    assert(document_save_begin(&save, DOCUMENT_SAVE_NATIVE, revision, file(), 10, 0) == DOCUMENT_SAVE_PENDING);
    drain(1); assert(!fs_init() && !fs_load_disk());
    assert(document_save_poll(&save) && save.result == BOS_E_STALE);
    document_save_detach(&save);
}
static void adapter_failures(void) {
    setup(); DocumentSave save = {0}; write_new("retained RAM");
    writable = 0;
    assert(document_save_begin(&save, DOCUMENT_SAVE_NATIVE, revision, file(), 12, 0) == DOCUMENT_SAVE_ERROR);
    assert(save.result == BOS_E_PROTECTED && !save.pending && !operations && !polls);
    expect_value("retained RAM"); writable = 1; data_write_error = 1;
    assert(document_save_begin(&save, DOCUMENT_SAVE_NATIVE, revision, file(), 12, 0) == DOCUMENT_SAVE_PENDING);
    drain(1); assert(document_save_poll(&save) && save.result == BOS_E_IO && fs_needs_sync());
    assert(!live_records() && fs_writes_allowed()); expect_value("retained RAM");
    data_write_error = 0; clear_trace();
    assert(document_save_begin(&save, DOCUMENT_SAVE_NATIVE, revision, file(), 12, 0) == DOCUMENT_SAVE_PENDING);
    drain(1); assert(document_save_poll(&save) && save.result == BOS_OK);
    document_save_detach(&save); clear_trace();
    unsigned handles[8][4];
    for (unsigned i = 0; i < 8; ++i) for (unsigned j = 0; j < 4; ++j)
        assert(!native_sync_begin(owner(i + 1), &handles[i][j]));
    write_new("capacity RAM");
    assert(document_save_begin(&save, DOCUMENT_SAVE_NATIVE, revision, file(), 12, 0) == DOCUMENT_SAVE_ERROR);
    assert(save.result == BOS_E_CAPACITY && !save.pending && !operations && !polls);
    expect_value("capacity RAM");
    for (unsigned i = 1; i <= 8; ++i) native_sync_owner_release(owner(i));
    assert(document_save_begin(&save, DOCUMENT_SAVE_NATIVE, revision, file(), 12, 0) == DOCUMENT_SAVE_PENDING);
    drain(1); assert(document_save_poll(&save) && save.result == BOS_OK); document_save_detach(&save);
}
static void adapter_floppy(void) {
    reset(); clear_trace(); DocumentSave save = {0};
    int id = fs_create(0, "compat.txt"); assert(id >= 0 && fs_write(id, "floppy", 6) == 6);
    int writes_before = write_calls;
    assert(document_save_begin(&save, DOCUMENT_SAVE_NATIVE, revision, id, 6, 0) == DOCUMENT_SAVE_OK);
    assert(!save.pending && !save.handle && write_calls > writes_before && !operations && !polls);
    assert(fs_write(id, "retained", 8) == 8); write_budget = 0;
    assert(document_save_begin(&save, DOCUMENT_SAVE_NATIVE, revision, id, 8, 0) == DOCUMENT_SAVE_ERROR);
    assert(save.result == BOS_E_IO && !save.pending); document_save_detach(&save); write_budget = -1;
}
static void finite_identities(void) {
    DocumentRevision last = {7, ~0u - 1}, a, b;
    assert(document_revision_next(&last, &a) && a.epoch == 7 && a.counter == ~0u);
    assert(document_revision_next(&last, &b) && b.epoch == 8 && b.counter == 1);
    assert(!document_revision_equal(a, b));
    last = (DocumentRevision){~0u, ~0u}; a = revision;
    assert(!document_revision_next(&last, &a) && document_revision_equal(a, revision));
    kernel_owner_serial = 0x0ffffffeu;
    assert(kernel_owner_allocate() == 0xffffffffu && !kernel_owner_allocate());
    DocumentSave save = {0}; assert(!document_save_ensure_owner(&save) && save.result == BOS_E_CAPACITY);
}
int main(void) {
    adapter_boundary(); adapter_stale(); adapter_failures(); adapter_floppy(); finite_identities();
    puts("document adapter: real FS boundaries, ownership, stale versions, capacity/failure, compatibility and finite identities passed");
    return 0;
}
