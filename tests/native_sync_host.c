/* Ordinary completion ownership and supported backend tests. The existing FS
 * host fixture supplies RAM/disks; its legacy test main is never executed. */
static int trace_begin(int operation, unsigned lba, int sectors);
static int trace_poll(int operation, unsigned lba, unsigned sectors);
#define TEST_ATA_ASYNC_BEGIN(operation, lba, sectors) trace_begin(operation, lba, sectors)
#define TEST_ATA_ASYNC_POLL(operation, lba, sectors) trace_poll(operation, lba, sectors)
#define main unused_legacy_fs_main
#include "fs_host.c"
#undef main
#include "../src/native_sync.c"

static unsigned operations, polls, max_sectors, waits;
static int kinds[8];
static int trace_begin(int operation, unsigned lba, int sectors) {
    (void)lba; (void)sectors;
    assert(operations < 8); kinds[operations++] = operation; return 0;
}
static int trace_poll(int operation, unsigned lba, unsigned sectors) {
    (void)operation; (void)lba; ++polls;
    assert(sectors <= 8); if (sectors > max_sectors) max_sectors = sectors;
    if (waits) { --waits; return 1; }
    return 0;
}
static void clear_trace(void) {
    operations = polls = max_sectors = waits = 0;
    memset(kinds, 0, sizeof kinds);
}
static unsigned owner(unsigned serial) { return BOS_HANDLE_TYPE_PROCESS | serial; }
static unsigned live_records(void) {
    unsigned n = 0;
    for (unsigned i = 0; i < NATIVE_SYNC_CAPACITY; ++i) n += results[i].owner != 0;
    return n;
}
static void setup(void) {
    assert(!fs_sync_busy()); native_sync_tick(); assert(!ticket_owned && !live_records());
    clear_trace(); reset();
    memset(data_disk, 0, sizeof data_disk);
    unsigned *mark = (unsigned *)data_disk;
    mark[0] = DATA_MARKER_MAGIC; mark[1] = DATA_MARKER_VERSION;
    mark[2] = DATA_DISK_SECTORS; mark[3] = DATA_SLOT_SECTORS;
    mark[4] = DATA_FIRST_LBA; mark[5] = DATA_SECOND_LBA; mark[6] = crc32(mark, 24);
    data_present = 1;
    assert(!fs_init() && fs_load_disk() == FS_LOAD_BLANK);
    assert(!fs_empty_dir(0)); checkpoint("old snapshot"); clear_trace();
    assert(native_sync_available() && fs_writes_allowed());
    assert(native_sync_capacity() == 32 && native_sync_per_owner_limit() == 4);
}
static void write_new(const char *text) {
    assert(fs_write(file(), text, (int)strlen(text)) == (int)strlen(text));
}
static void drain(int service) {
    unsigned steps = 0;
    while (fs_sync_busy()) {
        unsigned before = polls;
        assert(fs_sync_step() != FS_SYNC_IDLE);
        assert(polls - before <= 1);
        if (service) native_sync_tick();
        assert(++steps < 100000);
    }
    assert(fs_sync_step() == FS_SYNC_IDLE);
    if (service) native_sync_tick();
}
static void protocol(void) {
    const int expected[] = {1, 2, 0, 1, 2, 0, 0};
    assert(operations == 7 && max_sectors <= 8);
    for (unsigned i = 0; i < 7; ++i) assert(kinds[i] == expected[i]);
}
static void two_clients_and_boundary(void) {
    setup(); write_new("accepted boundary");
    unsigned a, b, c, serial = sync_serial, incarnation = fs_incarnation();
    assert(!native_sync_begin(owner(1), &a));
    assert(!operations && !polls && sync_job.phase == SYNC_SERIALIZE_NODE);
    assert(sync_job.pos == SECTOR_SIZE && sync_job.node == 0);
    assert(!native_sync_begin(owner(2), &b));
    assert(!native_sync_begin(owner(1), &c));
    assert(a != b && a != c && b != c && sync_serial == serial + 1);
    assert(native_sync_poll(owner(1), a) == BOS_PENDING);
    assert(native_sync_poll(owner(2), a) == BOS_E_STALE);
    assert(native_sync_release(owner(2), a) == BOS_E_STALE);
    assert(fs_write(file(), "later bytes", 11) == FS_ERR_BUSY);
    assert(fs_init() == FS_ERR_BUSY && fs_incarnation() == incarnation);
    assert(!native_sync_release(owner(1), c));
    assert(!operations && !polls); /* begin/poll/release perform metadata only. */
    waits = 2; drain(1); protocol();
    assert(!ticket_owned && !sync_results[2].occupied);
    assert(!native_sync_poll(owner(1), a) && !native_sync_poll(owner(2), b));
    assert(native_sync_poll(owner(1), c) == BOS_E_STALE);
    assert(!fs_needs_sync());
    assert(!fs_init() && !fs_load_disk()); expect_value("accepted boundary");
    /* Detached completed results are historical even after a remount. */
    assert(!native_sync_poll(owner(1), a));
    native_sync_owner_release(owner(1)); native_sync_owner_release(owner(2));
}
static void close_exit_and_reuse(void) {
    setup(); write_new("all owners close"); unsigned a, b, c;
    assert(!native_sync_begin(owner(1), &a) && !native_sync_begin(owner(2), &b));
    assert(!native_sync_release(owner(1), a)); native_sync_owner_release(owner(2));
    assert(!live_records() && ticket_owned && fs_sync_busy());
    assert(native_sync_poll(owner(1), a) == BOS_E_STALE);
    assert(native_sync_poll(owner(2), b) == BOS_E_STALE);
    /* A new process in the same display slot uses a fresh owner key. */
    assert(!native_sync_begin(owner(3), &c));
    assert(native_sync_poll(owner(3), b) == BOS_E_STALE);
    native_sync_owner_release(owner(3)); drain(1);
    assert(!ticket_owned && !sync_results[2].occupied && !fs_needs_sync());
    protocol(); clear_trace(); write_new("autosave after close");
    fs_autosync(); assert(fs_sync_busy()); drain(1); protocol();
    assert(!sync_results[0].occupied);
}
static void join_autosave_and_explicit(void) {
    setup(); write_new("autosave join"); fs_autosync();
    assert(fs_sync_busy() && sync_autosave && sync_results[0].occupied);
    FsSyncTicket auto_ticket = sync_results[0].ticket; unsigned a, b;
    assert(!native_sync_begin(owner(1), &a));
    assert(sync_job.subscribers == 5 && !operations);
    /* Blocking compatibility joins via its independent slot1. */
    assert(!fs_sync()); native_sync_tick(); protocol();
    assert(fs_sync_result(auto_ticket) == FS_SYNC_STALE && !sync_results[1].occupied);
    assert(!native_sync_poll(owner(1), a));
    clear_trace(); write_new("explicit join"); FsSyncTicket explicit;
    assert(!fs_sync_request(&explicit)); assert(!native_sync_begin(owner(2), &b));
    assert(sync_job.subscribers == 5 && !fs_sync()); native_sync_tick(); protocol();
    assert(!fs_sync_result(explicit) && !native_sync_poll(owner(2), b));
    assert(!sync_results[1].occupied && !sync_results[2].occupied);
    /* Existing slot0's retained result is untouched. A native save can use its
     * independent record without stealing it or falsely joining its old result. */
    clear_trace(); write_new("new independent commit"); unsigned c;
    assert(!native_sync_begin(owner(1), &c) && fs_sync_busy());
    assert(native_sync_poll(owner(1), a) == BOS_OK);
    assert(native_sync_poll(owner(1), c) == BOS_PENDING);
    assert(!fs_sync_result(explicit)); drain(1); protocol();
    assert(!fs_sync_result(explicit)); assert(!fs_sync_release(explicit));
    native_sync_owner_release(owner(1)); native_sync_owner_release(owner(2));
}
static void retained_results_do_not_starve(void) {
    setup(); write_new("first version"); unsigned a, b;
    assert(!native_sync_begin(owner(1), &a)); drain(0);
    assert(ticket_owned && sync_results[2].occupied);
    clear_trace(); write_new("second version");
    /* Request reaps the finished old boundary before admitting a new one. */
    assert(!native_sync_begin(owner(2), &b));
    assert(!native_sync_poll(owner(1), a));
    assert(native_sync_poll(owner(2), b) == BOS_PENDING && !operations);
    drain(1); assert(!native_sync_poll(owner(2), b));
    clear_trace(); write_new("third version"); fs_autosync();
    assert(fs_sync_busy() && sync_autosave); drain(1);
    assert(!native_sync_poll(owner(1), a) && !native_sync_poll(owner(2), b));
    assert(!fs_init() && !fs_load_disk()); expect_value("third version");
    native_sync_owner_release(owner(1)); native_sync_owner_release(owner(2));
}
static void old_completion_and_new_autosave(void) {
    setup(); write_new("old completed boundary"); unsigned a, b;
    assert(!native_sync_begin(owner(1), &a)); drain(0);
    assert(ticket_owned && sync_results[2].occupied);
    clear_trace(); write_new("new autosave boundary"); fs_autosync();
    assert(fs_sync_busy() && sync_autosave && sync_job.subscribers == 1);
    /* Old coordinator ticket was completed, even though another commit is now
     * busy. Collect that old outcome, then join exactly the new leased state. */
    assert(!native_sync_begin(owner(2), &b));
    assert(!native_sync_poll(owner(1), a));
    assert(native_sync_poll(owner(2), b) == BOS_PENDING && sync_job.subscribers == 5);
    assert(fs_write(file(), "too late", 8) == FS_ERR_BUSY);
    drain(1); protocol(); assert(!native_sync_poll(owner(2), b));
    assert(!fs_init() && !fs_load_disk()); expect_value("new autosave boundary");
    native_sync_owner_release(owner(1)); native_sync_owner_release(owner(2));
    /* The reserved internal FS subscriber rejects a duplicate owner; it never
     * lends a raw ticket to the coordinator or alters the output on BUSY. */
    FsSyncTicket other; assert(!fs_sync_request_owned(&other)); b = 777;
    assert(native_sync_begin(owner(1), &b) == BOS_E_BUSY && b == 777);
    assert(!ticket_owned && !live_records() && !fs_sync_release(other));
}
static void clean_protected_failure_and_remount(void) {
    setup(); unsigned a, b, out = 0x12345678;
    assert(!native_sync_begin(owner(1), &a) && !native_sync_poll(owner(1), a));
    assert(!operations && !polls && !ticket_owned && !fs_sync_busy());
    assert(!native_sync_release(owner(1), a));
    writable = 0;
    assert(!native_sync_begin(owner(1), &a)); /* Clean known durable snapshot. */
    assert(!native_sync_poll(owner(1), a)); native_sync_owner_release(owner(1));
    write_new("protected RAM");
    assert(native_sync_begin(owner(1), &out) == BOS_E_PROTECTED && out == 0x12345678);
    assert(!operations && !polls && !live_records());
    assert(!fs_init() && !fs_load_disk());
    write_new("failed write"); data_write_error = 1;
    assert(!native_sync_begin(owner(1), &a)); drain(1);
    assert(native_sync_poll(owner(1), a) == BOS_E_IO && fs_needs_sync());
    assert(fs_writes_allowed()); data_write_error = 0; clear_trace();
    assert(!native_sync_begin(owner(2), &b)); drain(1); protocol();
    assert(!native_sync_poll(owner(2), b) && native_sync_poll(owner(1), a) == BOS_E_IO);
    native_sync_owner_release(owner(1)); native_sync_owner_release(owner(2));
    clear_trace(); write_new("header outcome uncertain");
    data_fail_flush_at = data_flush_count + 2;
    assert(!native_sync_begin(owner(1), &a)); drain(1);
    assert(native_sync_poll(owner(1), a) == BOS_E_IO && !fs_writes_allowed());
    assert(native_sync_begin(owner(2), &out) == BOS_E_PROTECTED && out == 0x12345678);
    native_sync_owner_release(owner(1)); data_fail_flush_at = 0;
    assert(!fs_init() && !fs_load_disk());
    clear_trace(); write_new("completed before remount");
    assert(!native_sync_begin(owner(1), &a)); drain(0);
    unsigned incarnation = fs_incarnation();
    assert(!fs_init() && !fs_load_disk() && fs_incarnation() > incarnation);
    native_sync_tick(); assert(native_sync_poll(owner(1), a) == BOS_E_STALE);
    assert(!ticket_owned); native_sync_owner_release(owner(1));
}
static void unsupported_floppy(void) {
    assert(!live_records() && !ticket_owned); clear_trace(); reset();
    int calls = write_calls; unsigned out = 777;
    assert(!native_sync_available());
    assert(native_sync_begin(owner(1), &out) == BOS_E_UNSUPPORTED && out == 777);
    assert(!fs_sync_busy() && write_calls == calls && !live_records());
    FsSyncTicket ticket = {77,88};
    assert(fs_sync_request_owned(&ticket) == FS_SYNC_UNSUPPORTED);
    assert(ticket.incarnation == 77 && ticket.serial == 88 && write_calls == calls);
}
static void capacity_and_exhaustion(void) {
    setup(); unsigned handles[8][4], rejected = 999;
    for (unsigned i = 0; i < 8; ++i) for (unsigned j = 0; j < 4; ++j)
        assert(!native_sync_begin(owner(i + 1), &handles[i][j]));
    assert(live_records() == 32 && !ticket_owned && !operations && !polls);
    assert(native_sync_begin(owner(1), &rejected) == BOS_E_CAPACITY && rejected == 999);
    assert(native_sync_begin(owner(9), &rejected) == BOS_E_CAPACITY && rejected == 999);
    native_sync_owner_release(owner(1));
    assert(!native_sync_begin(owner(9), &rejected) && rejected != handles[0][0]);
    assert(native_sync_poll(owner(9), handles[0][0]) == BOS_E_STALE);
    for (unsigned i = 1; i <= 9; ++i) native_sync_owner_release(owner(i));
    assert(!live_records());
    /* Direct ordinary unit-level counter boundaries: allocation fails closed. */
    sync_serial = ~0u; rejected = 999;
    assert(native_sync_begin(owner(1), &rejected) == BOS_E_CAPACITY && rejected == 999);
    assert(!fs_init() && !fs_load_disk()); next_serial = BOS_HANDLE_SERIAL_MAX - 1;
    assert(!native_sync_begin(owner(1), &rejected));
    assert(rejected == (BOS_HANDLE_TYPE_OPERATION | BOS_HANDLE_SERIAL_MAX));
    assert(!native_sync_release(owner(1), rejected)); rejected = 999;
    assert(native_sync_begin(owner(1), &rejected) == BOS_E_CAPACITY && rejected == 999);
    assert(!live_records() && !ticket_owned);
}
int main(void) {
    two_clients_and_boundary(); close_exit_and_reuse(); join_autosave_and_explicit();
    retained_results_do_not_starve(); old_completion_and_new_autosave();
    clean_protected_failure_and_remount();
    unsupported_floppy(); capacity_and_exhaustion();
    puts("native sync ownership, joins, boundaries, cleanup and bounded progress passed");
    return 0;
}
