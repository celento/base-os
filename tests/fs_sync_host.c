/* Ordinary state/transport-error tests only. No memory faults or fuzzing. */
static int transport_begin(int operation, unsigned lba, int sectors);
static int transport_poll(int operation, unsigned lba, unsigned sectors);
static void transport_after(int operation, unsigned lba, void *buffer, unsigned sectors);
#define TEST_ATA_ASYNC_BEGIN(operation, lba, sectors) transport_begin(operation, lba, sectors)
#define TEST_ATA_ASYNC_POLL(operation, lba, sectors) transport_poll(operation, lba, sectors)
#define TEST_ATA_ASYNC_AFTER(operation, lba, buffer, sectors) transport_after(operation, lba, buffer, sectors)
#define main legacy_fs_main
#include "fs_host.c"
#undef main

static struct { int operation; unsigned lba, sectors; } operations[8];
static unsigned operation_count, poll_count, max_sectors, wait_polls;
static unsigned fail_begin, fail_poll_operation, mismatch_operation;
static int trace_enabled;
static int transport_begin(int operation, unsigned lba, int sectors) {
    if (!trace_enabled) return 0;
    assert(operation_count < 8);
    operations[operation_count].operation = operation;
    operations[operation_count].lba = lba;
    operations[operation_count].sectors = sectors;
    ++operation_count;
    return fail_begin == operation_count;
}
static int transport_poll(int operation, unsigned lba, unsigned sectors) {
    (void)operation; (void)lba;
    if (!trace_enabled) return 0;
    ++poll_count;
    if (sectors > max_sectors) max_sectors = sectors;
    assert(sectors <= 8);
    if (wait_polls) { --wait_polls; return 1; }
    return fail_poll_operation == operation_count ? -1 : 0;
}
static void transport_after(int operation, unsigned lba, void *buffer, unsigned sectors) {
    (void)lba;
    if (trace_enabled && operation == 0 && sectors && mismatch_operation == operation_count) {
        /* A successful read reporting different bytes is not durable proof. */
        ((unsigned char *)buffer)[0] ^= 1;
        mismatch_operation = 0;
    }
}
static void begin_trace(void) {
    trace_enabled = 1; operation_count = poll_count = max_sectors = wait_polls = 0;
    fail_begin = fail_poll_operation = mismatch_operation = 0;
    memset(operations, 0, sizeof(operations));
}
static void marked(void) {
    memset(data_disk, 0, sizeof(data_disk));
    unsigned *m = (unsigned *)data_disk;
    m[0] = DATA_MARKER_MAGIC; m[1] = DATA_MARKER_VERSION;
    m[2] = DATA_DISK_SECTORS; m[3] = DATA_SLOT_SECTORS;
    m[4] = DATA_FIRST_LBA; m[5] = DATA_SECOND_LBA; m[6] = crc32(m, 24);
    data_present = 1;
}
static void setup(void) {
    trace_enabled = 0; reset(); marked();
    assert(fs_init() == 0 && fs_load_disk() == FS_LOAD_BLANK);
    assert(fs_empty_dir(0) == 0);
    checkpoint("old snapshot");
}
static void reload(void) {
    trace_enabled = 0; assert(fs_init() == 0 && fs_load_disk() == 0);
}
static void drain(void) {
    unsigned count = 0;
    while (fs_sync_busy()) {
        unsigned before = poll_count;
        assert(fs_sync_step() != FS_SYNC_IDLE);
        assert(poll_count - before <= 1);
        assert(++count < 100000);
    }
    assert(fs_sync_step() == FS_SYNC_IDLE);
}
static unsigned char image_before[FS_IMG_CAPACITY], pool_before[FS_POOL_CAPACITY];
static FsNode nodes_before[FS_MAX_NODES];
static unsigned identities_before[FS_MAX_NODES];
static void assert_busy_lease(int id, int dir) {
    assert(fs_sync_busy() && fs_needs_sync());
    memcpy(image_before, image_arena, sizeof(image_before));
    memcpy(pool_before, pool_arena, sizeof(pool_before));
    memcpy(nodes_before, nodes, sizeof(nodes_before));
    memcpy(identities_before, identities, sizeof(identities_before));
    unsigned used = pool_used, identity = next_identity;
    int active = active_slot; unsigned gen = generation;
    assert(fs_create(0, "new") == FS_ERR_BUSY);
    assert(fs_mkdir(0, "new") == FS_ERR_BUSY);
    assert(fs_create_app(0, "new") == FS_ERR_BUSY);
    assert(fs_write(id, fs_data(id) + 1, fs_size(id) - 1) == FS_ERR_BUSY);
    assert(fs_write(id, "replacement", 11) == FS_ERR_BUSY);
    assert(fs_rename(id, "renamed") == FS_ERR_BUSY);
    assert(fs_move(id, dir) == FS_ERR_BUSY);
    assert(fs_copy(id, dir) == FS_ERR_BUSY);
    assert(fs_copy(dir, 0) == FS_ERR_BUSY);
    assert(fs_delete(id) == FS_ERR_BUSY);
    assert(fs_delete(dir) == FS_ERR_BUSY);
    assert(fs_empty_dir(0) == FS_ERR_BUSY);
    assert(fs_init() == FS_ERR_BUSY);
    assert(fs_load_disk() == FS_ERR_BUSY);
    assert(fs_needs_sync() && used == pool_used && identity == next_identity);
    assert(active_slot == active && generation == gen);
    assert(!memcmp(nodes_before, nodes, sizeof(nodes_before)));
    assert(!memcmp(identities_before, identities, sizeof(identities_before)));
    assert(!memcmp(pool_before, pool_arena, sizeof(pool_before)));
    assert(!memcmp(image_before, image_arena, sizeof(image_before)));
    char text[64]; int list[8];
    assert(fs_read(id, text, sizeof(text)) > 0);
    assert(!memcmp(text, fs_data(id), strlen(text)));
    assert(fs_list(0, list, 8) >= 1 && fs_find_child(0, fs_name(id)) == id);
    assert(fs_sync_busy() && fs_storage_status());
}
static void expected_protocol(unsigned target, unsigned sectors) {
    assert(operation_count == 7 && max_sectors <= 8);
    const int kinds[] = {1, 2, 0, 1, 2, 0, 0};
    const unsigned lbas[] = {target + 1, 0, target + 1, target, 0, target, target + 1};
    const unsigned counts[] = {sectors, 0, sectors, 1, 0, 1, sectors};
    for (unsigned i = 0; i < 7; ++i) {
        assert(operations[i].operation == kinds[i]);
        assert(operations[i].lba == lbas[i] && operations[i].sectors == counts[i]);
    }
}
static void lease_protocol_and_join(void) {
    setup();
    int id = file(), dir = fs_mkdir(0, "Folder"); assert(dir > 0);
    int child = fs_create(dir, "child"); assert(child > 0 && fs_write(child, "child", 5) == 5);
    assert(fs_write(id, "new snapshot", 12) == 12);
    begin_trace();
    FsSyncTicket ticket = {0}, rejected = {77, 88};
    assert(fs_sync_request(&ticket) == 0 && fs_sync_busy());
    assert(!operation_count && sync_job.pos == FS_SECTOR_SIZE);
    assert(fs_sync_request(&rejected) == FS_ERR_BUSY && rejected.incarnation == 77 && rejected.serial == 88);
    assert(fs_sync_result(ticket) == FS_SYNC_PENDING && fs_sync_release(ticket) == FS_ERR_BUSY);
    unsigned seen = 0, loops = 0;
    while (fs_sync_busy()) {
        unsigned phase = sync_job.phase;
        if (!(seen & (1u << phase))) { seen |= 1u << phase; assert_busy_lease(id, dir); }
        assert(fs_sync_result(ticket) == FS_SYNC_PENDING && fs_needs_sync());
        if (phase == SYNC_IO && !wait_polls && !poll_count) {
            wait_polls = 2;
            assert(fs_sync_step() == FS_SYNC_WAIT);
            assert(fs_sync_step() == FS_SYNC_WAIT);
        }
        assert(fs_sync_step() != FS_SYNC_IDLE);
        assert(++loops < 5000);
    }
    assert(fs_sync_result(ticket) == 0 && !fs_needs_sync() && !fs_storage_status());
    expected_protocol(DATA_SECOND_LBA, sync_job.sectors);
    unsigned steps_before = poll_count;
    assert(fs_sync() == 0 && poll_count == steps_before);
    assert(fs_sync_request(&rejected) == FS_ERR_BUSY);
    /* A retained explicit result does not block a new synchronous save. */
    assert(fs_write(id, "third snapshot", 14) == 14);
    trace_enabled = 0;
    assert(fs_sync() == 0 && fs_sync_result(ticket) == 0);
    assert(fs_sync_release(ticket) == 0 && fs_sync_result(ticket) == FS_SYNC_STALE);
    assert(fs_sync_release(ticket) == FS_SYNC_STALE);
    reload(); expect_value("third snapshot");

    /* Join an explicit async owner and preserve that owner's result. */
    assert(fs_write(file(), "joined", 6) == 6);
    assert(fs_sync_request(&ticket) == 0);
    assert(fs_sync_step() == FS_SYNC_MORE);
    assert(fs_sync() == 0 && fs_sync_result(ticket) == 0 && !fs_sync_busy());
    FsSyncTicket old = ticket;
    assert(fs_sync_release(ticket) == 0);
    assert(fs_sync_request(&ticket) == 0 && fs_sync_result(ticket) == 0);
    assert(ticket.serial != old.serial && fs_sync_result(old) == FS_SYNC_STALE);
    assert(fs_init() == 0 && fs_sync_result(ticket) == FS_SYNC_STALE);
    assert(fs_load_disk() == 0); expect_value("joined");
    puts("incremental FS: bounded phases, complete lease, stable reads, exact durability order and retained tickets passed");
}
static void autosave_join_and_errors(void) {
    setup(); assert(fs_write(file(), "autosave", 8) == 8);
    fs_autosync(); assert(fs_sync_busy() && sync_autosave);
    FsSyncTicket old = sync_results[0].ticket;
    assert(fs_sync() == 0 && !fs_sync_busy() && !sync_autosave);
    assert(fs_sync_result(old) == FS_SYNC_STALE);
    assert(fs_write(file(), "next save", 9) == 9 && fs_sync() == 0);
    reload(); expect_value("next save");

    assert(fs_write(file(), "retry", 5) == 5);
    begin_trace(); fail_poll_operation = 1;
    fs_autosync(); old = sync_results[0].ticket;
    assert(fs_sync() == -1 && fs_needs_sync() && !fs_sync_busy());
    assert(fs_sync_result(old) == FS_SYNC_STALE && sync_failures == 1 && writable);
    unsigned calls = operation_count;
    fs_autosync(); assert(!fs_sync_busy() && operation_count == calls);
    trace_enabled = 0; assert(fs_write(file(), "immediate retry", 15) == 15 && fs_sync() == 0);
    reload(); expect_value("immediate retry");

    /* Joining a failed explicit owner retains its failure while a new
     * synchronous retry uses only its own independent completion record. */
    assert(fs_write(file(), "explicit retry", 14) == 14);
    begin_trace(); fail_poll_operation = 2;
    FsSyncTicket explicit_ticket;
    assert(fs_sync_request(&explicit_ticket) == 0 && fs_sync() == -1);
    assert(fs_sync_result(explicit_ticket) == -1);
    trace_enabled = 0;
    assert(fs_sync() == 0 && fs_sync_result(explicit_ticket) == -1);
    assert(fs_sync_release(explicit_ticket) == 0);
    reload(); expect_value("explicit retry");

    /* Every transport boundary fails deterministically without memory faults. */
    for (unsigned operation = 1; operation <= 7; ++operation) {
        setup(); int prior_slot = active_slot; unsigned prior_generation = generation;
        memcpy(saved, disk, sizeof(disk));
        unsigned char old_header[SECTOR_SIZE];
        memcpy(old_header, data_disk + slot_lba(prior_slot) * SECTOR_SIZE, SECTOR_SIZE);
        assert(fs_write(file(), "pending", 7) == 7);
        begin_trace(); fail_poll_operation = operation;
        FsSyncTicket ticket;
        assert(fs_sync_request(&ticket) == 0); drain();
        assert(fs_sync_result(ticket) == -1 && fs_needs_sync());
        assert(active_slot == prior_slot && generation == prior_generation);
        assert(!memcmp(old_header, data_disk + slot_lba(prior_slot) * SECTOR_SIZE, SECTOR_SIZE));
        assert(!memcmp(saved, disk, sizeof(disk)));
        assert(writable == (operation < 4));
        assert(fs_sync_release(ticket) == 0);
        if (operation >= 4) {
            unsigned calls_before = operation_count;
            assert(fs_sync() == -1 && operation_count == calls_before);
        }
        reload(); expect_value(operation <= 4 ? "old snapshot" : "pending");
    }
    /* Submission rejection before header is retryable; a post-header
     * rejection must protect the disk until mount resolves the result. */
    for (unsigned operation = 1; operation <= 7; ++operation) {
        setup(); assert(fs_write(file(), "pending", 7) == 7);
        begin_trace(); fail_begin = operation;
        FsSyncTicket ticket; assert(fs_sync_request(&ticket) == 0); drain();
        assert(fs_sync_result(ticket) == -1 && fs_needs_sync());
        assert(writable == (operation <= 4));
        assert(fs_sync_release(ticket) == 0);
        reload(); expect_value(operation <= 4 ? "old snapshot" : "pending");
    }
    const unsigned mismatches[] = {3, 6, 7};
    for (unsigned i = 0; i < 3; ++i) {
        setup(); assert(fs_write(file(), "readback", 8) == 8);
        begin_trace(); mismatch_operation = mismatches[i];
        FsSyncTicket ticket; assert(fs_sync_request(&ticket) == 0); drain();
        assert(fs_sync_result(ticket) == -1 && fs_needs_sync());
        assert(writable == (mismatches[i] == 3));
        assert(fs_sync_release(ticket) == 0);
        reload(); expect_value(mismatches[i] == 3 ? "old snapshot" : "readback");
    }
    puts("incremental FS: autosave reaping/join/retry and every pre/post-header transport failure passed");
}
static char bytes[FS_FILE_MAX];
static void capacity_and_tokens(void) {
    setup(); trace_enabled = 0; assert(fs_empty_dir(0) == 0);
    for (unsigned i = 0; i < sizeof(bytes); ++i) bytes[i] = (char)(i * 37 + i / 4096);
    int ids[4];
    for (int i = 0; i < 4; ++i) {
        char name[] = "file0"; name[4] += i;
        ids[i] = fs_create(0, name); assert(ids[i] > 0);
    }
    while (fs_node_count() < FS_MAX_NODES) {
        char name[24]; snprintf(name, sizeof(name), "node%d", fs_node_count());
        assert(fs_create(0, name) > 0);
    }
    unsigned capacity = fs_capacity();
    for (int i = 0; i < 4; ++i) {
        unsigned size = i < 3 ? FS_FILE_MAX : capacity - 3 * FS_FILE_MAX;
        assert(fs_write(ids[i], bytes, size) == (int)size);
    }
    assert(fs_used_bytes() == capacity && fs_write(ids[0], bytes, FS_FILE_MAX + 1) == -1);
    FsSyncTicket ticket; assert(fs_sync_request(&ticket) == 0); drain();
    assert(fs_sync_result(ticket) == 0 && fs_sync_release(ticket) == 0);
    reload(); assert(fs_node_count() == FS_MAX_NODES && fs_used_bytes() == capacity);
    for (int i = 0; i < 4; ++i) assert(!memcmp(fs_data(ids[i]), bytes, fs_size(ids[i])));

    /* Exhaustion is a deliberate counter boundary, not ticket reuse. */
    assert(fs_sync_request(&ticket) == 0); FsSyncTicket old = ticket;
    assert(fs_sync_release(ticket) == 0);
    sync_serial = ~0u;
    FsSyncTicket rejected = {123, 456};
    assert(fs_sync_request(&rejected) == -1 && rejected.incarnation == 123 && rejected.serial == 456);
    assert(fs_sync_result(old) == FS_SYNC_STALE);
    assert(fs_init() == 0 && fs_load_disk() == 0);
    assert(fs_sync_request(&ticket) == 0 && ticket.incarnation != old.incarnation);
    assert(fs_sync_release(ticket) == 0);
    unsigned incarnation = sync_incarnation; sync_incarnation = ~0u;
    assert(fs_init() == -1 && fs_load_disk() == -1 && fs_node_count() == FS_MAX_NODES);
    sync_incarnation = incarnation;
    puts("incremental FS: exact full-volume 256-node capacity and non-reused incarnation/serial boundaries passed");
}
int main(void) {
    lease_protocol_and_join(); autosave_join_and_errors(); capacity_and_tokens();
    printf("incremental FS control state: job=%zu, results=%zu, total=%zu bytes\n",
           sizeof(sync_job), sizeof(sync_results), sizeof(sync_job) + sizeof(sync_results) + 3 * sizeof(unsigned));
    return 0;
}
