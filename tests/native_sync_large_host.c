/* Ordinary owned sync on the supported large-profile full-volume boundary. */
static int bounded_poll(unsigned sectors);
#define TEST_ATA_ASYNC_POLL(operation, lba, sectors) bounded_poll(sectors)
#define main unused_large_fixture_main
#include "large_volume_host.c"
#undef main
#include "../src/native_sync.c"
unsigned large_test_polls;
static unsigned service_polls;
static int bounded_poll(unsigned sectors) { assert(sectors <= 8); ++service_polls; return 0; }
int main(void) {
    for (unsigned i = 0; i < sizeof bytes; ++i) bytes[i] = (char)(i * 17 + i / 4096);
    high_available = 1; mark();
    assert(!fs_init() && fs_load_disk() == FS_LOAD_BLANK && fs_large_profile());
    assert(native_sync_available() && !fs_empty_dir(0));
    int first = create("first", FS_LARGE_FILE_MAX);
    int second = create("second", 33543168 - FS_LARGE_FILE_MAX);
    while (fs_node_count() < FS_MAX_NODES) {
        char name[24]; snprintf(name, sizeof name, "empty%d", fs_node_count());
        assert(fs_create(0, name) > 0);
    }
    assert(fs_used_bytes() == fs_capacity() && fs_capacity() == 33543168);
    memset(pool_arena, 0xd3, sizeof pool_arena);
    memset(image_arena, 0xe4, sizeof image_arena);
    unsigned a, b, owner_a = BOS_HANDLE_TYPE_PROCESS | 1, owner_b = BOS_HANDLE_TYPE_PROCESS | 2;
    unsigned old_writes = writes, old_reads = reads;
    assert(!native_sync_begin(owner_a, &a) && !native_sync_begin(owner_b, &b));
    assert((unsigned)writes == old_writes && (unsigned)reads == old_reads && !service_polls);
    assert(native_sync_poll(owner_a, a) == BOS_PENDING);
    unsigned steps = 0;
    while (fs_sync_busy()) {
        assert(native_sync_poll(owner_b, b) == BOS_PENDING && fs_needs_sync());
        assert(fs_write(first, fs_data(second), fs_size(second)) == FS_ERR_BUSY);
        unsigned polls_before = service_polls;
        assert(fs_sync_step() != FS_SYNC_IDLE);
        assert(service_polls - polls_before <= 1); native_sync_tick();
        assert(++steps < 150000);
    }
    assert(steps > 40000 && !native_sync_poll(owner_a, a) && !native_sync_poll(owner_b, b));
    assert(!ticket_owned && !sync_results[2].occupied && !fs_needs_sync());
    assert(!fs_load_disk() && fs_large_profile());
    for (unsigned i = 0; i < sizeof pool_arena; ++i) assert(pool_arena[i] == 0xd3);
    for (unsigned i = 0; i < sizeof image_arena; ++i) assert(image_arena[i] == 0xe4);
    verify(first, FS_LARGE_FILE_MAX, 0); verify(second, 33543168 - FS_LARGE_FILE_MAX, 0);
    assert(fs_node_count() == FS_MAX_NODES && fs_used_bytes() == fs_capacity());
    native_sync_owner_release(owner_a); native_sync_owner_release(owner_b);
    printf("native large sync: two owners, full 256-node/33,543,168-byte commit, %u bounded steps, exact reload passed\n", steps);
    return 0;
}
